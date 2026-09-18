#include "FileManager.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "../core/Config.h"
#include "../core/JobRegistry.h"
#include "../core/Subnet.h"
#include "../storage/PathUtil.h"
#include "FileSink.h"
#include "FileSource.h"

namespace dhcp {
namespace files {

namespace {
const char* TAG = "FileManager";

/** @brief Read window of the volume check (one FAT/SDMMC transfer per chunk). */
constexpr size_t kCheckChunkBytes = 16 * 1024;

/** @brief errno text for the `detail` out-parameter. */
std::string errnoText()
{
    return std::string(strerror(errno));
}

/** @brief Case-insensitive "a before b" comparison of entry names. */
bool nameLess(const std::string& a, const std::string& b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ca = tolower(static_cast<unsigned char>(a[i]));
        const int cb = tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

/**
 * @brief Depth-first delete of @p fullPath (file or directory tree).
 *
 * Used only for an explicitly requested recursive delete: the tree is walked
 * with opendir/readdir and removed bottom-up, so a directory can never be
 * removed before its contents.
 */
bool removeTree(const std::string& fullPath)
{
    struct stat st = {};
    if (stat(fullPath.c_str(), &st) != 0) return false;

    if (!S_ISDIR(st.st_mode)) return unlink(fullPath.c_str()) == 0;

    DIR* dir = opendir(fullPath.c_str());
    if (dir == nullptr) return false;

    bool ok = true;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!removeTree(fullPath + "/" + name)) ok = false;
    }
    closedir(dir);

    return rmdir(fullPath.c_str()) == 0 && ok;
}

/** @brief True when the directory has at least one entry besides `.`/`..`. */
bool isDirEmpty(const std::string& fullPath)
{
    DIR* dir = opendir(fullPath.c_str());
    if (dir == nullptr) return false;   // cannot tell — treat as non-empty

    bool empty = true;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        empty = false;
        break;
    }
    closedir(dir);
    return empty;
}

} // namespace

// `httpStatusFor` and `messageFor` live in `FileStatus.cpp`: the transfer engine
// and the JSON builders need the message table as well, and both are host-tested.

FileManager::FileManager()
{
    checkMutex_ = xSemaphoreCreateMutex();
    if (checkMutex_ == nullptr) {
        ESP_LOGE(TAG, "failed to create the check mutex — volume checks disabled");
    }
    transferMutex_ = xSemaphoreCreateMutex();
    if (transferMutex_ == nullptr) {
        ESP_LOGE(TAG, "failed to create the transfer mutex — transfers disabled");
    }
}

FileManager::~FileManager()
{
    // A running check stops at its next step; the mutex is deliberately not
    // deleted here — the task may be inside it at this very moment, and this
    // runs once, at shutdown.
    checkCancel();
    transferCancel();

    // Volumes unmount themselves (they own their driver state).
    volumes_.clear();
}

uint32_t FileManager::nowMs()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void FileManager::addVolume(std::unique_ptr<storage::IFileSystem> volume)
{
    if (!volume) return;

    for (const auto& existing : volumes_) {
        if (existing->id() == volume->id()) {
            ESP_LOGW(TAG, "volume '%s' is already registered — ignored",
                     volume->id().c_str());
            return;
        }
    }

    ESP_LOGI(TAG, "volume registered: id=%s mountPoint=%s", volume->id().c_str(),
             volume->mountPoint().c_str());
    volumes_.push_back(std::move(volume));
}

// The classic ESP32 has neither the FAT data partition nor a card slot, so the
// whole feature is reported as unsupported there and the UI hides it.
bool FileManager::supported() const
{
#if CONFIG_IDF_TARGET_ESP32P4
    return true;
#else
    return false;
#endif
}

void FileManager::mountAll()
{
    for (auto& vol : volumes_) {
        vol->mount();
    }
    lastRetryMs_ = nowMs();
}

void FileManager::refresh()
{
    const uint32_t now = nowMs();

    // Throttle the whole pass: a missing card must not turn every UI poll into
    // a full SDMMC init attempt (it is not cheap and logs a failure each time).
    if ((uint32_t)(now - lastRetryMs_) < kRetryMs) return;

    // A format owns the volume for as long as it runs: it unmounts, erases and
    // re-creates the filesystem, and this pass — driven by the UI polling
    // /api/status — would walk right into the middle of that (the liveness probe
    // would even unmount the volume under the format). The volumes keep their
    // last reported state until the format is over.
    if (formatting_.load()) return;

    for (auto& vol : volumes_) {
        if (vol->isMounted()) {
            // Mounted is not the same as still there: the volume itself knows
            // how to notice a medium that was pulled out behind the cache (the
            // microSD card asks the card on the bus).
            vol->verify();
        } else {
            vol->mount();
        }
    }
    lastRetryMs_ = now;
}

std::vector<storage::VolumeInfo> FileManager::volumes()
{
    refresh();

    std::vector<storage::VolumeInfo> out;
    out.reserve(volumes_.size());
    for (auto& vol : volumes_) {
        out.push_back(vol->info());
    }
    return out;
}

storage::IFileSystem* FileManager::find(const std::string& volumeId)
{
    refresh();

    for (auto& vol : volumes_) {
        if (vol->id() == volumeId) return vol.get();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────
// Path / volume helpers
// ─────────────────────────────────────────────────────

FileStatus FileManager::resolve(const std::string& volumeId,
                               storage::IFileSystem*& vol)
{
    vol = find(volumeId);
    if (vol == nullptr) return FileStatus::NotFound;
    if (!vol->isMounted()) return FileStatus::NotMounted;

    // A format owns the volume from the moment it starts: it unmounts, erases and
    // mounts it again, so nothing else may touch it meanwhile. Every volume
    // operation comes through here, which makes this the one place that has to
    // know — the request that arrives during a format gets a clear 409 instead of
    // reaching into a filesystem that is being taken apart (transfers run in
    // tasks of their own, so this can really happen now).
    if (formatting_.load()) return FileStatus::Busy;

    return FileStatus::Ok;
}

FileStatus FileManager::absolute(storage::IFileSystem& vol, const std::string& path,
                                 std::string& rel, std::string& full)
{
    // Single enforcement point: every operation goes through the same policy.
    if (!storage::PathUtil::normalize(path, rel)) return FileStatus::InvalidPath;
    full = storage::PathUtil::join(vol.mountPoint(), rel);
    return FileStatus::Ok;
}

// ─────────────────────────────────────────────────────
// Directory listing
// ─────────────────────────────────────────────────────

FileStatus FileManager::scan(const std::string& volumeId, const std::string& path,
                            IDirVisitor& visitor, std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;

    struct stat stDir = {};
    if (::stat(full.c_str(), &stDir) != 0) {
        if (detail) *detail = errnoText();
        return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
    }
    if (!S_ISDIR(stDir.st_mode)) return FileStatus::InvalidPath;

    DIR* dir = opendir(full.c_str());
    if (dir == nullptr) {
        if (detail) *detail = errnoText();
        return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == ".." || name == ".") continue;
        // `<name>.part` is the temporary file of a paused (or abandoned) upload,
        // and that suffix is reserved for it anyway: it is not operator data, so
        // it is neither listed nor offered for open/rename/delete — and a
        // transfer must not copy it either.
        if (storage::PathUtil::isPartName(name)) continue;

        FileEntry e;
        e.name = name;

        struct stat stEnt = {};
        const std::string entPath = full + "/" + name;
        if (::stat(entPath.c_str(), &stEnt) == 0) {
            e.isDir = S_ISDIR(stEnt.st_mode);
            e.size = e.isDir ? 0 : static_cast<uint64_t>(stEnt.st_size);
            e.mtime = static_cast<uint64_t>(stEnt.st_mtime);
        } else {
            ESP_LOGW(TAG, "stat failed for %s", entPath.c_str());
        }

        if (!visitor.visit(e)) break;   // the caller has everything it needs
    }
    closedir(dir);
    return FileStatus::Ok;
}

FileStatus FileManager::list(const std::string& volumeId, const std::string& path,
                            std::vector<FileEntry>& out, std::string* detail)
{
    out.clear();

    // The listing is one use of the raw walk: collect, sort, cap. Sharing the walk
    // with the transfer engine is what keeps the two from ever disagreeing about
    // what a directory contains — the cap here never reaches the engine, which
    // would otherwise delete a source tree it had only partly copied.
    struct CollectVisitor : IDirVisitor {
        CollectVisitor(std::vector<FileEntry>& target, size_t limit)
            : entries(target), cap(limit) {}

        bool visit(const FileEntry& entry) override
        {
            entries.push_back(entry);
            return entries.size() < cap;
        }

        std::vector<FileEntry>& entries;
        size_t cap;
    };

    CollectVisitor visitor(out, kMaxListEntries);
    const FileStatus st = scan(volumeId, path, visitor, detail);
    if (st != FileStatus::Ok) return st;

    // Directories first, then case-insensitive by name.
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return nameLess(a.name, b.name);
    });

    return FileStatus::Ok;
}

FileStatus FileManager::createWriter(const std::string& volumeId,
                                    const std::string& path,
                                    std::unique_ptr<IFileSink>& out,
                                    std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;
    if (rel == "/") return FileStatus::InvalidPath;   // needs a file name

    // Replacing a directory with a file makes no sense — the same rule the
    // upload path applies before it opens its sink.
    struct stat stTarget = {};
    if (::stat(full.c_str(), &stTarget) == 0 && S_ISDIR(stTarget.st_mode)) {
        return FileStatus::AlreadyExists;
    }

    auto sink = std::make_unique<FileSink>(full, 0);
    if (!sink->isOpen()) {
        if (detail) *detail = errnoText();
        return FileStatus::IoError;
    }

    out = std::move(sink);
    return FileStatus::Ok;
}

// ─────────────────────────────────────────────────────
// mkdir / rename / remove / format
// ─────────────────────────────────────────────────────

FileStatus FileManager::mkdir(const std::string& volumeId, const std::string& path,
                              std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;
    if (rel == "/") return FileStatus::AlreadyExists;   // the root always exists

    if (::mkdir(full.c_str(), 0777) != 0) {
        if (detail) *detail = errnoText();
        return (errno == EEXIST) ? FileStatus::AlreadyExists : FileStatus::IoError;
    }
    ESP_LOGI(TAG, "mkdir %s (volume %s)", rel.c_str(), volumeId.c_str());
    return FileStatus::Ok;
}

FileStatus FileManager::rename(const std::string& volumeId, const std::string& from,
                               const std::string& to, std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string relFrom, fullFrom, relTo, fullTo;
    st = absolute(*vol, from, relFrom, fullFrom);
    if (st != FileStatus::Ok) return st;
    st = absolute(*vol, to, relTo, fullTo);
    if (st != FileStatus::Ok) return st;

    // The volume root itself cannot be renamed, and a directory must not be
    // moved inside itself (/a → /a/b would detach the tree).
    if (relFrom == "/" || relTo == "/") return FileStatus::InvalidPath;
    if (relTo.rfind(relFrom + "/", 0) == 0) return FileStatus::InvalidPath;

    struct stat stTo = {};
    if (::stat(fullTo.c_str(), &stTo) == 0) return FileStatus::AlreadyExists;

    if (::rename(fullFrom.c_str(), fullTo.c_str()) != 0) {
        if (detail) *detail = errnoText();
        return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
    }
    ESP_LOGI(TAG, "rename %s -> %s (volume %s)", relFrom.c_str(), relTo.c_str(),
             volumeId.c_str());
    return FileStatus::Ok;
}

FileStatus FileManager::remove(const std::string& volumeId, const std::string& path,
                               bool recursive, std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;
    if (rel == "/") return FileStatus::InvalidPath;   // never the whole volume

    struct stat stPath = {};
    if (::stat(full.c_str(), &stPath) != 0) {
        if (detail) *detail = errnoText();
        return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
    }

    if (S_ISDIR(stPath.st_mode)) {
        if (!recursive) {
            if (!isDirEmpty(full)) return FileStatus::NotEmpty;
            if (rmdir(full.c_str()) != 0) {
                if (detail) *detail = errnoText();
                return FileStatus::IoError;
            }
        } else if (!removeTree(full)) {
            if (detail) *detail = errnoText();
            return FileStatus::IoError;
        }
    } else if (unlink(full.c_str()) != 0) {
        if (detail) *detail = errnoText();
        return FileStatus::IoError;
    }

    ESP_LOGI(TAG, "removed %s (volume %s, recursive=%d)", rel.c_str(),
             volumeId.c_str(), (int)recursive);
    return FileStatus::Ok;
}

FileStatus FileManager::format(const std::string& volumeId, std::string* detail)
{
    // Deliberately **not** `resolve()`: formatting is the operation for a card
    // whose filesystem is unusable — an interrupted format is the classic way to
    // get one — and such a card cannot be mounted, so demanding a mounted volume
    // would leave it with no way back at all (the format needs a mount, the mount
    // needs a filesystem, and only a format could create one). The volume object
    // is enough: the filesystem creates the filesystem while mounting.
    storage::IFileSystem* vol = find(volumeId);
    if (vol == nullptr) return FileStatus::NotFound;

    // Only removable media: the internal FAT partition must never be wiped
    // through the API (it holds cache.dat and is reformatted on demand by its
    // own mount options).
    if (vol->id() != "sd") return FileStatus::Unsupported;

    // One format at a time, and not while a check walks the volume.
    if (formatting_.load()) {
        if (detail) *detail = "a format is already running";
        return FileStatus::Busy;
    }
    if (checkReport().busy) {
        if (detail) *detail = "a volume check is running";
        return FileStatus::Busy;
    }

    // Own the volume for the duration: the periodic refresh pass must not probe
    // or unmount it while the filesystem is being re-created (see refresh()).
    formatting_.store(true);
    const bool ok = vol->format();
    formatting_.store(false);

    if (!ok) {
        if (detail) *detail = vol->lastError();
        return FileStatus::IoError;
    }
    return FileStatus::Ok;
}

FileStatus FileManager::powerCycle(const std::string& volumeId, uint32_t offMs,
                                   std::string* detail)
{
    // Deliberately **not** `resolve()`: this is called while a format owns the
    // volume — that is exactly the situation it exists for — so the busy check
    // that every other operation goes through would refuse it.
    storage::IFileSystem* vol = find(volumeId);
    if (vol == nullptr) return FileStatus::NotFound;

    if (!vol->powerCycle(offMs)) {
        if (detail) *detail = vol->lastError();
        return vol->lastError().empty() ? FileStatus::Unsupported
                                        : FileStatus::IoError;
    }
    return FileStatus::Ok;
}

// ─────────────────────────────────────────────────────
// Streamed transfers (upload / download)
// ─────────────────────────────────────────────────────

FileStatus FileManager::stat(const std::string& volumeId, const std::string& path,
                             FileEntry& out, std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;
    if (rel == "/") return FileStatus::InvalidPath;   // the root has no metadata

    struct stat stEntry = {};
    if (::stat(full.c_str(), &stEntry) != 0) {
        if (detail) *detail = errnoText();
        return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
    }

    out.name = storage::PathUtil::basename(rel);
    out.isDir = S_ISDIR(stEntry.st_mode);
    out.size = out.isDir ? 0 : static_cast<uint64_t>(stEntry.st_size);
    out.mtime = static_cast<uint64_t>(stEntry.st_mtime);
    return FileStatus::Ok;
}


FileStatus FileManager::openRead(const std::string& volumeId,
                                 const std::string& path,
                                 std::unique_ptr<IFileSource>& out,
                                 std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;
    if (rel == "/") return FileStatus::InvalidPath;   // a volume is not a file

    struct stat stFile = {};
    if (::stat(full.c_str(), &stFile) != 0) {
        if (detail) *detail = errnoText();
        return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
    }
    if (S_ISDIR(stFile.st_mode)) return FileStatus::InvalidPath;   // not a file

    auto src = std::make_unique<FileSource>(full);
    if (!src->isOpen()) {
        if (detail) *detail = errnoText();
        return FileStatus::IoError;
    }

    out = std::move(src);
    return FileStatus::Ok;
}

FileStatus FileManager::openWrite(const std::string& volumeId,
                                  const std::string& path,
                                  uint64_t offset, uint64_t totalLen, uint64_t chunkLen,
                                  std::unique_ptr<IFileSink>& out, UploadRange& range,
                                  std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;
    if (rel == "/") return FileStatus::InvalidPath;   // needs a file name

    // Replacing an existing directory with a file makes no sense.
    struct stat stTarget = {};
    if (::stat(full.c_str(), &stTarget) == 0 && S_ISDIR(stTarget.st_mode)) {
        return FileStatus::AlreadyExists;
    }

    // How much of the file is already on the device decides whether this chunk
    // continues the upload or has to start it over. The arithmetic lives in
    // UploadRange; a client that lost track asks uploadOffset for the truth.
    const std::string partPath = full + storage::PathUtil::kUploadPartSuffix;
    uint64_t partSize = 0;
    struct stat stPart = {};
    if (::stat(partPath.c_str(), &stPart) == 0) {
        partSize = static_cast<uint64_t>(stPart.st_size);
    }

    const std::string reason = UploadRange::check(partSize, offset, chunkLen,
                                                  totalLen != 0, totalLen, range);
    if (!reason.empty()) {
        ESP_LOGW(TAG, "upload of %s refused: %s", rel.c_str(), reason.c_str());
        if (detail) *detail = reason;
        return FileStatus::Conflict;
    }

    // Check the space *before* the body is read: a refused upload then costs
    // the client nothing but a header round-trip. A continued upload only needs
    // room for the part that is still missing.
    const auto info = vol->info();
    if (info.mounted &&
        info.freeBytes < range.neededBytes() + kFreeSpaceReserve) {
        ESP_LOGW(TAG, "upload of %llu bytes into %s rejected: %llu free",
                 (unsigned long long)range.neededBytes(), rel.c_str(),
                 (unsigned long long)info.freeBytes);
        if (detail) *detail = "volume is full";
        return FileStatus::NoSpace;
    }

    auto sink = std::make_unique<FileSink>(full, range.offset);
    if (!sink->isOpen()) {
        if (detail) *detail = errnoText();
        return FileStatus::IoError;
    }

    ESP_LOGI(TAG, "upload started: %s (offset %llu of %llu, volume %s)", rel.c_str(),
             (unsigned long long)range.offset, (unsigned long long)range.total,
             volumeId.c_str());
    out = std::move(sink);
    return FileStatus::Ok;
}

FileStatus FileManager::uploadOffset(const std::string& volumeId, const std::string& path,
                                     uint64_t& offset, std::string* detail)
{
    offset = 0;

    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;

    struct stat stPart = {};
    if (::stat((full + storage::PathUtil::kUploadPartSuffix).c_str(), &stPart) != 0) {
        // Nothing was started, or the temporary file is already gone: the client
        // is told to begin at zero, which is also the answer after a reboot.
        if (errno == ENOENT) return FileStatus::Ok;
        if (detail) *detail = errnoText();
        return FileStatus::IoError;
    }

    offset = static_cast<uint64_t>(stPart.st_size);
    return FileStatus::Ok;
}

FileStatus FileManager::discardUpload(const std::string& volumeId, const std::string& path,
                                      std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    std::string rel, full;
    st = absolute(*vol, path, rel, full);
    if (st != FileStatus::Ok) return st;

    if (::unlink((full + storage::PathUtil::kUploadPartSuffix).c_str()) != 0 &&
        errno != ENOENT) {
        if (detail) *detail = errnoText();
        return FileStatus::IoError;
    }

    ESP_LOGI(TAG, "discarded the partial upload of %s", rel.c_str());
    return FileStatus::Ok;
}

uint64_t FileManager::freeBytes(const std::string& volumeId)
{
    storage::IFileSystem* vol = find(volumeId);
    if (vol == nullptr || !vol->isMounted()) return 0;
    return vol->info().freeBytes;
}

// ─────────────────────────────────────────────────────
// Volume check (read-only "Check for errors")
//
// The walk reads every file to the end. A directory entry can look healthy
// while its cluster chain is broken, so only fetching the data finds out —
// and that is exactly what an operator wants to know before trusting a card.
// Nothing here writes: FatFs has no fsck, and a diagnostic that could change
// the card on its own would be worse than no diagnostic at all.
// ─────────────────────────────────────────────────────

FileStatus FileManager::checkStart(const std::string& volumeId, std::string* detail)
{
    storage::IFileSystem* vol = nullptr;
    const FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    if (!supported()) return FileStatus::Unsupported;

    // The check reads every file of the volume; a format running at the same
    // time would erase what is being read.
    if (formatting_.load()) {
        if (detail) *detail = "a format is running";
        return FileStatus::Busy;
    }

    bool started = false;
    if (checkMutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
        // One check at a time: two walks would share the SDMMC bus and make
        // both reports meaningless.
        if (!checkState_.busy) {
            checkState_ = CheckReport{};
            checkState_.busy = true;
            checkState_.volume = volumeId;
            checkState_.budgetBytes = kCheckBudgetBytes;
            checkMount_ = vol->mountPoint();
            checkCancel_ = false;
            started = true;
        }
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));
    }

    if (!started) {
        if (detail) *detail = "a check is already running";
        ESP_LOGW(TAG, "check refused: another one is running");
        return FileStatus::Busy;
    }

    const BaseType_t res = xTaskCreate(checkTask, "file_check", 6144, this,
                                       tskIDLE_PRIORITY + 1,
                                       reinterpret_cast<TaskHandle_t*>(&checkTask_));
    if (res != pdTRUE) {
        checkTask_ = nullptr;
        if (checkMutex_ != nullptr) {
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
            checkState_.busy = false;
            checkState_.finished = true;
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));
        }
        if (detail) *detail = "failed to start the check task";
        ESP_LOGE(TAG, "failed to create the check task");
        return FileStatus::IoError;
    }

    ESP_LOGI(TAG, "check started: volume=%s, budget=%d MB", volumeId.c_str(),
             (int)CONFIG_FILES_CHECK_MAX_MB);
    // The walk is the longest operation the device offers, so it announces itself
    // in the registry: the scheduler page shows it and can stop it from there.
    ::dhcp::core::JobRegistry::instance().begin(
        "file_check", "jobs.file_check", volumeId,
        static_cast<uint32_t>(kCheckBudgetBytes));
    return FileStatus::Ok;
}

void FileManager::checkCancel()
{
    if (checkMutex_ == nullptr) return;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
    if (checkState_.busy) {
        checkCancel_ = true;
        ESP_LOGI(TAG, "check cancel requested");
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));

    // Whichever endpoint asked (the Files page has its own cancel route), the
    // scheduler page must show the operation as stopping.
    ::dhcp::core::JobRegistry::instance().requestCancel("file_check");
}

CheckReport FileManager::checkReport()
{
    CheckReport out;
    if (checkMutex_ == nullptr) return out;

    xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
    out = checkState_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));
    return out;
}

bool FileManager::checkStopRequested()
{
    if (checkMutex_ == nullptr) return true;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
    const bool stop = checkCancel_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));
    return stop;
}

void FileManager::checkSetProgress(const std::string& current, uint32_t dirs,
                                   uint32_t files, uint32_t badEntries, uint64_t bytes)
{
    if (checkMutex_ == nullptr) return;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
    checkState_.current = current;
    checkState_.dirs = dirs;
    checkState_.files = files;
    checkState_.badEntries = badEntries;
    checkState_.bytes = bytes;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));

    ::dhcp::core::JobRegistry::instance().progress(
        "file_check", static_cast<uint32_t>(bytes), 0, current);
}

void FileManager::checkFinish(const CheckReport& report)
{
    if (checkMutex_ == nullptr) return;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
    checkState_ = report;
    checkState_.busy = false;
    checkState_.finished = true;
    checkState_.current.clear();
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));

    // Errors *found* are a result, not a failure: the walk did its job. Only a
    // check the operator stopped is reported as cancelled.
    ::dhcp::core::JobRegistry::instance().finish(
        "file_check",
        report.cancelled ? ::dhcp::core::JobState::Cancelled
                         : ::dhcp::core::JobState::Done,
        report.badEntries > 0 ? std::to_string(report.badEntries) + " damaged"
                              : std::string{});
}

void FileManager::checkTask(void* arg)
{
    auto* self = static_cast<FileManager*>(arg);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    std::string mount;
    if (self->checkMutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(self->checkMutex_), portMAX_DELAY);
        mount = self->checkMount_;
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->checkMutex_));
    }

    self->checkWalk(mount);

    // The task deletes itself, so the handle must be cleared before that.
    self->checkTask_ = nullptr;
    vTaskDelete(nullptr);
}

void FileManager::checkWalk(const std::string& mountPoint)
{
    CheckReport report;
    if (checkMutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(checkMutex_), portMAX_DELAY);
        report.volume = checkState_.volume;
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(checkMutex_));
    }
    report.budgetBytes = kCheckBudgetBytes;

    // Explicit stack of directories instead of recursion: every cancel check
    // then sits in one loop, and the depth is bounded by the same policy the
    // rest of the API uses (PathUtil caps paths at kMaxPathLen).
    std::vector<std::string> pending;
    pending.push_back("/");

    uint64_t budgetLeft = kCheckBudgetBytes;
    uint32_t processed = 0;
    bool truncated = false;
    bool cancelled = false;

    const auto noteError = [&report](const std::string& path, const std::string& why) {
        ++report.badEntries;
        if (report.errors.size() < kCheckMaxErrors) {
            report.errors.push_back({path, why});
        }
        ESP_LOGW(TAG, "check: %s — %s", path.c_str(), why.c_str());
    };

    while (!pending.empty()) {
        if (checkStopRequested()) {
            cancelled = true;
            break;
        }

        const std::string dir = pending.back();
        pending.pop_back();
        ++report.dirs;

        DIR* handle = opendir(storage::PathUtil::join(mountPoint, dir).c_str());
        if (handle == nullptr) {
            // An unreadable directory is a finding of its own: the entries
            // below it cannot be checked at all.
            noteError(dir, errnoText());
            continue;
        }

        struct dirent* ent = nullptr;
        while ((ent = readdir(handle)) != nullptr) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;

            const std::string rel = (dir == "/") ? ("/" + name) : (dir + "/" + name);
            const std::string full = storage::PathUtil::join(mountPoint, rel);

            struct stat st = {};
            if (::stat(full.c_str(), &st) != 0) {
                noteError(rel, errnoText());
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                pending.push_back(rel);
                continue;
            }

            if (processed >= kCheckMaxFiles) {
                truncated = true;
                break;
            }
            ++processed;

            const uint64_t size = static_cast<uint64_t>(st.st_size);
            FILE* f = fopen(full.c_str(), "rb");
            if (f == nullptr) {
                noteError(rel, errnoText());
                continue;
            }

            std::vector<char> buffer(kCheckChunkBytes);
            uint64_t read = 0;
            bool budgetHit = false;
            bool ioFail = false;
            std::string failDetail;

            while (read < size) {
                if (budgetLeft == 0) {
                    budgetHit = true;
                    break;
                }
                const uint64_t want = std::min<uint64_t>(
                    std::min<uint64_t>(kCheckChunkBytes, size - read), budgetLeft);
                const size_t got = fread(buffer.data(), 1, static_cast<size_t>(want), f);
                if (got == 0) break;
                read += got;
                budgetLeft -= got;
                report.bytes += got;
            }
            if (ferror(f)) {
                ioFail = true;
                failDetail = errnoText();
            }
            fclose(f);

            checkSetProgress(rel, report.dirs, report.files, report.badEntries,
                             report.bytes);

            if (budgetHit) {
                // The budget is a deliberate stop, not a defect.
                truncated = true;
                break;
            }
            if (ioFail) {
                noteError(rel, failDetail);
                continue;
            }
            if (read != size) {
                // The directory entry promises more bytes than the chain has —
                // a broken chain, which is what a full read is for.
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "size mismatch (%llu in the entry, %llu readable)",
                         (unsigned long long)size, (unsigned long long)read);
                noteError(rel, msg);
                continue;
            }
            ++report.files;
        }
        closedir(handle);

        if (truncated) break;
    }

    report.truncated = truncated;
    report.cancelled = cancelled;
    report.finished = true;
    checkFinish(report);

    ESP_LOGI(TAG, "check %s: volume=%s dirs=%u files=%u bad=%u bytes=%llu%s",
             cancelled ? "cancelled" : "finished", report.volume.c_str(),
             static_cast<unsigned>(report.dirs),
             static_cast<unsigned>(report.files),
             static_cast<unsigned>(report.badEntries),
             static_cast<unsigned long long>(report.bytes),
             truncated ? " (stopped at the limit)" : "");
}

// ─────────────────────────────────────────────────────
// LAN-only access filter
// ─────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────
// Copy / move between volumes
//
// One job, one task, one snapshot — the same shape the volume check above uses,
// for the same reason: copying a directory off a card takes far longer than the
// httpd can wait, and the operator has to be able to watch it and stop it. The
// work itself lives in `TransferEngine`; what is here is the volume side: the
// job lifecycle, the snapshot the REST layer reads and the job-registry entry
// the scheduler page shows.
// ─────────────────────────────────────────────────────

FileStatus FileManager::transferConflicts(const TransferRequest& req,
                                          std::vector<std::string>& names,
                                          std::string* detail)
{
    // Cheap by construction: N `stat()` calls on the selected entries, no walk.
    // The REST handler runs this before it answers, which is what lets it ask
    // "these names are taken, replace them?" without starting anything.
    return TransferEngine::conflicts(*this, req, names, detail);
}

FileStatus FileManager::transferStart(const TransferRequest& req, std::string* detail)
{
    if (!supported()) return FileStatus::Unsupported;

    // Both volumes must be usable *now*: an absent card has to be a clear error
    // here, not a failure discovered in the middle of the job.
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(req.srcVolume, vol);
    if (st != FileStatus::Ok) {
        if (detail) *detail = "the source volume is not available";
        return st;
    }
    st = resolve(req.dstVolume, vol);
    if (st != FileStatus::Ok) {
        if (detail) *detail = "the destination volume is not available";
        return st;
    }

    if (transferMutex_ == nullptr) {
        if (detail) *detail = "the transfer mutex is missing";
        return FileStatus::IoError;
    }

    bool started = false;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(transferMutex_), portMAX_DELAY);
    // `transferTask_` is cleared by the task itself as its very last act, so this
    // also covers the moment between "the report says done" and "the task is
    // really gone" — a new request then cannot overwrite the request the old task
    // is still reading.
    if (!transferState_.busy && transferTask_ == nullptr) {
        transferState_ = TransferReport{};
        transferState_.busy = true;
        transferState_.op = req.op;
        transferState_.srcVolume = req.srcVolume;
        transferState_.dstVolume = req.dstVolume;
        transferState_.dstPath = req.dstPath;
        transferCancel_ = false;
        transferReq_ = req;
        started = true;
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(transferMutex_));

    if (!started) {
        if (detail) *detail = "another transfer is already running";
        return FileStatus::Busy;
    }

    // The engine copies with a 4 KB window on the stack, so the task needs room
    // for that plus the walk's bookkeeping (the same 8 KB the downloads use).
    const BaseType_t res = xTaskCreate(transferTask, "file_transfer", 8192, this,
                                       tskIDLE_PRIORITY + 1,
                                       reinterpret_cast<TaskHandle_t*>(&transferTask_));
    if (res != pdTRUE) {
        transferTask_ = nullptr;
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(transferMutex_), portMAX_DELAY);
        transferState_.busy = false;
        transferState_.finished = true;
        transferState_.phase = TransferPhase::Done;
        transferState_.error = "failed to start the transfer task";
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(transferMutex_));
        if (detail) *detail = "failed to start the transfer task";
        ESP_LOGE(TAG, "failed to create the transfer task");
        return FileStatus::IoError;
    }

    ESP_LOGI(TAG, "transfer started: %s %u entr%s %s:%s -> %s:%s",
             (req.op == TransferOp::Move) ? "move" : "copy",
             static_cast<unsigned>(req.paths.size()),
             req.paths.size() == 1 ? "y" : "ies", req.srcVolume.c_str(),
             req.paths.size() == 1 ? req.paths[0].c_str() : "(batch)",
             req.dstVolume.c_str(), req.dstPath.c_str());

    // The registry entry makes the transfer visible on the scheduler page, where
    // it can be stopped as well; the byte total arrives with the measurement.
    std::string arg = std::to_string(req.paths.size()) + (req.paths.size() == 1 ? " entry -> " : " entries -> ");
    arg += req.dstVolume + req.dstPath;
    ::dhcp::core::JobRegistry::instance().begin("transfer", "jobs.transfer", arg, 0);
    return FileStatus::Ok;
}

void FileManager::transferCancel()
{
    if (transferMutex_ == nullptr) return;

    xSemaphoreTake(static_cast<SemaphoreHandle_t>(transferMutex_), portMAX_DELAY);
    if (transferState_.busy) {
        transferCancel_ = true;
        ESP_LOGI(TAG, "transfer cancel requested");
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(transferMutex_));

    // Whichever endpoint asked (the Files page has its own cancel route), the
    // scheduler page must show the operation as stopping.
    ::dhcp::core::JobRegistry::instance().requestCancel("transfer");
}

bool FileManager::transferStopRequested()
{
    if (transferMutex_ == nullptr) return true;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(transferMutex_), portMAX_DELAY);
    const bool stop = transferCancel_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(transferMutex_));
    return stop;
}

void FileManager::transferReport(TransferReport& out)
{
    if (transferMutex_ == nullptr) {
        out = TransferReport{};
        return;
    }
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(transferMutex_), portMAX_DELAY);
    out = transferState_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(transferMutex_));
}

void FileManager::transferPublish(const TransferReport& report)
{
    if (transferMutex_ == nullptr) return;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(transferMutex_), portMAX_DELAY);
    transferState_ = report;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(transferMutex_));
}

void FileManager::TransferObserver::onTransferProgress(const TransferReport& report)
{
    owner_.transferPublish(report);

    // The scheduler page shows the same numbers. The registry counts in 32-bit
    // steps; a transfer beyond 4 GB simply pins at the maximum, which is honest
    // enough for a bar and keeps one truth for both places.
    const auto clamp = [](uint64_t value) {
        return (value > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
    };
    ::dhcp::core::JobRegistry::instance().progress("transfer", clamp(report.doneBytes),
                                                   clamp(report.totalBytes),
                                                   report.current);
}

bool FileManager::TransferObserver::transferCancelRequested()
{
    return owner_.transferStopRequested();
}

void FileManager::transferTask(void* arg)
{
    auto* self = static_cast<FileManager*>(arg);

    TransferObserver observer(*self);
    TransferReport report;
    TransferEngine::run(*self, self->transferReq_, report, observer);

    // The engine published through the observer on its way, including the final
    // state; the job record is closed here.
    const ::dhcp::core::JobState state = report.cancelled ? ::dhcp::core::JobState::Cancelled
                          : (report.failed > 0) ? ::dhcp::core::JobState::Failed
                                                : ::dhcp::core::JobState::Done;
    std::string summary = std::to_string(report.filesDone) + " file(s)";
    if (report.skipped > 0) summary += ", " + std::to_string(report.skipped) + " skipped";
    if (report.failed > 0) summary += ", " + std::to_string(report.failed) + " failed";
    if (!report.error.empty()) summary += ": " + report.error;
    ::dhcp::core::JobRegistry::instance().finish("transfer", state, summary);

    ESP_LOGI(TAG, "transfer finished: %s (%s)", ::dhcp::core::jobStateText(state), summary.c_str());

    // The task deletes itself, so the handle must be cleared before that.
    self->transferTask_ = nullptr;
    vTaskDelete(nullptr);
}

void FileManager::applyAccessFilter()
{
    const auto fileCfg = ::dhcp::core::Config::instance().getFiles();
    const auto net = ::dhcp::core::Config::instance().getDhcp();

    allowOwnSubnet_ = fileCfg.allowOwnSubnet;

    uint32_t addr = 0;
    uint32_t mask = 0;
    const bool haveAddr = ::dhcp::core::Subnet::parseIp4(net.serverIp, addr);
    const bool haveMask = ::dhcp::core::Subnet::parseIp4(net.subnet, mask);

    subnetValid_ = haveAddr && haveMask && ::dhcp::core::Subnet::isValidMask(mask);
    if (subnetValid_) {
        subnetNet_ = ::dhcp::core::Subnet::network(addr, mask);
        subnetMask_ = mask;
    }

    if (allowOwnSubnet_ && !subnetValid_) {
        ESP_LOGW(TAG, "LAN-only filter enabled but the subnet (%s / %s) is "
                      "unusable — filtering skipped",
                 net.serverIp.c_str(), net.subnet.c_str());
    } else if (allowOwnSubnet_) {
        ESP_LOGI(TAG, "LAN-only filter on: %s/%d",
                 ::dhcp::core::Subnet::toString(subnetNet_).c_str(),
                 ::dhcp::core::Subnet::prefixLength(subnetMask_));
    } else {
        ESP_LOGI(TAG, "LAN-only filter off: file access from any address");
    }
}

bool FileManager::allowClient(uint32_t clientIp4Host)
{
    if (!allowOwnSubnet_) return true;      // filter disabled by the operator
    if (clientIp4Host == 0) {
        ESP_LOGW(TAG, "client address unknown — access granted");
        return true;                        // fail-open: never lock the LAN out
    }
    if (!subnetValid_) return true;         // unusable subnet → filtering skipped

    if (::dhcp::core::Subnet::contains(subnetNet_, subnetMask_, clientIp4Host)) {
        return true;
    }

    ++foreignBlocked_;
    const uint32_t now = nowMs();
    if (foreignBlocked_ == 1 || (uint32_t)(now - lastForeignLogMs_) >= 5000) {
        lastForeignLogMs_ = now;
        ESP_LOGW(TAG, "file request from %s dropped (not in %s/%d; total %u)",
                 ::dhcp::core::Subnet::toString(clientIp4Host).c_str(),
                 ::dhcp::core::Subnet::toString(subnetNet_).c_str(),
                 ::dhcp::core::Subnet::prefixLength(subnetMask_),
                 (unsigned)foreignBlocked_);
    }
    return false;
}

} // namespace files
} // namespace dhcp
