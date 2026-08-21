#ifndef DHCP_DHCP_DHCPRESTLOGGER_H
#define DHCP_DHCP_DHCPRESTLOGGER_H

#include <string>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace dhcp {
namespace dhcp {

/**
 * @brief Async DHCP event logger → external REST service (fire-and-forget).
 *
 * DHCP events (OFFER/ACK/NAK/RELEASE/DECLINE) are pushed into a bounded ring
 * buffer (FreeRTOS queue); on overflow the oldest record is overwritten. A
 * dedicated sender task drains the buffer and POSTs each record as JSON with
 * a timeout, so a slow REST server never blocks the DHCP task.
 */
class DhcpRestLogger {
public:
    DhcpRestLogger() = default;
    ~DhcpRestLogger() = default;

    /**
     * @brief Enable/disable REST logging.
     */
    void setEnabled(bool enabled);

    /**
     * @brief Set the external REST logging URL.
     */
    void setUrl(const std::string& url);

    /**
     * @brief Set HTTP Basic auth credentials.
     */
    void setAuth(bool enabled, const std::string& user,
                 const std::string& pass);

    /**
     * @brief Log a DHCP event (non-blocking).
     *
     * @param event     OFFER | ACK | NAK | RELEASE | DECLINE.
     * @param mac       Client MAC ("xx:xx:xx:xx:xx:xx").
     * @param ip        Client IP (may be empty).
     * @param mask      Subnet mask (may be empty).
     * @param gateway   Router/gateway advertised (may be empty).
     * @param dns       DNS advertised (may be empty).
     * @param leaseTime Lease time in seconds (0 if not applicable).
     */
    void logEvent(const std::string& event,
                  const std::string& mac,
                  const std::string& ip,
                  const std::string& mask,
                  const std::string& gateway,
                  const std::string& dns,
                  int32_t leaseTime);

    /**
     * @brief Stop the sender task (called on shutdown).
     */
    void stopRestSender();

private:
    // Fixed-size queue item (no heap allocation in the DHCP task).
    struct Record {
        char event[16] = {0};
        char mac[24] = {0};
        char ip[16] = {0};
        char mask[16] = {0};
        char gateway[16] = {0};
        char dns[16] = {0};
        int32_t leaseTime = 0;
        bool stop = false;   // internal stop marker
    };
    static constexpr int kQueueDepth = 16;
    static constexpr int kSenderStack = 8192;
    static constexpr int kSenderPriority = 3;
    static constexpr int kSendTimeoutMs = 5000;

    void updateSenderState();
    void ensureSender();
    static void senderTask(void* arg);
    void sendRecord(const Record& rec);
    std::string buildJson(const Record& rec) const;

    bool enabled_ = false;
    std::string url_;
    bool authEnabled_ = false;
    std::string authUser_;
    std::string authPassword_;

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool stopRequested_ = false;
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DHCPRESTLOGGER_H
