#ifndef DHCP_NET_IFILEOPERATIONS_H
#define DHCP_NET_IFILEOPERATIONS_H

#include <cstdint>
#include <string>
#include <vector>

namespace dhcp {
namespace net {

/**
 * @file IFileOperations.h
 * @brief The port the file shell writes through (Ports & Adapters).
 *
 * The shell itself is policy: which path a typed name means, which flags are
 * allowed, what the answer reads like. Every actual touch of a volume happens
 * behind this interface, which is what lets the whole shell run in a host test
 * against a fake — and what keeps the POSIX adapter (the only ESP-IDF part) small
 * enough to read.
 *
 * Paths handed to an implementation are already absolute and already sandboxed by
 * `FileShellPath`: an implementation may trust them to name a volume, and must not
 * do any policy of its own.
 */

/// @brief What an answer of the port means. One enumeration, so the shell's
///        sentences and the adapter's errno mapping cannot drift apart.
enum class FileOpStatus {
    Ok,
    NotFound,
    Exists,
    NotDirectory,
    IsDirectory,
    NotEmpty,
    Denied,
    NoSpace,
    IoError,
    Unsupported
};

/// @brief One directory entry as `ls` needs it.
struct FileEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
};

/// @brief What a volume reports about itself (`df`), in bytes.
struct FileSpace {
    bool mounted = false;
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
};

class IFileOperations {
public:
    virtual ~IFileOperations() = default;

    /// @brief Does @p path exist at all, and is it a directory?
    virtual FileOpStatus attributes(const std::string& path, bool& exists,
                                    bool& isDir, uint64_t& size) = 0;

    /// @brief The entries of a directory, in the order the volume returns them.
    virtual FileOpStatus list(const std::string& path, std::vector<FileEntry>& out) = 0;

    /// @brief Create one directory; the parent has to exist.
    virtual FileOpStatus makeDir(const std::string& path) = 0;

    /// @brief Create an empty file if it is not there. An existing one is left alone.
    virtual FileOpStatus touch(const std::string& path, bool& created) = 0;

    /// @brief Remove a file (never a directory).
    virtual FileOpStatus removeFile(const std::string& path) = 0;

    /// @brief Remove a directory with everything under it.
    virtual FileOpStatus removeTree(const std::string& path) = 0;

    /// @brief Rename within a volume.
    virtual FileOpStatus rename(const std::string& from, const std::string& to) = 0;

    /// @brief Copy a file's bytes; the caller has already refused an existing target.
    virtual FileOpStatus copy(const std::string& from, const std::string& to) = 0;

    /**
     * @brief Read a text file for `cat`.
     *
     * @param limit  the most bytes @p out may take; a longer file is refused rather
     *               than cut, because a truncated answer looks like a whole one.
     * @param binary set when the content cannot be printed (a zero byte, or bytes
     *               that are not printable in the current locale).
     */
    virtual FileOpStatus readText(const std::string& path, uint64_t limit,
                                  std::string& out, bool& binary) = 0;

    /// @brief Capacity of one volume root (`/fat`, `/sdcard`).
    virtual FileSpace space(const std::string& volumeRoot) = 0;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_IFILEOPERATIONS_H
