#ifndef DHCP_TIME_TIMELOGGER_H
#define DHCP_TIME_TIMELOGGER_H

#include <string>
#include <cstdint>

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
 *
 * The REST path is fire-and-forget: records go into a bounded ring buffer
 * drained by a dedicated sender task, so a slow REST server never blocks the
 * NTP server task.
 */
class TimeLogger {
public:
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
     * @brief Log one served NTP request.
     * @param clientAddr  Source IP of the NTP client.
     * @param stratum     Stratum advertised in the reply.
     */
    void logRequest(const std::string& clientAddr, uint8_t stratum);

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

    bool logTerminal_ = false;
    bool logRest_ = false;
    std::string logUrl_;
    bool logAuthEnabled_ = false;
    std::string logAuthUser_;
    std::string logAuthPassword_;

    QueueHandle_t restQueue_ = nullptr;
    TaskHandle_t restTask_ = nullptr;
    volatile bool restStopRequested_ = false;
};

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_TIMELOGGER_H
