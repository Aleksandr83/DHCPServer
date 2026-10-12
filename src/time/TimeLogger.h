#ifndef DHCP_TIME_TIMELOGGER_H
#define DHCP_TIME_TIMELOGGER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/NtpLogWriter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace dhcp {
namespace time {

/**
 * @brief NTP request logger.
 *
 * Logs served NTP requests to:
 *   1. Terminal (ESP_LOGI) if enabled
 *   2. External REST service (HTTP POST) if configured and enabled
 *   3. A file on a volume (stage 191), archived to the card when it is full
 *
 * Both asynchronous paths are fire-and-forget: records go into a bounded ring
 * buffer drained by a task of its own, so neither a slow REST server nor a slow
 * FAT write ever blocks the NTP server task. For the file, that also keeps the
 * card *archive* — which copies 64 KB — out of the request path entirely.
 */
class TimeLogger {
public:
    /** @brief Is the card mounted right now? Asked at every rotation. */
    using CardMountedCheck = std::function<bool()>;

    TimeLogger();
    ~TimeLogger() = default;

    /** @brief Enable/disable terminal logging. */
    void setLogTerminal(bool enabled) { logTerminal_ = enabled; }
    bool logTerminal() const { return logTerminal_; }

    /** @brief Enable/disable REST logging (starts/stops the sender task). */
    void setLogRest(bool enabled);

    /** @brief Set the external REST logging URL. */
    void setLogUrl(const std::string& url);

    /** @brief Set HTTP Basic auth credentials for REST logging. */
    void setLogAuth(bool enabled, const std::string& user,
                    const std::string& pass);

    bool logRest() const { return logRest_; }
    const std::string& logUrl() const { return logUrl_; }
    bool logAuthEnabled() const { return logAuthEnabled_; }
    const std::string& logAuthUser() const { return logAuthUser_; }
    const std::string& logAuthPassword() const { return logAuthPassword_; }

    /** @brief Stop the async REST sender task (does not free the queue). */
    void stopRestSender();

    /**
     * @brief Tell the logger where the file log lives (stage 191).
     *
     * The paths are known where the volumes are — `main.cpp` — and not here: this
     * class writes where it is told to. Called once at boot, before the setting
     * is applied, so the order of the two does not matter.
     *
     * @param fatMountPoint  e.g. `/fat` → the live file is `<it>/logs/Ntp.log`.
     * @param cardMountPoint e.g. `/sdcard` → where a full file is archived; ""
     *                       when the board has no card slot at all.
     * @param cardMounted    Asked at every rotation, not remembered: a card can
     *                       be inserted and removed while the device runs.
     */
    void setFileLogVolumes(const std::string& fatMountPoint,
                           const std::string& cardMountPoint,
                           CardMountedCheck cardMounted);

    /**
     * @brief Log one event of the clock's own life (stage 192).
     *
     * The client starting and stopping, a successful synchronisation, and the
     * periodic "still not synchronised" line: what the device says about its own
     * attempts, next to what its clients ask. It goes to the same file as the
     * requests — one log for the time service — and to the terminal when that
     * destination is on.
     *
     * @param text The sentence, without the timestamp (the writing task adds it,
     *             so the line is stamped when it is written, like every other).
     */
    void logEvent(const std::string& text);

    /** @brief Enable/disable file logging (starts/stops the writer task). */
    void setLogFile(bool enabled);
    bool logFile() const { return logFile_; }

    /**
     * @brief Log one answered NTP request.
     *
     * @param clientAddr    Source IP of the NTP client.
     * @param stratum       Stratum the reply actually carried (16 for a request
     *                      answered with "the clock is not trustworthy").
     * @param clockVerified False when that was the answer. The terminal and REST
     *                      destinations keep their old behaviour and stay quiet
     *                      about such requests (they were never counted there),
     *                      while the **file** log records them with a mark: when
     *                      the device cannot synchronise, these lines are the
     *                      proof that clients are asking at all (stage 191).
     */
    void logRequest(const std::string& clientAddr, uint8_t stratum,
                    bool clockVerified = true);

private:
    // ─── Async REST logging (fire-and-forget) ───────
    // Rule 39: the client address is the only text in this record.
    static constexpr size_t kClientTextBytes = 48;

    struct RestLogRecord {
        uint32_t ts = 0;          // uptime ms
        uint8_t  stratum = 0;
        bool     stop = false;    // internal: stop marker for the sender task
        char client[kClientTextBytes] = {0};
    };
    static constexpr int kRestQueueDepth = 16;
    static constexpr int kRestSenderStack = 8192;  // sender task stack (TLS)
    static constexpr int kRestSenderPriority = 3;

    void updateRestSenderState();
    void ensureRestSender();
    static void restSenderTask(void* arg);
    void sendRestLog(const RestLogRecord& rec);
    std::string buildRestJson(const RestLogRecord& rec) const;

    // ─── Async file logging (fire-and-forget) ───────
    // Rule 39: the fields one queued record carries. One text field serves both
    // kinds of line — the client address of a request, or the sentence of an
    // event — so the queue holds one type of record and the writing task decides
    // which shape to build from the flag.
    static constexpr int kFileQueueDepth = 16;
    static constexpr int kFileTaskStack = 4096;   // no TLS and no HTTP here
    static constexpr int kFileTaskPriority = 3;
    /** How often a write that keeps failing is repeated on the terminal. */
    static constexpr uint32_t kWriteFailureWarnEvery = 100;
    /** Longest event sentence the queue carries (it is clamped, not dropped). */
    static constexpr size_t kEventTextBytes = 160;

    struct FileLogRecord {
        uint8_t stratum = 0;
        bool    clockVerified = true;
        bool    stop = false;     // internal: stop marker for the writer task
        bool    isEvent = false;  // text is an event's sentence, not a client
        char    text[kEventTextBytes] = {0};
    };

    void updateFileLogState();
    void ensureFileWriter();
    void stopFileWriter();
    static void fileLogTask(void* arg);
    void writeFileLog(const FileLogRecord& rec);

    bool logTerminal_ = false;
    bool logRest_ = false;
    std::string logUrl_;
    bool logAuthEnabled_ = false;
    std::string logAuthUser_;
    std::string logAuthPassword_;

    // File logging: off until the setting says otherwise, and mute until the
    // volumes are known (see setFileLogVolumes).
    bool logFile_ = false;
    bool fileVolumesReady_ = false;
    std::string fatMountPoint_;
    std::string cardMountPoint_;
    CardMountedCheck cardMounted_;
    std::unique_ptr<core::NtpLogWriter> fileWriter_;
    uint32_t writeFailures_ = 0;

    QueueHandle_t restQueue_ = nullptr;
    TaskHandle_t restTask_ = nullptr;
    volatile bool restStopRequested_ = false;

    QueueHandle_t fileQueue_ = nullptr;
    TaskHandle_t fileTask_ = nullptr;
    volatile bool fileStopRequested_ = false;
};

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_TIMELOGGER_H
