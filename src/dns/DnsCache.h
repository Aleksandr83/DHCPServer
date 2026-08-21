#ifndef DHCP_DNS_DNSCACHE_H
#define DHCP_DNS_DNSCACHE_H

#include <string>
#include <vector>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace dhcp {
namespace dns {

/**
 * @brief External DNS cache REST client.
 *
 * Implements the server contract (see Docs/Rest.md):
 *   - lookup (read):  GET  {url}/{domain}
 *                     → 200 {"domain":...,"ips":["1.2.3.4",...],"type":N,...}
 *                     → 404 (not found / expired)
 *   - store (upsert): PUT  {url}/{domain}  body {"ips":[...],"type":N}
 *                     → 200
 *
 * Store is fire-and-forget (ring buffer + sender task, same as DnsLogger).
 * Lookup is ASYNC: the DNS server task must never block on HTTP, so
 * lookups are submitted via submitLookup() and performed by a dedicated
 * worker task; results are delivered back to the DNS task through a UDP
 * loopback wakeup socket (lookupFd() / drainLookupResult()). The DNS server
 * forwards the query to the external DNS in parallel and answers the client
 * from whichever responds first.
 *
 * The cache is active only when enabled() is true AND url() is non-empty.
 */
class DnsCache {
public:
    DnsCache();
    ~DnsCache();

    /**
     * @brief Enable/disable the external cache (master switch).
     */
    void setEnabled(bool enabled);

    bool enabled() const { return enabled_; }

    /**
     * @brief Enable/disable reading from the cache (lookup).
     */
    void setReadEnabled(bool enabled);
    bool readEnabled() const { return readEnabled_; }

    /**
     * @brief Enable/disable writing to the cache (store on forward).
     */
    void setWriteEnabled(bool enabled);
    bool writeEnabled() const { return writeEnabled_; }

    /**
     * @brief Enable/disable verbose terminal logging (mirrors the DNS
     * server's "Log to Terminal" setting). When off, informational cache
     * lines are suppressed; warnings/errors still print.
     */
    void setTerminalLogging(bool enabled);

    /**
     * @brief Whether the cache is usable for READING: enabled, reading is
     * on, a URL is set, AND the circuit breaker is not open (not bypassing
     * a slow/failing server).
     */
    bool usable() const {
        return enabled_ && readEnabled_ && !url_.empty() && !isDegraded();
    }

    /**
     * @brief Set the external cache service URL (e.g.
     *        "https://dhcpserverweb.lo/api/v1/dns-cache").
     */
    void setUrl(const std::string& url);

    /**
     * @brief Set HTTP Basic auth credentials for cache REST requests.
     *
     * @param enabled Whether to send the Authorization header.
     * @param user    Basic auth username.
     * @param pass    Basic auth password.
     */
    void setAuth(bool enabled, const std::string& user,
                 const std::string& pass);

    bool authEnabled() const { return authEnabled_; }
    const std::string& authUser() const { return authUser_; }
    const std::string& authPassword() const { return authPassword_; }

    /**
     * @brief Get the configured URL.
     */
    const std::string& url() const { return url_; }

    /**
     * @brief Non-blocking submit of an async cache lookup.
     *
     * The lookup is performed by a dedicated worker task (started by
     * startLookupWorker()); the result is delivered back via the wakeup
     * socket with the same @p token so the caller can correlate it.
     * No-op if the cache is disabled, the URL is empty, or the worker is
     * not running.
     */
    void submitLookup(const std::string& domain, uint16_t type, uint8_t token);

    /**
     * @brief Start the async lookup worker task + wakeup socket.
     * Idempotent. Call once at DNS server startup.
     */
    void startLookupWorker();

    /**
     * @brief Stop the async lookup worker task.
     */
    void stopLookupWorker();

    /**
     * @brief UDP wakeup socket the DNS server task selects on.
     * @return fd, or -1 if the worker is not running.
     */
    int lookupFd() const { return lookupFd_; }

    /**
     * @brief A completed cache lookup result (echoed back to the caller).
     */
    struct LookupResult {
        uint8_t token = 0;          // caller token (e.g. forward slot index)
        bool hit = false;           // cache returned a matching answer
        char ips[256] = {0};        // comma-separated matching IPs if hit
    };

    /**
     * @brief Non-blocking read of one pending lookup result.
     * @return true if a result was read into @p out.
     */
    bool drainLookupResult(LookupResult& out);

    /**
     * @brief Store a domain→IP mapping in the external cache (upsert).
     *
     * Fire-and-forget: the record is pushed into a bounded ring buffer and
     * POSTed by a dedicated sender task. No-op if the cache is disabled or
     * the URL is empty. Never blocks the caller.
     */
    void store(const std::string& domain, uint16_t type,
               const std::vector<std::string>& ips);

    /**
     * @brief Stop the async store sender task.
     *
     * Wakes the sender (if blocked on the queue), waits for it to exit and
     * marks it as stopped. No-op if it is not running.
     */
    void stopCacheSender();

private:
    static std::string normalizeDomain(const std::string& domain);
    static std::string urlEncode(const std::string& s);

    // Performs one lookup (GET + parse + type filter). Returns true on hit.
    // Runs inside the lookup worker task (never the DNS server task).
    bool doLookupAndParse(const std::string& domain, uint16_t type,
                          std::vector<std::string>& result);
    // Returns the HTTP status code (0 = transport/init failure).
    int doLookup(const std::string& url, std::string& respBody);

    // ─── Async store (fire-and-forget) ──────────────
    struct CacheStoreRecord {
        bool stop = false;    // internal: stop marker for the sender task
        uint16_t type = 0;    // DNS query type (1=A, 28=AAAA)
        char domain[128] = {0};
        char ips[256] = {0};  // comma-separated IP list (no spaces)
    };
    static constexpr int kStoreQueueDepth = 32;      // ring buffer capacity
    static constexpr int kStoreSenderStack = 8192;   // sender task stack (TLS)
    static constexpr int kStoreSenderPriority = 3;
    static constexpr int kStoreSendTimeoutMs = 5000;
    // Per-lookup socket timeout. Must exceed the cache server's response
    // time (~0.9 s here) so lookups complete as 404 misses instead of
    // failing with ESP_ERR_HTTP_EAGAIN.
    static constexpr int kLookupTimeoutMs = 3000;

    void updateCacheSenderState();
    void ensureCacheSender();
    static void cacheSenderTask(void* arg);
    void sendStore(const CacheStoreRecord& rec);
    std::string buildStoreJson(const CacheStoreRecord& rec) const;

    bool enabled_ = false;
    bool readEnabled_ = true;
    bool writeEnabled_ = true;
    bool terminalLogging_ = true;   // mirrors DNS "Log to Terminal"
    std::string url_;
    bool authEnabled_ = false;
    std::string authUser_;
    std::string authPassword_;

    // ─── Async lookup (worker task + wakeup socket) ─
    struct LookupRequest {
        uint8_t token = 0;
        uint16_t type = 0;
        char domain[128] = {0};
    };
    static constexpr int kLookupQueueDepth = 16;
    static constexpr int kLookupWorkerStack = 8192;   // TLS in the worker
    static constexpr int kLookupWorkerPriority = 3;
    void ensureLookupWorker();
    static void lookupWorkerTask(void* arg);
    void lookupWorkerLoop();

    QueueHandle_t lookupQueue_ = nullptr;
    TaskHandle_t lookupTask_ = nullptr;
    volatile bool lookupStopRequested_ = false;
    int lookupFd_ = -1;        // UDP wakeup socket (DNS task selects on it)
    uint16_t lookupPort_ = 0;  // loopback port of lookupFd_

    // ─── Lookup dedup ───────────────────────────────
    // Domains that currently have an in-flight lookup, so repeated queries
    // for the same domain don't each fire an HTTP GET to the cache server.
    static constexpr int kMaxPendingDomains = 16;
    char pendingDomains_[kMaxPendingDomains][128] = {{0}};
    int pendingCount_ = 0;
    portMUX_TYPE pendingLock_ = portMUX_INITIALIZER_UNLOCKED;
    // Returns true if the lookup should proceed (domain not already pending
    // or the dedup table is full). False = already pending, skip.
    bool markPending(const std::string& d);
    void clearPending(const std::string& d);

    // ─── Cache circuit breaker ──────────────────────
    // If the cache server is slow (lookup > kSlowLookupMs) or failing, open
    // the breaker and bypass the cache for kDegradeCooldownMs, so a slow
    // cache can never degrade DNS. After the cooldown a probe lookup is
    // allowed; if it is still slow the breaker re-opens.
    static constexpr int64_t kSlowLookupMs = 800;      // slower = "bad"
    static constexpr int kFailThreshold = 3;           // consecutive bad
    static constexpr int64_t kDegradeCooldownMs = 30000;
    uint32_t lookupBadCount_ = 0;            // worker-only, no volatile needed
    volatile int64_t degradedUntilMs_ = 0;   // esp_timer ms, 0 = healthy
    bool isDegraded() const;
    void notifyLookupOutcome(int64_t elapsedMs);

    QueueHandle_t storeQueue_ = nullptr;
    TaskHandle_t storeTask_ = nullptr;
    volatile bool storeStopRequested_ = false;
    uint32_t storeDropped_ = 0;   // count of records dropped on overflow
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSCACHE_H
