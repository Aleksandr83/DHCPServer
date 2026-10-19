#ifndef DHCP_NET_POSIXFILEOPERATIONS_H
#define DHCP_NET_POSIXFILEOPERATIONS_H

#include "IFileOperations.h"

namespace dhcp {
namespace net {

/**
 * @file PosixFileOperations.h
 * @brief The adapter: `IFileOperations` on top of the VFS the device mounts.
 *
 * The only part of the file shell that knows a real filesystem exists. It does no
 * policy of its own — the paths arrive absolute and already sandboxed — and it maps
 * `errno` onto `FileOpStatus` in one place, so the shell's sentences and the
 * device's failures cannot drift apart.
 *
 * Two behaviours are deliberate:
 *
 *   * `space()` asks the FAT layer (`esp_vfs_fat_info`), not `statvfs`: this
 *     ESP-IDF does not register `statvfs` for a FAT volume, and a `df` that
 *     silently printed zeros would be worse than one that says the volume is not
 *     mounted;
 *   * `readText()` refuses a file over the limit instead of cutting it and marks
 *     anything with a NUL or an unprintable control byte as binary: `cat` is for
 *     reading, and half a log line looks like a whole one.
 */
class PosixFileOperations : public IFileOperations {
public:
    FileOpStatus attributes(const std::string& path, bool& exists, bool& isDir,
                            uint64_t& size) override;
    FileOpStatus list(const std::string& path, std::vector<FileEntry>& out) override;
    FileOpStatus makeDir(const std::string& path) override;
    FileOpStatus touch(const std::string& path, bool& created) override;
    FileOpStatus removeFile(const std::string& path) override;
    FileOpStatus removeTree(const std::string& path) override;
    FileOpStatus rename(const std::string& from, const std::string& to) override;
    FileOpStatus copy(const std::string& from, const std::string& to) override;
    FileOpStatus readText(const std::string& path, uint64_t limit, std::string& out,
                          bool& binary) override;
    FileSpace space(const std::string& volumeRoot) override;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_POSIXFILEOPERATIONS_H
