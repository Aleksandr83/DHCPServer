#ifndef DHCP_FILES_IFILEOPS_H
#define DHCP_FILES_IFILEOPS_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "IFileManager.h"
#include "IFileSink.h"
#include "IFileSource.h"

namespace dhcp {
namespace files {

/**
 * @brief Visitor of @ref IFileOps::scan — one directory entry at a time.
 *
 * The transfer engine walks trees that are far larger than one HTTP listing,
 * so it must not be handed a materialized `std::vector<FileEntry>`: a directory
 * of 520 entries would be cut at @ref IFileManager::kMaxListEntries, and on a
 * *move* the missing tail would be deleted from the source with the tree —
 * silent data loss. The visitor is called per entry instead, which keeps the
 * engine's memory constant and makes the listing cap irrelevant for it.
 */
class IDirVisitor {
public:
    virtual ~IDirVisitor() = default;

    /**
     * @brief Handle one entry.
     * @return false to stop the walk (the caller has everything it needs).
     */
    virtual bool visit(const FileEntry& entry) = 0;
};

/**
 * @brief The filesystem seam the transfer engine works through.
 *
 * The engine has to be testable on the host — a cross-volume copy that loses
 * files is exactly the kind of defect that must be proven in a test, and the
 * device filesystem (FatFS over the VFS) cannot be brought up in one. So the
 * engine never calls POSIX, `IFileManager` or the VFS directly: it asks this
 * interface, `FileManager` implements it on the device and the host test
 * implements it over a real temporary directory.
 *
 * Signatures deliberately match @ref IFileManager where the operation is the
 * same (`stat`, `mkdir`, `remove`, `rename`, `openRead`, `freeBytes`), so
 * `FileManager` satisfies both interfaces with a single override each.
 */
class IFileOps {
public:
    virtual ~IFileOps() = default;

    /**
     * @brief Walk a directory without building a listing.
     *
     * Entries are visited in filesystem order (no sorting) and the temporary
     * `.part` files of interrupted uploads are skipped, exactly like the
     * explorer does.
     */
    virtual FileStatus scan(const std::string& volumeId, const std::string& path,
                            IDirVisitor& visitor, std::string* detail = nullptr) = 0;

    /** @brief Metadata of one entry; @ref FileStatus::NotFound when it is gone. */
    virtual FileStatus stat(const std::string& volumeId, const std::string& path,
                            FileEntry& out, std::string* detail = nullptr) = 0;

    /** @brief Create a directory (@ref FileStatus::AlreadyExists when it is there). */
    virtual FileStatus mkdir(const std::string& volumeId, const std::string& path,
                             std::string* detail = nullptr) = 0;

    /** @brief Delete a file or (with @p recursive) a whole directory. */
    virtual FileStatus remove(const std::string& volumeId, const std::string& path,
                              bool recursive = false,
                              std::string* detail = nullptr) = 0;

    /** @brief Rename/move inside one volume (instant; no bytes are copied). */
    virtual FileStatus rename(const std::string& volumeId, const std::string& from,
                              const std::string& to, std::string* detail = nullptr) = 0;

    /** @brief Open a regular file for a streamed read. */
    virtual FileStatus openRead(const std::string& volumeId, const std::string& path,
                               std::unique_ptr<IFileSource>& out,
                               std::string* detail = nullptr) = 0;

    /**
     * @brief Open a destination file for a streamed write.
     *
     * Unlike `IFileManager::openWrite` this is not an upload: the engine writes
     * the whole file in one go, so there is no `UploadRange`, no continuation
     * and no temporary file left behind on failure — the sink discards its
     * `.part` when the transfer is aborted or cancelled.
     */
    virtual FileStatus createWriter(const std::string& volumeId, const std::string& path,
                                   std::unique_ptr<IFileSink>& out,
                                   std::string* detail = nullptr) = 0;

    /** @brief Free bytes of a mounted volume (0 when it is not mounted). */
    virtual uint64_t freeBytes(const std::string& volumeId) = 0;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_IFILEOPS_H
