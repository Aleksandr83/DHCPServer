#ifndef DHCP_FILES_IFILEMANAGER_H
#define DHCP_FILES_IFILEMANAGER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "IFileSink.h"
#include "IFileSource.h"
#include "../storage/IFileSystem.h"

namespace dhcp {
namespace files {

/**
 * @brief Outcome of a file-explorer operation.
 *
 * The REST layer maps these 1:1 onto HTTP status codes (@ref httpStatusFor),
 * so handlers never have to parse error strings and the UI can translate the
 * message through its own dictionary.
 */
enum class FileStatus {
    Ok,             ///< 200 — done
    InvalidPath,    ///< 400 — path rejected by PathUtil (escape attempt, bad name)
    NotFound,       ///< 404 — volume id or file/directory does not exist
    NotMounted,     ///< 409 — volume exists but is not mounted (card missing)
    AlreadyExists,  ///< 409 — destination already exists
    NotEmpty,       ///< 409 — directory is not empty and `recursive` was not set
    Conflict,       ///< 409 — the file changed on the volume since it was read
    TooLarge,       ///< 413 — file does not fit the in-memory editor limit
    NotText,        ///< 415 — the content is binary, not editable as text
    NoSpace,        ///< 507 — the volume is too small for the uploaded data
    IoError,        ///< 500 — the filesystem call failed (see detail)
    Unsupported     ///< 400 — operation not offered for this volume (e.g. format FAT)
};

/** @brief HTTP status code for a @ref FileStatus. */
int httpStatusFor(FileStatus status);

/** @brief Human-readable (English) message for a @ref FileStatus. */
const char* messageFor(FileStatus status);

/** @brief One directory entry of `GET /api/files/list`. */
struct FileEntry {
    std::string name;      ///< Entry name only (no path)
    bool isDir = false;    ///< Directory (vs regular file)
    uint64_t size = 0;     ///< Size in bytes (0 for directories)
    uint64_t mtime = 0;    ///< Last modification time (Unix seconds, 0 = unknown)
};

/** @brief One entry a volume check could not read. */
struct CheckError {
    std::string path;      ///< Volume-relative path of the entry
    std::string detail;    ///< Why it failed (errno text, size mismatch, …)
};

/**
 * @brief Snapshot of a volume check (see @ref IFileManager::checkStart).
 *
 * The check walks the whole volume and **reads every file to the end**, which
 * is the only way to find out that a file cannot be read any more: a directory
 * entry can look perfectly healthy while its cluster chain is broken, and the
 * failure only shows up when the data is actually fetched. The walk is
 * deliberately **read-only** — no `f_getfree()`, no chain repair — because
 * FatFs has no fsck and a diagnostic must never be able to change the card on
 * its own.
 *
 * A check is bounded by a byte budget (Kconfig `FILES_CHECK_MAX_MB`) and a file
 * count, so a 32 GB card cannot turn one click into a half-hour job: hitting
 * either limit ends the walk with @ref truncated set. The report is a
 * snapshot — the REST layer serialises it while the job keeps running, which
 * is what lets the UI show progress without a second endpoint per counter.
 */
struct CheckReport {
    bool busy = false;      ///< A check is running right now
    bool finished = false;  ///< The last check ended (normally, at a cap or cancelled)
    bool truncated = false; ///< Stopped at a cap — not everything was read
    bool cancelled = false; ///< Stopped because @ref IFileManager::checkCancel was called
    std::string volume;     ///< Volume id the report belongs to
    std::string current;    ///< Path being read (empty when idle)
    uint32_t dirs = 0;      ///< Directories walked
    uint32_t files = 0;     ///< Files read completely
    uint32_t badEntries = 0;///< Entries that could NOT be read (files and directories)
    uint64_t bytes = 0;     ///< Bytes actually read so far
    uint64_t budgetBytes = 0;   ///< Byte budget of this check (0 = unlimited)
    std::vector<CheckError> errors;   ///< The failures (bounded by @ref IFileManager::kCheckMaxErrors)
};

/**
 * @brief Facade over the mounted FAT volumes, used by the REST API and the UI.
 *
 * The manager owns the volumes (`storage::IFileSystem`) and implements the
 * whole explorer API on top of them, so the REST layer never touches a
 * `IFileSystem` directly:
 *
 *  - @ref volumes / @ref mountAll / @ref refresh — volume lifecycle (mounting
 *    at boot, throttled retry for the microSD card that may be inserted later),
 *  - @ref list / @ref mkdir / @ref rename / @ref remove — directory browsing
 *    and structural operations,
 *  - @ref format — erase an external card (never the internal FAT).
 *
 * Every path passed in is **validated and normalized by `storage::PathUtil`
 * inside the manager** (single enforcement point, the REST handlers stay
 * thin): paths are volume-relative, `..` and illegal characters are rejected.
 */
class IFileManager {
public:
    /** @brief Maximum number of entries returned by @ref list. */
    static constexpr size_t kMaxListEntries = 512;

    virtual ~IFileManager() = default;

    /**
     * @brief True when this build/board offers the file explorer at all.
     *
     * False on the classic ESP32: neither the FAT data partition nor a card
     * slot exists there, so the web UI hides the "Files" menu entry.
     */
    virtual bool supported() const = 0;

    /** @brief Mount every registered volume (called once at boot). */
    virtual void mountAll() = 0;

    /**
     * @brief Throttled retry of the volumes that are not mounted yet, plus a
     * liveness check of the mounted ones.
     *
     * Called from the REST handlers, so an inserted card appears within a few
     * seconds without a reboot, and a *removed* one disappears just as quickly:
     * a mounted volume is asked whether its medium is still there (see
     * `IFileSystem::verify`), because neither FatFS nor the driver notices a
     * card being pulled out. The implementation throttles the whole pass to one
     * every few seconds.
     */
    virtual void refresh() = 0;

    /** @brief Current state of every registered volume. */
    virtual std::vector<storage::VolumeInfo> volumes() = 0;

    /**
     * @brief Look up a volume by its API id.
     * @return nullptr when the id is unknown (→ `404` in the REST layer).
     */
    virtual storage::IFileSystem* find(const std::string& volumeId) = 0;

    /**
     * @brief List a directory.
     *
     * Directories come first, then files; both sorted case-insensitively by
     * name. At most @ref kMaxListEntries entries are returned (the REST layer
     * reports a `truncated` flag) to keep the response bounded.
     *
     * @param[in]  volumeId Volume id (`fat`, `sd`).
     * @param[in]  path     Volume-relative directory path (`/` = root).
     * @param[out] out      Entries; cleared on failure.
     * @param[out] detail   Optional technical detail (errno text).
     */
    virtual FileStatus list(const std::string& volumeId, const std::string& path,
                            std::vector<FileEntry>& out,
                            std::string* detail = nullptr) = 0;

    /** @brief Create a directory. @p path is the directory to create. */
    virtual FileStatus mkdir(const std::string& volumeId, const std::string& path,
                             std::string* detail = nullptr) = 0;

    /** @brief Rename/move @p from to @p to (both volume-relative). */
    virtual FileStatus rename(const std::string& volumeId, const std::string& from,
                              const std::string& to,
                              std::string* detail = nullptr) = 0;

    /**
     * @brief Delete a file, or a directory.
     * @param[in] recursive Delete a non-empty directory with its contents.
     */
    virtual FileStatus remove(const std::string& volumeId, const std::string& path,
                              bool recursive = false,
                              std::string* detail = nullptr) = 0;

    /**
     * @brief Format a volume (destroys all data on it).
     *
     * Only external media support this — the internal FAT partition is not a
     * removable medium and reports @ref FileStatus::Unsupported.
     */
    virtual FileStatus format(const std::string& volumeId,
                              std::string* detail = nullptr) = 0;

    /**
     * @brief Metadata of a single entry (no need to list the parent).
     *
     * `name` is the last segment, `mtime` the Unix seconds of the last write —
     * the value the editor echoes back on save to detect a file that changed
     * underneath it.
     */
    virtual FileStatus stat(const std::string& volumeId, const std::string& path,
                            FileEntry& out, std::string* detail = nullptr) = 0;

    /**
     * @brief Open a regular file for a streamed download.
     *
     * @param[out] out Source to pull byte windows from; untouched on failure.
     */
    virtual FileStatus openRead(const std::string& volumeId,
                                const std::string& path,
                                std::unique_ptr<IFileSource>& out,
                                std::string* detail = nullptr) = 0;

    /**
     * @brief Open a file for a streamed upload.
     *
     * Performs the checks that must happen *before* the HTTP body is read:
     * the volume is mounted, the path is a valid file path, magic/`.part`-like
     * names inside the volume are accepted, and the free space is at least
     * @p expectedLen plus a small reserve — otherwise
     * @ref FileStatus::NoSpace is returned and the caller can answer `507`
     * without wasting the transfer.
     *
     * An existing file with the same name is replaced on @ref IFileSink::commit.
     *
     * @param[in]  expectedLen `Content-Length` of the upload (0 = empty file).
     * @param[out] out         Sink to push byte windows into; untouched on failure.
     */
    virtual FileStatus openWrite(const std::string& volumeId,
                                 const std::string& path, uint64_t expectedLen,
                                 std::unique_ptr<IFileSink>& out,
                                 std::string* detail = nullptr) = 0;

    /** @brief Free bytes of a mounted volume (0 when it is not mounted). */
    virtual uint64_t freeBytes(const std::string& volumeId) = 0;

    /**
     * @brief Start a read-only check of a volume ("Check for errors").
     *
     * The walk runs in its own task: reading a card takes minutes, and the
     * httpd handler that asked for it must answer immediately. Poll
     * @ref checkReport for progress and the result, @ref checkCancel to stop
     * early. Only one check runs at a time (a second start is refused with
     * @ref FileStatus::Conflict).
     *
     * @param[in]  volumeId Volume to check (`fat`, `sd`).
     * @param[out] detail   Optional technical detail.
     */
    virtual FileStatus checkStart(const std::string& volumeId,
                                  std::string* detail = nullptr) = 0;

    /** @brief Ask a running check to stop (returns immediately). */
    virtual void checkCancel() = 0;

    /** @brief Snapshot of the running/last check (safe to call any time). */
    virtual CheckReport checkReport() = 0;

    /**
     * @brief Re-read the LAN-only flag and the device subnet from the config.
     *
     * "Own subnet" is the device address plus the netmask from the DHCP
     * settings (`core::Subnet`) — the same definition the DNS and NTP filters
     * use, so all three always agree. Called at boot, when the file settings
     * are saved, and when the DHCP settings change (that page owns the subnet).
     *
     * With the filter enabled but an unusable (zero/non-contiguous) mask the
     * filter is **skipped with a warning** instead of blocking every client:
     * a broken subnet must not lock the operator out of the explorer.
     */
    virtual void applyAccessFilter() = 0;

    /**
     * @brief Decide whether a client may use the file endpoints.
     *
     * @param[in] clientIp4Host Client IPv4 in **host** byte order (0 =unknown,
     *                         in which case access is granted and warned about).
     * @return false only for a client outside the device's own subnet while
     *         the filter is enabled and the subnet is usable.
     */
    virtual bool allowClient(uint32_t clientIp4Host) = 0;

    /** @brief Clients refused by the LAN filter (diagnostics). */
    virtual uint32_t foreignBlocked() const = 0;

    /** @brief True when the LAN filter is currently enforced. */
    virtual bool filterActive() const = 0;

    /** @brief Free space kept in reserve when checking an upload fits. */
    static constexpr uint64_t kFreeSpaceReserve = 4096;

    /** @brief Files a single check reads at most (bounds one walk's runtime). */
    static constexpr uint32_t kCheckMaxFiles = 4096;

    /** @brief Failures kept in a report; beyond it only the counter grows. */
    static constexpr size_t kCheckMaxErrors = 64;

    /**
     * @brief Largest file the built-in text editor will load/save.
     *
     * The editor holds the whole file in RAM (the REST layer builds one JSON
     * document from it), so there is a hard limit; downloading such a file is
     * always possible, editing it is not.
     */
    static constexpr size_t kMaxTextBytes = 512 * 1024;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_IFILEMANAGER_H
