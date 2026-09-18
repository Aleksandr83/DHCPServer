#ifndef DHCP_WEB_FILEJSON_H
#define DHCP_WEB_FILEJSON_H

#include <cstdint>
#include <string>
#include <vector>

#include "../core/JobRegistry.h"
#include "../files/IFileManager.h"
#include "../files/TransferEngine.h"
#include "../storage/IFileSystem.h"
#include "JsonWriter.h"

namespace dhcp {
namespace web {

/**
 * @brief JSON payloads of the file-explorer REST endpoints.
 *
 * The handlers of the file-explorer API used to assemble their bodies inline,
 * next to the HTTP plumbing, with a hand-written answer per endpoint. That is
 * how a leading comma survived review in the volumes endpoint: the body was
 * `{,"enabled":…}`, the status was still `200 OK`, and the page died in
 * `JSON.parse()` instead — a defect neither the browser check (it ran against
 * a mock server with its own JSON) nor a compiler could catch.
 *
 * So the payloads live here: pure functions over the domain types, with no
 * `httpd`, no `esp_err_t` and no I/O, which makes them host-testable with
 * exact-string assertions (see `test/test_filejson.cpp`). Handlers keep the
 * transport half — auth, the access filter, status codes — and hand the
 * payload building over.
 *
 * Member order is part of the contract: the web UI and `Docs/Rest.md` document
 * these field names, so the builders keep them stable.
 */
class FileJson {
public:
    /** @brief Input of @ref list — one directory listing plus its volume state. */
    struct ListPayload {
        std::string volume;                 ///< Volume id the listing came from
        std::string path;                   ///< Normalized volume-relative path
        bool mounted = false;               ///< Volume mounted when it was read
        uint64_t totalBytes = 0;            ///< Capacity of the volume
        uint64_t freeBytes = 0;             ///< Free space of the volume
        bool truncated = false;             ///< Listing hit `kMaxListEntries`
        std::vector<::dhcp::files::FileEntry> entries;
    };

    /** @brief Input of @ref text — one readable text file. */
    struct TextPayload {
        std::string volume;                 ///< Volume id
        std::string path;                   ///< Normalized volume-relative path
        std::string text;                   ///< File content (already readable text)
        uint64_t size = 0;                  ///< Size in bytes
        uint64_t mtime = 0;                 ///< Modification time (Unix seconds)
        bool truncated = false;             ///< Content was cut at the size limit
    };

    /** @brief Input of @ref settings — the file-explorer access policy. */
    struct SettingsPayload {
        bool enabled = false;               ///< The build has FAT volumes
        bool allowOwnSubnet = true;         ///< LAN-only filter switched on
        bool filterActive = false;          ///< The filter can actually be applied
        std::string subnetAddress;          ///< Device address of the subnet
        std::string subnetMask;             ///< Netmask of the subnet
        uint64_t blockedCount = 0;          ///< Refused requests since boot
    };

    /**
     * @brief Array of volume objects — the representation shared by
     * `GET /api/status` (`volumes`) and `GET /api/files/volumes`.
     *
     * `[{"id":…,"mount_point":…,"mounted":…,"present":…,"total_bytes":…,
     * "free_bytes":…,"error":…}, …]`
     *
     * @param volumes State of every registered volume (may be empty).
     */
    static std::string volumeArray(const std::vector<::dhcp::storage::VolumeInfo>& volumes);

    /**
     * @brief Body of `GET /api/files/volumes`.
     *
     * `{"enabled":<bool>,"volumes":[…]}` — the explorer's own endpoint, which
     * also reports whether this build offers the explorer at all.
     *
     * @param enabled Whether the running build offers the explorer at all.
     * @param volumes State of every registered volume (may be empty).
     */
    static std::string volumes(bool enabled,
                               const std::vector<::dhcp::storage::VolumeInfo>& volumes);

    /** @brief Body of `GET /api/files/list` — see @ref ListPayload. */
    static std::string list(const ListPayload& payload);

    /** @brief Body of `GET /api/files/text` — see @ref TextPayload. */
    static std::string text(const TextPayload& payload);

    /** @brief Body of `GET /api/files/settings` — see @ref SettingsPayload. */
    static std::string settings(const SettingsPayload& payload);

    /**
     * @brief Body of `GET /api/files/transfer` — the snapshot of the transfer job.
     *
     * `{"phase":…,"busy":…,"finished":…,"cancelled":…,"instant":…,"op":…,
     * "src_volume":…,"dst_volume":…,"dst_path":…,"current":…,"done_bytes":…,
     * "total_bytes":…,"needed_bytes":…,"free_bytes":…,"files_done":…,
     * "files_total":…,"dirs_done":…,"dirs_total":…,"skipped":…,"failed":…,
     * "deleted":…,"error":…,"error_path":…}`
     *
     * One payload covers every state the page has to draw: while `busy` the bar
     * uses `done_bytes`/`total_bytes` (`total_bytes == 0` means the measurement is
     * still running, so the page draws an indeterminate bar rather than dividing
     * by zero), and once `finished` the very same object is the summary — how much
     * was copied, what was skipped and what failed.
     */
    static std::string transfer(const ::dhcp::files::TransferReport& report);

    /**
     * @brief Body of the `409` answer of `POST /api/files/transfer`.
     *
     * `{"status":"conflict","conflicts":[{"name":…}, …]}` — the names that
     * are already in the destination. That is what the page turns into "replace
     * them?" before it sends the same request again with an explicit policy.
     */
    static std::string transferConflicts(const std::vector<std::string>& names);

    /**
     * @brief Body of `GET /api/files/check` — a volume-check report.
     *
     * `{"busy":…,"finished":…,"truncated":…,"cancelled":…,"volume":…,
     * "current":…,"dirs":…,"files":…,"bad_entries":…,"bytes_read":…,
     * "budget_bytes":…,"errors":[{"path":…,"detail":…}, …]}`
     *
     * `bad_entries` counts every failure, `errors` carries at most
     * @ref ::dhcp::files::IFileManager::kCheckMaxErrors of them (a card that is
     * failing wholesale should not build a megabyte-long answer), so the UI can
     * say "N damaged, first 64 listed" without a second field.
     *
     * @param report Snapshot of the running or last check.
     */
    static std::string check(const ::dhcp::files::CheckReport& report);

    /**
     * @brief Body of `GET /api/jobs` — the long-running operations of the device.
     *
     * `{"jobs":[{"id":…,"title_key":…,"arg":…,"state":…,"done":…,
     * "total":…,"percent":…,"detail":…,"elapsed_ms":…,
     * "cancel_requested":…,"repeat_sec":…}, …]}`
     *
     * Every entry here is an operation that has not finished, so every one of
     * them can be asked to stop — there is no "cancellable" flag to look at.
     *
     * `percent` is **-1** when the operation cannot say (an unknown total): the
     * page draws an indeterminate bar then, so it never divides by anything
     * itself. The list is empty when nothing runs — a finished one-off operation
     * is removed by the registry, a repeating one stays with its `repeat_sec`.
     */
    static std::string jobs(const std::vector<::dhcp::core::JobInfo>& jobs);

private:
    /** @brief `[{"name":…}, …]` for the names taken in the destination. */
    static std::string nameArray(const std::vector<std::string>& names);

    /** @brief `[{…}, …]` for the directory listing (comma-safe for 0/1/n entries). */
    static std::string entryArray(const std::vector<::dhcp::files::FileEntry>& entries);

    /** @brief One entry object `{"name":…,"is_dir":…,"size":…,"mtime":…}`. */
    static std::string entryObject(const ::dhcp::files::FileEntry& entry);

    /** @brief `[{…}, …]` for the failures of a volume check. */
    static std::string checkErrorArray(const std::vector<::dhcp::files::CheckError>& errors);

    /** @brief One failure object `{"path":…,"detail":…}`. */
    static std::string checkErrorObject(const ::dhcp::files::CheckError& error);

    /** @brief `[{…}, …]` for the running and scheduled operations. */
    static std::string jobArray(const std::vector<::dhcp::core::JobInfo>& jobs);

    /** @brief One operation object (see @ref jobs for the fields). */
    static std::string jobObject(const ::dhcp::core::JobInfo& job);
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_FILEJSON_H
