#ifndef DHCP_DHCP_DHCPFILELOG_H
#define DHCP_DHCP_DHCPFILELOG_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "DhcpLogWriter.h"
#include "../core/ArchiveFileLogTarget.h"
#include "../core/ErrorLogCore.h"
#include "../core/FreeRtosErrorQueue.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dhcp {
namespace dhcp {

/**
 * @brief The DHCP log as a file: `logs/Dhcp.log` on a volume (stage 212).
 *
 * The operator asked for the terminal log to have a file of its own, and this is
 * the file: every line the DHCP server would have printed goes to it as well, with
 * the stamp the other logs carry (`[2026-10-16 21:30:00] dhcp: DHCP OFFER …`), the
 * switch on the DHCP Logging page decides whether anything is kept, the size is
 * chosen on that page and a full file is copied to the card.
 *
 * The arrangement is deliberately the authorisation log's (stage 205) — and the
 * error log's and the journal's before it — because each part of it was decided for
 * a reason:
 *
 *   * **the producer is the DHCP task**, which must never wait for a FAT write: a
 *     packet that is not answered in time is a client without an address, so a
 *     producer only formats a line and enqueues it;
 *   * **one low-priority task** drains the queue and writes, flushing every line,
 *     so a line about an address is on the card before the device may restart;
 *   * **a line lost to a full queue is counted** and the count is written with the
 *     next line, because a log with silent holes is worse than no log;
 *   * the task runs from boot and the **switch** decides whether lines are
 *     submitted, so switching the log off is effective at once and a settings
 *     change cannot fail halfway.
 */
class DhcpFileLog {
public:
    /** @brief Is the card there right now? Asked at every rotation. */
    using CardMountedCheck = core::LogFileWriter::CardCheck;

    /** @brief Point the log at a volume and start the writing task. */
    bool start(const std::string& mountPoint = "/fat",
               const std::string& cardMountPoint = std::string(),
               CardMountedCheck cardMounted = CardMountedCheck());

    /** @brief The switch of the DHCP Logging page: keep the DHCP log in a file. */
    void setLogFile(bool enabled) { logFile_ = enabled; }
    bool logFile() const { return logFile_; }

    /**
     * @brief The cap the file rotates at, in bytes (one of the three sizes the
     *        settings pages offer — see `core/LogFileSizes.h`).
     *
     * Applied to a writer that is already running: a smaller cap archives a file
     * that is over it at once, a larger one raises the limit where it stands.
     */
    void setFileLogSize(size_t bytes);
    size_t fileLogSize() const { return fileLogSize_; }

    /** @brief The file the lines go to ("" before start()). */
    const std::string& target() const;

    /** @brief Lines lost so far (switched off is not a loss). */
    uint32_t dropped() const;

    /**
     * @brief Keep one line: the text is what the terminal was given.
     *
     * The caller owns the sentence (the DHCP server builds it once and hands the
     * same text to both destinations — stage 212); this class adds the stamp and
     * the `dhcp:` tag, so the file and the terminal cannot disagree about what
     * happened.
     */
    void note(core::LogLevel level, const std::string& text);

private:
    static void taskEntry(void* arg);
    void run();

    core::FreeRtosErrorQueue queue_;
    std::unique_ptr<core::ArchiveFileLogTarget> target_;
    std::unique_ptr<core::ErrorLogCore> core_;
    /** The switch of the DHCP Logging page; off until the setting says otherwise. */
    bool logFile_ = false;
    size_t fileLogSize_ = core::kLogFileDefaultMaxBytes;
    TaskHandle_t task_ = nullptr;
    /** Lines that arrived before start(): counted, like the other logs. */
    uint32_t preStartDropped_ = 0;
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DHCPFILELOG_H
