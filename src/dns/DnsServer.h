#ifndef DHCP_DNS_DNSSERVER_H
#define DHCP_DNS_DNSSERVER_H

#include "IDnsServer.h"
#include "DnsCache.h"
#include "DnsLogger.h"
#include "../dhcp/IDhcpServer.h"

#include <string>
#include <map>
#include <vector>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace dhcp {
namespace dns {

/**
 * @brief DNS proxy server implementation.
 *
 * Processing pipeline for each query:
 *   1. Log the request (terminal + optional REST)
 *   2. Search local hosts file (in-memory map)
 *   3. Search external cache (DnsCache REST client, if enabled)
 *   4. Forward to external DNS server → return result (and store in cache)
 *
 * Listens on UDP port 53 in a dedicated FreeRTOS task.
 */
class DnsServer : public IDnsServer {
public:
    DnsServer();
    ~DnsServer() override;

    // IDnsServer interface
    bool start() override;
    void stop() override;
    DnsServerState state() const override { return state_; }
    bool isRunning() const override { return state_ == DnsServerState::RUNNING; }
    uint32_t queryCount() const override { return queryCount_; }
    std::string stateString() const override;

    /**
     * @brief Add a local host entry (domain → IP).
     * Used for the local hosts file lookup step.
     */
    void addLocalHost(const std::string& domain, const std::string& ip);

    /**
     * @brief Clear all local host entries.
     */
    void clearLocalHosts();

    /**
     * @brief Enable/disable terminal logging at runtime.
     */
    void setLogTerminal(bool enabled);

    /**
     * @brief Re-point the REST logger at the local hosts map.
     *
     * Called at startup and after Local Hosts change at runtime, so the
     * REST URL host resolution always uses the current list (no reboot
     * needed after saving Local Hosts).
     */
    void syncLoggerLocalHosts();

    /**
     * @brief Get references to logger and cache for configuration.
     */
    DnsLogger& logger() { return logger_; }
    DnsCache& cache() { return cache_; }

    /**
     * @brief Provide the DHCP server for the client IP → MAC lookup fallback
     * (used when the ARP cache has no entry for a DNS client).
     */
    void setDhcpServer(::dhcp::dhcp::IDhcpServer* dhcp);

private:
    /**
     * @brief Resolve a client IPv4 (network byte order) to a MAC string.
     *
     * Combined lookup: ARP cache first (lwIP), then the DHCP lease table.
     * Returns "xx:xx:xx:xx:xx:xx" or an empty string if unknown.
     */
    std::string resolveClientMac(uint32_t clientIpNet) const;
    // Task
    static void serverTask(void* arg);
    void serverLoop();

    // DNS message parsing
    bool parseQuery(const uint8_t* buf, size_t len,
                    std::string& domain, uint16_t& type,
                    uint16_t& cls, uint16_t& id);

    // Extract A/AAAA answer IPs from an upstream DNS reply, so the resolved
    // mapping can be stored in the external cache (fire-and-forget).
    void parseForwardAnswer(const uint8_t* buf, size_t len,
                            std::vector<std::string>& ips);

    // DNS message building
    size_t buildAnswer(uint8_t* buf, size_t bufSize,
                       uint16_t id, const std::string& domain,
                       uint16_t type, uint16_t cls,
                       const std::vector<std::string>& ips,
                       uint32_t ttl);

    size_t buildNxdomain(uint8_t* buf, size_t bufSize,
                         uint16_t id, const std::string& domain,
                         uint16_t type, uint16_t cls);

    // Domain name encoding helpers
    static size_t encodeDomainName(uint8_t* dst, const std::string& domain);
    static std::string decodeDomainName(const uint8_t* data, size_t len,
                                        size_t& offset);

    // Async forwarding state — one slot per in-flight client query.
    // Strict cache-first: the external cache is consulted first, and the
    // query is sent to the external DNS ONLY on a cache miss (or cache-wait
    // timeout). A slot has two phases:
    //   phase 1 (waitingCache): awaiting the async external-cache result; the
    //     query is NOT yet sent upstream (external DNS is only queried when
    //     the cache misses, per the cache semantics).
    //   phase 2 (forwarding): the cache missed/timed out and the query was
    //     sent to the external DNS; the client is answered from the first
    //     arriving upstream reply.
    // The main loop uses select() on the listening socket, the sockets of all
    // phase-2 forwards and the cache wakeup socket, so no slow upstream or
    // cache server ever blocks handling of other queries.
    struct PendingForward {
        bool active = false;
        bool waitingCache = true;    // phase 1: awaiting the cache result
        int fwdFd = -1;              // UDP socket used for this forward
        struct sockaddr_in client;   // original client to reply to
        socklen_t clientLen = 0;
        uint16_t qid = 0;
        uint16_t qtype = 0;
        uint16_t qclass = 0;
        std::string domain;          // for NXDOMAIN fallback on timeout
        uint8_t query[1024] = {0};   // raw query (for the delayed forward)
        uint16_t queryLen = 0;
        uint64_t createdMs = 0;      // when the slot was created
        uint64_t sentMs = 0;         // when the query was forwarded (phase 2)
    };
    static constexpr int kMaxPendingForwards = 32;
    static constexpr int kCacheWaitTimeoutMs = 2000;  // cache result cap
    PendingForward pendingForwards_[kMaxPendingForwards];

    // Find a free pending slot, or -1 if all are busy.
    int allocPendingSlot();
    // Close the socket and deactivate a pending forward slot.
    void freePendingSlot(int idx);
    // Reply NXDOMAIN to the pending slot's client and free the slot.
    void expirePendingSlot(int idx);
    // Start the phase-2 upstream forward for a pending slot. Returns false
    // (slot left in cache phase) if the socket/send failed.
    bool startForward(int idx, uint64_t now);
    uint64_t nowMs() const;

    DnsServerState state_ = DnsServerState::STOPPED;
    TaskHandle_t taskHandle_ = nullptr;
    int socketFd_ = -1;
    bool stopRequested_ = false;
    uint32_t queryCount_ = 0;

    // External DNS server address
    uint32_t externalDnsIp_ = 0;

    // Terminal logging flag (gates the per-query ESP_LOGI lines)
    bool logTerminal_ = false;

    // Local hosts (domain → IP)
    std::map<std::string, std::vector<std::string>> localHosts_;

    // DHCP server (for client IP → MAC fallback lookup)
    ::dhcp::dhcp::IDhcpServer* dhcpServer_ = nullptr;

    // Sub-components
    DnsLogger logger_;
    DnsCache cache_;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSSERVER_H
