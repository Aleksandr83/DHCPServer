#ifndef DHCP_CORE_AUTHLOG_H
#define DHCP_CORE_AUTHLOG_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ArchiveFileLogTarget.h"
#include "AuthLogWriter.h"
#include "ErrorLogCore.h"
#include "FreeRtosErrorQueue.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dhcp {
namespace core {

/**
 * @brief The authorisation log: `logs/Auth.log` on a volume (stage 205).
 *
 * Who tried to log in and what happened to them: a refused password with the
 * attempt and the budget, a client that was locked out and for how long, and the
 * moment somebody got in **after** failing. The switch on the Security page turns
 * the file on and off; the sentences themselves are `AuthLogFormat.h`'s, and the
 * rotation is `LogFileWriter`'s through `ArchiveFileLogTarget` — this class is the
 * same arrangement the error log and the journal use, and deliberately so:
 *
 *   * **producers** are the HTTP task (one task serves every request), so a
 *     producer only formats a sentence and enqueues it — no FAT write ever
 *     happens in the task that answers the browser;
 *   * **one low-priority task** drains the queue and writes, flushing every line,
 *     so a line that describes a refused password survives the restart it may have
 *     been the reason for;
 *   * a sentence lost to a full queue is **counted**, and the count is written
 *     into the file with the next line.
 *
 * The task is started once, at boot, and the switch decides whether sentences are
 * submitted at all — not whether the task exists. A settings change therefore
 * cannot fail halfway, and switching the log off is effective at once: the next
 * refused password is not written.
 *
 * Not a word about credentials is ever stored: the file holds an address, an
 * attempt number and a period. What was typed is the one thing this log must not
 * keep.
 */
class AuthLog {
public:
    /** @brief Is the card there right now? Asked at every rotation. */
    using CardMountedCheck = LogFileWriter::CardCheck;

    static AuthLog& instance();

    /**
     * @brief Point the log at a volume and start the writing task.
     * @param mountPoint     e.g. "/fat" → the file is `<mountPoint>/logs/Auth.log`
     * @param cardMountPoint e.g. "/sdcard" → where a full log is archived; "" when
     *                       the board has no card slot.
     * @param cardMounted    Asked at every rotation, not remembered.
     * @return false when the task could not be created (sentences keep queueing
     *         until the queue fills, then they are counted as drops).
     */
    bool start(const std::string& mountPoint = "/fat",
               const std::string& cardMountPoint = std::string(),
               CardMountedCheck cardMounted = CardMountedCheck());

    /** @brief The switch of the Security page: keep the attempts in the file. */
    void setLogFile(bool enabled) { logFile_ = enabled; }
    bool logFile() const { return logFile_; }

    /**
     * @brief The cap the file rotates at, in bytes (one of the three sizes the
     *        settings pages offer — see `LogFileSizes.h`).
     *
     * Applied to a writer that is already running: a smaller cap archives a file
     * that is over it at once, a larger one raises the limit where it stands.
     */
    void setFileLogSize(size_t bytes);
    size_t fileLogSize() const { return fileLogSize_; }

    /** @brief The file the sentences go to ("" before start()). */
    const std::string& target() const;

    /** @brief Sentences lost so far (switched off is not a loss). */
    uint32_t dropped() const;

    /** @brief A password that was refused: `failed login from … (3 of 5)`. */
    void noteFailedAttempt(const std::string& clientIp, uint32_t attempt,
                           uint32_t maxAttempts);

    /** @brief An address the device stopped answering: `locked out … for 300 s`. */
    void noteLockout(const std::string& clientIp, uint32_t lockoutSec);

    /** @brief Somebody got in after failing, which is the line that explains it. */
    void noteLogin(const std::string& clientIp, uint32_t failedBefore);

private:
    AuthLog() = default;
    ~AuthLog();

    AuthLog(const AuthLog&) = delete;
    AuthLog& operator=(const AuthLog&) = delete;

    static void taskEntry(void* arg);
    void run();

    /** @brief Format a sentence into a line and enqueue it. */
    bool submit(LogLevel level, const std::string& text);

    FreeRtosErrorQueue queue_;
    std::unique_ptr<ArchiveFileLogTarget> target_;
    std::unique_ptr<ErrorLogCore> core_;
    /** The switch of the Security page; off until the setting says otherwise. */
    bool logFile_ = false;
    size_t fileLogSize_ = kLogFileDefaultMaxBytes;
    TaskHandle_t task_ = nullptr;
    /** Sentences that arrived before start(): counted, like the other logs. */
    uint32_t preStartDropped_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTHLOG_H
