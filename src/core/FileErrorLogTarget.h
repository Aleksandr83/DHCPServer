#ifndef DHCP_CORE_FILEERRORLOGTARGET_H
#define DHCP_CORE_FILEERRORLOGTARGET_H

#include <string>

#include "IErrorLogTarget.h"

namespace dhcp {
namespace core {

/**
 * @brief The error log as a file on a volume (`/fat/logs/Errors.log` today).
 *
 * Deliberately plain stdio + sys/stat, so the same code runs on the device and
 * in the host tests, and so the volume it writes to is nothing but the path it is
 * constructed with: the settings page will one day hand it `/sdcard/logs/...`
 * instead, and nothing else has to change.
 *
 * Two properties matter here and are worth stating out loud:
 *
 *   * **every line is flushed and closed before the call returns** — the whole
 *     point of this file is to survive the restart it describes, and a buffered
 *     line is exactly what a reboot loses;
 *   * **the file is capped** (`kMaxBytes`), and when it is full the previous log
 *     is kept once as `Errors.log.1` and a fresh one starts. A diagnostic file
 *     that can fill the volume it lives on would cause the next failure itself.
 *
 * Nothing is created blindly. The directory the log lives in is created **once**,
 * and only inside a parent that already exists — the mount point (`/fat`) belongs
 * to the storage layer, and inventing a directory under that name would hide a
 * volume that never mounted behind a log file that looks healthy. If the parent
 * is not there, or if something that is not a file sits under the log's name, the
 * write is refused and the caller counts the loss.
 */
class FileErrorLogTarget : public IErrorLogTarget {
public:
    /** Size at which the file is rotated (one previous generation is kept). */
    static constexpr size_t kMaxBytes = 64 * 1024;
    /** Suffix of the kept generation. */
    static constexpr const char* kRotatedSuffix = ".1";

    explicit FileErrorLogTarget(std::string path);

    bool append(LogLevel level, const std::string& line) override;
    const std::string& description() const override { return path_; }

    /** @brief The directory the log lives in (created on first write). */
    static std::string directoryOf(const std::string& path);

private:
    /** Create the log's directory if it is not there yet. */
    bool ensureDirectory();
    /** Keep the current log as `.1` when it is full, so a new one can start. */
    bool rotateIfNeeded(size_t incomingBytes);

    std::string path_;
    bool directoryReady_ = false;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_FILEERRORLOGTARGET_H
