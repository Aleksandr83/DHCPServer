#ifndef DHCP_DNS_DNSLOGGER_H
#define DHCP_DNS_DNSLOGGER_H

#include <string>
#include <map>
#include <vector>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace dhcp {
namespace dns {

/**
 * @brief Where a DNS query was resolved from (logging category).
 */
enum class DnsLogSource {
    LOCAL,      // resolved from custom local hosts (user-assigned)
    CACHE,      // resolved from cache
    FORWARDED   // resolved via the external DNS server
};

/**
 * @brief DNS query logger.
 *
 * Logs DNS queries to:
 *   1. Terminal (ESP_LOGI) if enabled (and the source filter is on)
 *   2. External REST service (HTTP POST) if configured and enabled
 */
class DnsLogger {
public:
    DnsLogger();
    ~DnsLogger() = default;

    /**
     * @brief Enable/disable terminal logging.
     */
    void setLogTerminal(bool enabled);

    /**
     * @brief Enable/disable the per-category terminal filters.
     */
    void setLogForwarded(bool enabled);
    void setLogLocal(bool enabled);
    void setLogCache(bool enabled);

    /**
     * @brief Enable/disable the "sent to REST" terminal filter.
     *
     * When enabled, terminal logging only shows queries that were also
     * sent to the external REST log service.
     */
    void setLogRestSent(bool enabled);

    /**
     * @brief Get per-category filter state.
     */
    bool logForwarded() const { return logForwarded_; }
    bool logLocal() const { return logLocal_; }
    bool logCache() const { return logCache_; }
    bool logRestSent() const { return logRestSent_; }

    /**
     * @brief Enable/disable REST logging.
     */
    void setLogRest(bool enabled);

    /**
     * @brief Set the external REST logging URL.
     */
    void setLogUrl(const std::string& url);

    /**
     * @brief Set HTTP Basic auth credentials for REST logging.
     *
     * @param enabled Whether to send the Authorization header.
     * @param user    Basic auth username.
     * @param pass    Basic auth password.
     */
    void setLogAuth(bool enabled, const std::string& user,
                    const std::string& pass);

    bool logAuthEnabled() const { return logAuthEnabled_; }
    const std::string& logAuthUser() const { return logAuthUser_; }
    const std::string& logAuthPassword() const { return logAuthPassword_; }

    /**
     * @brief Provide the local hosts map (domain → IPs) so the REST URL
     *        hostname can be resolved against it before sending.
     *
     * The pointer must stay valid for the lifetime of the logger (it is
     * owned by the DNS server). Pass nullptr to disable resolution.
     */
    void setLocalHosts(const std::map<std::string, std::vector<std::string>>* hosts);

    /**
     * @brief Stop the async REST sender task.
     *
     * Wakes the sender (if it is blocked on the queue), waits for it to exit
     * and marks the REST sender as stopped. No-op if it is not running.
     */
    void stopRestSender();

    /**
     * @brief Log a DNS query.
     *
     * @param domain     The queried domain name.
     * @param type       DNS query type (1=A, 28=AAAA).
     * @param clientAddr Source IP in string form.
     * @param clientMac  Client MAC ("xx:xx:xx:xx:xx:xx") or empty if unknown.
     * @param source     Where the query was resolved from.
     * @param resolved   Whether the query was resolved successfully.
     * @param answer     The resolved IP (or empty if not resolved).
     */
    void logQuery(const std::string& domain, uint16_t type,
                  const std::string& clientAddr,
                  const std::string& clientMac,
                  DnsLogSource source,
                  bool resolved, const std::string& answer);

private:
    void logToTerminal(const std::string& domain, uint16_t type,
                       const std::string& clientAddr,
                       const std::string& clientMac,
                       DnsLogSource source,
                       bool resolved, const std::string& answer);
    void logToRest(const std::string& domain, uint16_t type,
                   const std::string& clientAddr,
                   const std::string& clientMac,
                   DnsLogSource source,
                   bool resolved, const std::string& answer);

    // ─── Async REST logging (fire-and-forget) ──────
    // Log records go into a bounded ring buffer (FreeRTOS queue); on overflow
    // the oldest record is overwritten. A dedicated sender task drains the
    // buffer and POSTs each record with a timeout, so a slow REST server
    // never blocks the DNS task.
    struct RestLogRecord {
        uint32_t ts = 0;      // uptime ms (diagnostics)
        uint16_t type = 0;    // DNS query type (1=A, 28=AAAA)
        bool resolved = false;
        bool stop = false;    // internal: stop marker for the sender task
        uint8_t source = 0;   // 0=local, 1=cache, 2=forwarded
        char domain[128] = {0};
        char client[48] = {0};
        char mac[32] = {0};   // client MAC ("xx:xx:xx:xx:xx:xx", empty if unknown)
        char answer[64] = {0};
    };
    static constexpr int kRestQueueDepth = 48;     // ring buffer capacity
    static constexpr int kRestSenderStack = 8192;  // sender task stack (TLS)
    static constexpr int kRestSenderPriority = 3;
    static constexpr int kRestSendTimeoutMs = 5000;

    void updateRestSenderState();
    void ensureRestSender();
    static void restSenderTask(void* arg);
    std::string resolveUrlHost(const std::string& url) const;
    void sendRestLog(const RestLogRecord& rec);
    std::string buildRestJson(const RestLogRecord& rec) const;

    bool logTerminal_ = false;
    bool logForwarded_ = true;
    bool logLocal_ = true;
    bool logCache_ = true;
    bool logRestSent_ = false;
    bool logRest_ = false;
    std::string logUrl_;
    bool logAuthEnabled_ = false;
    std::string logAuthUser_;
    std::string logAuthPassword_;

    QueueHandle_t restQueue_ = nullptr;
    TaskHandle_t restTask_ = nullptr;
    volatile bool restStopRequested_ = false;
    uint32_t restDropped_ = 0;   // count of records dropped on overflow
    const std::map<std::string, std::vector<std::string>>* localHosts_ = nullptr;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSLOGGER_H
