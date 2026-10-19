#ifndef DHCP_CORE_ATOMICFILE_H
#define DHCP_CORE_ATOMICFILE_H

#include <string>

namespace dhcp {
namespace core {

/**
 * @file AtomicFile.h
 * @brief Publishing a file so that a crash cannot empty it.
 *
 * `cache.dat` and `Statistica.dat` used to be written straight into their file.
 * `fopen(…, "wb")` truncates at once, so a panic — or a power cut — in the middle
 * of a multi-megabyte write left a half-written file: the next boot refused it as
 * damaged and the operator's cache was gone. On 19.10.2026 that is exactly what
 * happened when turning the SSH server off panicked the device.
 *
 * The policy is the one the file explorer already uses for uploads: write
 * `<name>.part`, then publish. FatFS forces one extra step — `f_rename()` refuses
 * an existing destination — so the old file is removed first, and that leaves a
 * window between two directory operations. @ref recover closes it: when the
 * destination is missing and a complete `.part` is beside it, the part **is** the
 * new file, and it is published rather than lost.
 *
 * What this costs: one extra directory entry per save (a `.part` that lives for
 * the duration of the write) and two renames instead of truncation. What it buys:
 * the file on the volume is either the previous complete version or the new one,
 * never a torn mixture.
 *
 * Free of ESP-IDF on purpose — host-tested in `test/test_atomicfile.cpp`.
 */
namespace atomicfile {

/// @brief `<path>.part` — where a new version is written.
std::string partPath(const std::string& path);

/**
 * @brief Publish `<path>.part` as @p path.
 *
 * The part must already be complete and closed. The old file is removed (FatFS
 * will not rename onto an existing name), then the part is renamed into place.
 * A false answer leaves whatever the volume holds untouched and keeps the part,
 * so the next @ref recover can still publish it.
 */
bool publish(const std::string& path);

/**
 * @brief Finish a publish that a crash interrupted.
 *
 * When @p path is missing and `<path>.part` exists, the part is published. A
 * healthy @p path is never touched, and a stale part beside it is left alone (a
 * publish of a newer version deletes it as it goes).
 */
void recover(const std::string& path);

/// @brief Does @p path open for reading right now?
bool readable(const std::string& path);

} // namespace atomicfile
} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_ATOMICFILE_H
