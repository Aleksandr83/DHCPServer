#ifndef DHCP_CORE_SSHLOG_H
#define DHCP_CORE_SSHLOG_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ArchiveFileLogTarget.h"
#include "ErrorLogCore.h"
#include "FreeRtosErrorQueue.h"
#include "SshLogWriter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dhcp {
namespace core {

/**
 * @brief The SSH journal: `logs/SSH.log` on a volume (stage 223).
 *
 * Sessions, refused passwords and the two refusals that never reach a password
 * (the own-subnet filter and the session limit). The arrangement is `AuthLog`'s,
 * deliberately: producers only format a sentence and enqueue it, one low-priority
 * task drains the queue and flushes every line, and a sentence lost to a full
 * queue is counted and reported with the next line. A FAT write never happens in
 * the SSH session task, which is the task a client is waiting on.
 *
 * Two switches, both on the Logging page: the file and the terminal. They are
 * separate because they answer different questions — a developer watching UART
 * wants to see sessions go by, while the file is for the operator who arrives
 * later. The switch is asked in one place (@ref submit), so turning either off is
 * effective at once and no caller has to know.
 *
 * A fault of the **server** (a listener that would not start, a port taken) is not
 * written here: it belongs to `Errors.log`, where the operator looks when
 * something is wrong. This file is a record of sessions, not of faults.
 */
class SshLog {
public:
    using CardMountedCheck = LogFileWriter::CardCheck;

    static SshLog& instance();

    /**
     * @brief Point the log at a volume and start the writing task.
     *
     * @param mountPoint     e.g. "/fat" → the file is `<mountPoint>/logs/SSH.log`
     * @param cardMountPoint e.g. "/sdcard" → where a full log is archived.
     * @param cardMounted    asked at every rotation, not remembered.
     * @return false when the task could not be created (sentences keep queueing
     *         until the queue fills, then they are counted as drops).
     */
    bool start(const std::string& mountPoint = "/fat",
               const std::string& cardMountPoint = std::string(),
               CardMountedCheck cardMounted = CardMountedCheck());

    /// @brief Keep the sentences in the file. ON by default (the operator asked
    /// for a journal that exists from the first session).
    void setLogFile(bool enabled) { logFile_ = enabled; }
    bool logFile() const { return logFile_; }

    /// @brief Echo the sentences to the UART console.
    void setLogTerminal(bool enabled) { logTerminal_ = enabled; }
    bool logTerminal() const { return logTerminal_; }

    /// @brief The cap the file rotates at (one of `LogFileSizes.h`'s three).
    void setFileLogSize(size_t bytes);
    size_t fileLogSize() const { return fileLogSize_; }

    /// @brief The file the sentences go to ("" before start()).
    const std::string& target() const;

    /// @brief Sentences lost so far (switched off is not a loss).
    uint32_t dropped() const;

    /// @brief `session opened from 192.168.1.5 as admin`.
    void noteSessionOpen(const std::string& clientIp, const std::string& user);

    /// @brief `session closed from 192.168.1.5 after 1 h 2 min 5 s`.
    void noteSessionClose(const std::string& clientIp, uint32_t durationSec);

    /// @brief `refused password from 192.168.1.5 (2 of 3)`.
    void noteAuthFailed(const std::string& clientIp, uint32_t attempt,
                        uint32_t maxAttempts);

    /// @brief `refused a session from 10.0.0.7: outside the own subnet`.
    void noteSubnetRefused(const std::string& clientIp);

    /// @brief `refused a session from 192.168.1.9: 2 of 2 already open`.
    void noteSessionLimit(const std::string& clientIp, uint32_t limit);

private:
    SshLog() = default;
    ~SshLog();

    SshLog(const SshLog&) = delete;
    SshLog& operator=(const SshLog&) = delete;

    static void taskEntry(void* arg);
    void run();

    /// @brief Format, echo to the terminal when asked, and enqueue.
    bool submit(LogLevel level, const std::string& text);

    FreeRtosErrorQueue queue_;
    std::unique_ptr<ArchiveFileLogTarget> target_;
    std::unique_ptr<ErrorLogCore> core_;
    bool logFile_ = true;
    bool logTerminal_ = false;
    size_t fileLogSize_ = kLogFileDefaultMaxBytes;
    TaskHandle_t task_ = nullptr;
    uint32_t preStartDropped_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SSHLOG_H
