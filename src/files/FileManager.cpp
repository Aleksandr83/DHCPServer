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

int httpStatusFor(FileStatus status)
{
    switch (status) {
        case FileStatus::Ok:            return 200;
        case FileStatus::InvalidPath:   return 400;
        case FileStatus::Unsupported:   return 400;
        case FileStatus::NotFound:      return 404;
        case FileStatus::NotMounted:    return 409;
        case FileStatus::AlreadyExists: return 409;
        case FileStatus::NotEmpty:      return 409;
        case FileStatus::Conflict:      return 409;
        case FileStatus::TooLarge:      return 413;
        case FileStatus::NotText:       return 415;
        case FileStatus::NoSpace:       return 507;
        case FileStatus::IoError:       return 500;
    }
    return 500;
}

const char* messageFor(FileStatus status)
{
    switch (status) {
        case FileStatus::Ok:            return "ok";
        case FileStatus::InvalidPath:   return "invalid path";
        case FileStatus::NotFound:      return "not found";
        case FileStatus::NotMounted:    return "volume is not mounted";
        case FileStatus::AlreadyExists: return "already exists";
        case FileStatus::NotEmpty:      return "directory is not empty";
        case FileStatus::Conflict:      return "file changed on the volume";
        case FileStatus::TooLarge:      return "file is too large for the editor";
        case FileStatus::NotText:       return "file is not a text file";
        case FileStatus::NoSpace:       return "not enough free space on the volume";
        case FileStatus::IoError:       return "filesystem error";
        case FileStatus::Unsupported:   return "operation not supported for this volume";
    }
    return "error";
}

FileManager::FileManager()
{
    checkMutex_ = xSemaphoreCreateMutex();
    if (checkMutex_ == nullptr) {
        ESP_LOGE(TAG, "failed to create the check mutex — volume checks disabled");
    }
}

FileManager::~FileManager()
{
    // A running check stops at its next step; the mutex is deliberately not
    // deleted here — the task may be inside it at this very moment, and this
    // runs once, at shutdown.
    checkCancel();

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

FileStatus FileManager::list(const std::string& volumeId, const std::string& path,
                             std::vector<FileEntry>& out, std::string* detail)
{
    out.clear();

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
        return FileStatus::IoError;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

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

        out.push_back(std::move(e));
        if (out.size() >= kMaxListEntries) break;
    }
    closedir(dir);

    // Directories first, then case-insensitive by name.
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return nameLess(a.name, b.name);
    });

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
    storage::IFileSystem* vol = nullptr;
    FileStatus st = resolve(volumeId, vol);
    if (st != FileStatus::Ok) return st;

    // Only removable media: the internal FAT partition must never be wiped
    // through the API (it holds cache.dat and is reformatted on demand by its
    // own mount options).
    if (vol->id() != "sd") return FileStatus::Unsupported;

    if (!vol->format()) {
        if (detail) *detail = vol->lastError();
        return FileStatus::IoError;
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
                                  const std::string& path, uint64_t expectedLen,
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

    // Replacing an existing directory with a file makes no sense.
    struct stat stTarget = {};
    if (::stat(full.c_str(), &stTarget) == 0 && S_ISDIR(stTarget.st_mode)) {
        return FileStatus::AlreadyExists;
    }

    // Check the space *before* the body is read: a refused upload then costs
    // the client nothing but a header round-trip.
    const auto info = vol->info();
    if (info.mounted &&
        info.freeBytes < expectedLen + kFreeSpaceReserve) {
        ESP_LOGW(TAG, "upload of %llu bytes into %s rejected: %llu free",
                 (unsigned long long)expectedLen, rel.c_str(),
                 (unsigned long long)info.freeBytes);
        if (detail) *detail = "volume is full";
        return FileStatus::NoSpace;
    }

    auto sink = std::make_unique<FileSink>(full);
    if (!sink->isOpen()) {
        if (detail) *detail = errnoText();
        return FileStatus::IoError;
    }

    ESP_LOGI(TAG, "upload started: %s (%llu bytes, volume %s)", rel.c_str(),
             (unsigned long long)expectedLen, volumeId.c_str());
    out = std::move(sink);
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
        return FileStatus::Conflict;
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
