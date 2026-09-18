#ifndef DHCP_CORE_ERRORLOG_H
#define DHCP_CORE_ERRORLOG_H

#include <cstdarg>
#include <cstdint>
#include <memory>
#include <string>

#include "ErrorLogCore.h"
#include "FileErrorLogTarget.h"
#include "FreeRtosErrorQueue.h"

namespace dhcp {
namespace core {

/**
 * @brief The device's error log: `logs/Errors.log` on a volume, written by a task
 *        of its own through a queue.
 *
 * The terminal is not always there (the operator often has no serial console
 * attached, which is exactly how a failed `Statistica.dat` write went unnoticed),
 * so critical errors are kept where the web file explorer can read them.
 *
 * The shape is dictated by who calls it:
 *
 *   * **producers** are the DNS task, the single httpd task, the transfer task —
 *     tasks the device cannot afford to park on a FAT write. `error()` therefore
 *     only formats a line and drops it into the queue, and returns immediately,
 *     whether the log is started or not;
 *   * **one consumer** — a low-priority task started by @ref start — takes the
 *     messages out and writes them. If that task is not running yet, or is
 *     slower than the errors arrive, messages are lost **and counted**, and the
 *     count is written into the log with the next line.
 *
 * The volume is a parameter of @ref start, not a constant: today the internal
 * FAT, tomorrow whatever the operator picks in the settings (the microSD is the
 * obvious candidate). No setting exists yet — only the seam.
 */
class ErrorLog {
public:
    static ErrorLog& instance();

    /**
     * @brief Point the log at a volume and start the writing task.
     * @param mountPoint e.g. "/fat" → the file is `<mountPoint>/logs/Errors.log`
     * @return false when the task could not be created (messages keep queueing
     *         until the queue fills, then they are counted as drops).
     */
    bool start(const std::string& mountPoint = "/fat");

    /** @brief Producer API. Returns false when the message was dropped. */
    bool error(const char* tag, const std::string& message);
    bool warn(const char* tag, const std::string& message);
    /** @brief Producer API, printf-style (the text is clamped to one queue item). */
    bool errorf(const char* tag, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));

    /** @brief The file the lines go to ("" before start()). */
    const std::string& target() const;

    /** @brief Messages lost so far (queue full, target refused them, or no task). */
    uint32_t dropped() const;

    /**
     * @brief The producer half, for code that must log from a task of its own
     * (the statistics write does: it owns its task and its own verdict).
     */
    ErrorLogCore* core() { return core_.get(); }

private:
    ErrorLog() = default;
    ~ErrorLog();
    ErrorLog(const ErrorLog&) = delete;
    ErrorLog& operator=(const ErrorLog&) = delete;

    static void taskEntry(void* arg);
    void run();

    FreeRtosErrorQueue queue_;
    std::unique_ptr<FileErrorLogTarget> target_;
    std::unique_ptr<ErrorLogCore> core_;
    TaskHandle_t task_ = nullptr;
    /** Kept separately: before start() there is no core to count into. */
    uint32_t preStartDropped_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_ERRORLOG_H
