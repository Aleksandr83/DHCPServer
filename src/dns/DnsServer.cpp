#include "DnsServer.h"
#include "../core/Config.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/prot/etharp.h"

static const char* TAG = "DnsServer";

// ─── DNS protocol constants ─────────────────────────
#define DNS_PORT             53
#define DNS_TYPE_A           1
#define DNS_TYPE_AAAA        28
#define DNS_CLASS_IN         1
#define DNS_FLAG_QR          0x8000
#define DNS_FLAG_RD          0x0100
#define DNS_FLAG_RA          0x0080
#define DNS_RCODE_NOERROR    0
#define DNS_RCODE_NXDOMAIN   3

// Timeout for external DNS forward (ms)
#define DNS_FORWARD_TIMEOUT_MS  1000

// Max DNS message size. Must be >= 1232 (EDNS0) so the Windows resolver's
// large responses are not silently truncated (lwIP truncates without the
// TC flag, which makes Windows reject the reply -> "internet doesn't work"
// while nslookup works). 4096 covers typical EDNS0 responses.
#define DNS_MAX_MSG_SIZE    4096

namespace dhcp {
namespace dns {

// ─────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────

DnsServer::DnsServer()
{
}

DnsServer::~DnsServer()
{
    stop();
    logger_.stopRestSender();
    cache_.stopCacheSender();
    cache_.stopLookupWorker();
}

// ─────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────

bool DnsServer::start()
{
    if (state_ == DnsServerState::RUNNING) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    // Load config
    auto dnsCfg = core::Config::instance().getDns();
    externalDnsIp_ = 0;
    inet_pton(AF_INET, dnsCfg.externalDns.c_str(), &externalDnsIp_);

    logger_.setLogTerminal(dnsCfg.logTerminal);
    logger_.setLogForwarded(dnsCfg.logForwarded);
    logger_.setLogLocal(dnsCfg.logLocal);
    logger_.setLogCache(dnsCfg.logCache);
    logger_.setLogRestSent(dnsCfg.logRestSent);
    logger_.setLogRest(dnsCfg.logRest);
    logger_.setLogUrl(dnsCfg.logUrl);
    logger_.setLogAuth(dnsCfg.logAuthEnabled,
                       dnsCfg.logAuthUser, dnsCfg.logAuthPassword);
    cache_.setEnabled(dnsCfg.cacheRest);
    cache_.setReadEnabled(dnsCfg.cacheRestRead);
    cache_.setWriteEnabled(dnsCfg.cacheRestWrite);
    cache_.setTerminalLogging(dnsCfg.logTerminal);
    cache_.setUrl(dnsCfg.cacheUrl);
    cache_.setAuth(dnsCfg.cacheAuthEnabled,
                   dnsCfg.cacheAuthUser, dnsCfg.cacheAuthPassword);
    // The cache lookup runs in a dedicated worker task so the DNS server
    // task is never blocked on an HTTP request (a synchronous lookup here
    // stalled every query and made sites time out).
    cache_.startLookupWorker();
    logTerminal_ = dnsCfg.logTerminal;

    // Diagnostic: shows the actual saved DNS logging config at startup
    // (helps debugging why no REST/terminal DNS logs appear). cache_auth_user
    // is printed so a saved-but-lost credential is visible right after boot.
    ESP_LOGI(TAG, "DNS log config: terminal=%d rest=%d restSentFilter=%d url=%s auth=%d cache=%d read=%d write=%d cacheUrl=%s cacheAuth=%d cacheUser=%s",
             dnsCfg.logTerminal ? 1 : 0,
             dnsCfg.logRest ? 1 : 0,
             dnsCfg.logRestSent ? 1 : 0,
             dnsCfg.logUrl.empty() ? "-" : dnsCfg.logUrl.c_str(),
             dnsCfg.logAuthEnabled ? 1 : 0,
             dnsCfg.cacheRest ? 1 : 0,
             dnsCfg.cacheRestRead ? 1 : 0,
             dnsCfg.cacheRestWrite ? 1 : 0,
             dnsCfg.cacheUrl.empty() ? "-" : dnsCfg.cacheUrl.c_str(),
             dnsCfg.cacheAuthEnabled ? 1 : 0,
             dnsCfg.cacheAuthUser.empty() ? "-" : dnsCfg.cacheAuthUser.c_str());

    // Load local hosts from config (custom domain → IP mappings)
    localHosts_.clear();
    auto hosts = core::Config::instance().getLocalHosts();
    for (const auto& h : hosts) {
        if (!h.enabled) continue;
        if (!h.ip4.empty()) addLocalHost(h.name, h.ip4);
        if (!h.ip6.empty()) addLocalHost(h.name, h.ip6);
    }
    // Let the REST logger resolve ".lo" style URL hosts against this list.
    syncLoggerLocalHosts();

    state_ = DnsServerState::RUNNING;
    stopRequested_ = false;

    BaseType_t res = xTaskCreatePinnedToCore(
        serverTask, "dns_server", 16384, this,
        configMAX_PRIORITIES - 2, &taskHandle_, 0);

    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        state_ = DnsServerState::ERROR;
        return false;
    }

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);
    return true;
}

void DnsServer::stop()
{
    if (state_ == DnsServerState::STOPPED) return;

    stopRequested_ = true;

    if (taskHandle_) {
        if (socketFd_ >= 0) {
            close(socketFd_);
            socketFd_ = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        taskHandle_ = nullptr;
    }

    state_ = DnsServerState::STOPPED;
    ESP_LOGI(TAG, "DNS server stopped");
}

std::string DnsServer::stateString() const
{
    switch (state_) {
        case DnsServerState::RUNNING: return "running";
        case DnsServerState::STOPPED: return "stopped";
        case DnsServerState::ERROR:   return "error";
        default:                       return "unknown";
    }
}

// ─────────────────────────────────────────────────────
// Local hosts management
// ─────────────────────────────────────────────────────

void DnsServer::addLocalHost(const std::string& domain, const std::string& ip)
{
    localHosts_[domain].push_back(ip);
    ESP_LOGI(TAG, "Local host: %s -> %s", domain.c_str(), ip.c_str());
}

void DnsServer::clearLocalHosts()
{
    localHosts_.clear();
}

void DnsServer::setLogTerminal(bool enabled)
{
    logTerminal_ = enabled;
    logger_.setLogTerminal(enabled);
    cache_.setTerminalLogging(enabled);
    ESP_LOGI(TAG, "Terminal logging %s", enabled ? "enabled" : "disabled");
}

void DnsServer::syncLoggerLocalHosts()
{
    logger_.setLocalHosts(&localHosts_);
}

void DnsServer::setDhcpServer(::dhcp::dhcp::IDhcpServer* dhcp)
{
    dhcpServer_ = dhcp;
}

std::string DnsServer::resolveClientMac(uint32_t clientIpNet) const
{
    char mac[18] = {0};
    uint8_t bytes[6] = {0};

    // 1) ARP cache first — a client that just sent a query is very likely
    //    in the L2 ARP table.
    struct netif* nif = netif_default;
    if (nif && (nif->flags & NETIF_FLAG_UP)) {
        ip4_addr_t ip4;
        ip4.addr = clientIpNet;
        struct eth_addr* eth = nullptr;
        const ip4_addr_t* ipRet = nullptr;  // lwIP asserts both out-params are non-NULL
        if (etharp_find_addr(nif, &ip4, &eth, &ipRet) >= 0 && eth) {
            std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                          eth->addr[0], eth->addr[1], eth->addr[2],
                          eth->addr[3], eth->addr[4], eth->addr[5]);
            return std::string(mac);
        }
    }

    // 2) DHCP lease table fallback.
    if (dhcpServer_ && dhcpServer_->getMacByIp(clientIpNet, bytes)) {
        std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                      bytes[0], bytes[1], bytes[2],
                      bytes[3], bytes[4], bytes[5]);
        return std::string(mac);
    }

    return "";
}

// ─────────────────────────────────────────────────────
// Task
// ─────────────────────────────────────────────────────

void DnsServer::serverTask(void* arg)
{
    DnsServer* self = static_cast<DnsServer*>(arg);
    self->serverLoop();
    vTaskDelete(nullptr);
}

void DnsServer::serverLoop()
{
    // Create UDP socket on port 53
    socketFd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketFd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        state_ = DnsServerState::ERROR;
        return;
    }

    int reuse = 1;
    setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(socketFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed on port %d", DNS_PORT);
        close(socketFd_);
        socketFd_ = -1;
        state_ = DnsServerState::ERROR;
        return;
    }

    ESP_LOGI(TAG, "DNS server listening on port %d", DNS_PORT);

    // Buffers on the heap (not the task stack) so a 4096-byte DNS message
    // does not overflow the 8K stack.
    std::vector<uint8_t> buf(DNS_MAX_MSG_SIZE);
    std::vector<uint8_t> response(DNS_MAX_MSG_SIZE);

    while (!stopRequested_) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socketFd_, &readfds);
        int maxFd = socketFd_;

        // Include sockets of all phase-2 forwards (cache phase has no socket
        // yet) so a slow upstream never blocks handling of other queries.
        for (int i = 0; i < kMaxPendingForwards; i++) {
            auto& pf = pendingForwards_[i];
            if (pf.active && pf.fwdFd >= 0) {
                FD_SET(pf.fwdFd, &readfds);
                if (pf.fwdFd > maxFd) {
                    maxFd = pf.fwdFd;
                }
            }
        }

        // Include the cache lookup wakeup socket (results delivered by the
        // cache worker task; the DNS task must not block on the lookup).
        const int cacheFd = cache_.lookupFd();
        if (cacheFd >= 0) {
            FD_SET(cacheFd, &readfds);
            if (cacheFd > maxFd) maxFd = cacheFd;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int sel = select(maxFd + 1, &readfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            // Never kill the whole DNS task on a transient select error.
            ESP_LOGW(TAG, "select() error errno=%d — continuing", errno);
            continue;
        }

        uint64_t now = nowMs();

        // ── Handle pending forward / cache-check slots ──
        for (int i = 0; i < kMaxPendingForwards; i++) {
            auto& pf = pendingForwards_[i];
            if (!pf.active) continue;

            if (pf.waitingCache) {
                // Phase 1: waiting for the async cache result.
                if (now - pf.createdMs >= kCacheWaitTimeoutMs) {
                    // Cache did not answer in time — treat as a miss and
                    // forward to the external DNS now.
                    if (pf.queryLen > 0 && startForward(i, now)) {
                        if (logTerminal_ && logger_.logForwarded()) {
                            ESP_LOGI(TAG, "DNS forward (cache timeout): %s type=%u",
                                     pf.domain.c_str(), pf.qtype);
                        }
                    } else {
                        expirePendingSlot(i);
                    }
                }
                continue;
            }

            if (pf.fwdFd >= 0 && FD_ISSET(pf.fwdFd, &readfds)) {
                ssize_t rl = recvfrom(pf.fwdFd, response.data(), response.size(), 0,
                                      nullptr, nullptr);
                if (rl > 0) {
                    char clientIp[16];
                    inet_ntop(AF_INET, &pf.client.sin_addr, clientIp,
                              sizeof(clientIp));
                    const std::string clientMac =
                        resolveClientMac(pf.client.sin_addr.s_addr);
                    // Extract A/AAAA answers and store the resolved mapping
                    // in the external cache (fire-and-forget ring buffer +
                    // sender task), so future queries hit the cache.
                    std::vector<std::string> answerIps;
                    parseForwardAnswer(response.data(),
                                       static_cast<size_t>(rl), answerIps);
                    if (!answerIps.empty()) {
                        if (logTerminal_) {
                            ESP_LOGI(TAG, "DNS forward: storing %zu IP(s) of %s to cache",
                                     answerIps.size(), pf.domain.c_str());
                        }
                        cache_.store(pf.domain, pf.qtype, answerIps);
                    } else {
                        if (logTerminal_) {
                            ESP_LOGI(TAG, "DNS forward: no A/AAAA answers for %s type=%u (not cached)",
                                     pf.domain.c_str(), pf.qtype);
                        }
                    }
                    logger_.logQuery(pf.domain, pf.qtype, clientIp, clientMac,
                                     DnsLogSource::FORWARDED, true,
                                     "(forwarded)");
                    if (logTerminal_ && logger_.logForwarded()) {
                        ESP_LOGI(TAG, "DNS query: %s type=%u from %s [forwarded]",
                                 pf.domain.c_str(), pf.qtype, clientIp);
                    }
                    sendto(socketFd_, response.data(), static_cast<size_t>(rl), 0,
                           (struct sockaddr*)&pf.client, pf.clientLen);
                } else {
                    ESP_LOGW(TAG, "recvfrom(fwd) errno=%d", errno);
                }
                freePendingSlot(i);
            } else if (pf.fwdFd >= 0 &&
                       now - pf.sentMs >= DNS_FORWARD_TIMEOUT_MS) {
                // Upstream did not answer in time — fall back to NXDOMAIN
                expirePendingSlot(i);
            }
        }

        // ── Handle async cache lookup results ──
        // A hit answers the client from the cache (no upstream query). A miss
        // starts the phase-2 forward — the external DNS is only queried when
        // the cache does not have the answer.
        {
            const int cFd = cache_.lookupFd();
            if (cFd >= 0 && FD_ISSET(cFd, &readfds)) {
                DnsCache::LookupResult res;
                while (cache_.drainLookupResult(res)) {
                    if (logTerminal_) {
                        ESP_LOGI(TAG, "Cache result: token=%u hit=%d ips=%s",
                                 res.token, res.hit ? 1 : 0, res.ips);
                    }
                    if (res.token >= kMaxPendingForwards) continue;
                    auto& pf = pendingForwards_[res.token];
                    if (!pf.active || !pf.waitingCache) continue;  // moved on

                    if (!res.hit) {
                        // Cache miss → forward to the external DNS now.
                        if (pf.queryLen > 0 && startForward(res.token, now)) {
                            if (logTerminal_ && logger_.logForwarded()) {
                                ESP_LOGI(TAG, "DNS forward (cache miss): %s type=%u",
                                         pf.domain.c_str(), pf.qtype);
                            }
                        } else {
                            expirePendingSlot(res.token);
                        }
                        continue;
                    }

                    // Cache hit — answer the client from the cache.
                    // Split the comma-separated IPs.
                    std::vector<std::string> ips;
                    const std::string list(res.ips);
                    size_t start = 0;
                    while (start <= list.size()) {
                        size_t comma = list.find(',', start);
                        if (comma == std::string::npos) comma = list.size();
                        const std::string ip = list.substr(start, comma - start);
                        if (!ip.empty()) ips.push_back(ip);
                        if (comma == list.size()) break;
                        start = comma + 1;
                    }
                    if (ips.empty()) {
                        // Malformed hit — fall back to forwarding.
                        if (pf.queryLen > 0 && startForward(res.token, now)) {
                            continue;
                        }
                        expirePendingSlot(res.token);
                        continue;
                    }

                    size_t rl = buildAnswer(response.data(), response.size(),
                                            pf.qid, pf.domain, pf.qtype,
                                            pf.qclass, ips, 300);
                    if (rl > 0) {
                        sendto(socketFd_, response.data(), rl, 0,
                               (struct sockaddr*)&pf.client, pf.clientLen);
                    }
                    char clientIp[16];
                    inet_ntop(AF_INET, &pf.client.sin_addr, clientIp,
                              sizeof(clientIp));
                    const std::string clientMac =
                        resolveClientMac(pf.client.sin_addr.s_addr);
                    logger_.logQuery(pf.domain, pf.qtype, clientIp, clientMac,
                                     DnsLogSource::CACHE, true, ips[0]);
                    if (logTerminal_ && logger_.logCache()) {
                        ESP_LOGI(TAG, "DNS query: %s type=%u from %s [cache]",
                                 pf.domain.c_str(), pf.qtype, clientIp);
                    }
                    freePendingSlot(res.token);
                }
            }
        }

        // ── Handle new client query ──
        if (FD_ISSET(socketFd_, &readfds)) {
            struct sockaddr_in from;
            socklen_t fromLen = sizeof(from);

            ssize_t recvLen = recvfrom(socketFd_, buf.data(), buf.size(), 0,
                                       (struct sockaddr*)&from, &fromLen);
            if (recvLen < 0) {
                ESP_LOGW(TAG, "recvfrom(main) errno=%d — continuing", errno);
                continue;
            }

            queryCount_++;

            // Parse query
            std::string domain;
            uint16_t qtype = 0, qclass = 0, qid = 0;
            if (!parseQuery(buf.data(), static_cast<size_t>(recvLen),
                            domain, qtype, qclass, qid)) {
                ESP_LOGW(TAG, "Failed to parse DNS query (%d bytes)",
                         (int)recvLen);
                continue;
            }

            // Client IP string
            char clientIpStr[16];
            inet_ntop(AF_INET, &from.sin_addr, clientIpStr, sizeof(clientIpStr));
            // Client MAC: ARP cache first, DHCP lease table fallback.
            const std::string clientMac = resolveClientMac(from.sin_addr.s_addr);

            // Step 1: Search local hosts (user-assigned). DNS names are
            // case-insensitive and may arrive with a trailing root dot, so
            // normalize before the map lookup.
            bool found = false;
            bool fromLocal = false;
            std::vector<std::string> resolvedIps;
            std::string lookupDomain = domain;
            for (auto& c : lookupDomain) c = static_cast<char>(tolower((unsigned char)c));
            if (!lookupDomain.empty() && lookupDomain.back() == '.') {
                lookupDomain.pop_back();
            }
            auto it = localHosts_.find(lookupDomain);
            if (it != localHosts_.end() && !it->second.empty()) {
                found = true;
                fromLocal = true;
                // Return only records matching the queried type: A -> IPv4,
                // AAAA -> IPv6. Answering e.g. an A record to an AAAA query is
                // rejected by strict resolvers (Android). Other types (HTTPS/SVCB
                // 64/65, MX, ...) get an empty answer (NODATA) so the client
                // falls back to A/AAAA.
                if (qtype == DNS_TYPE_A || qtype == DNS_TYPE_AAAA) {
                    for (const auto& ip : it->second) {
                        bool isV6 = ip.find(':') != std::string::npos;
                        if (qtype == DNS_TYPE_AAAA ? isV6 : !isV6) {
                            resolvedIps.push_back(ip);
                        }
                    }
                }
                if (logTerminal_ && logger_.logLocal()) {
                    ESP_LOGI(TAG, "DNS query: %s type=%u from %s [local]",
                             domain.c_str(), qtype, clientIpStr);
                }
            }

            // Note: the external cache is consulted ASYNCHRONOUSLY — see the
            // "Handle async cache lookup results" section and the forward
            // path below (cache_.submitLookup). A synchronous lookup here
            // would block the whole DNS server task for ~1.6 s per query.

            // Build and send response
            size_t respLen = 0;
            if (found && !resolvedIps.empty()) {
                respLen = buildAnswer(response.data(), response.size(),
                                      qid, domain, qtype, qclass,
                                      resolvedIps, 300);
                logger_.logQuery(domain, qtype, clientIpStr, clientMac,
                                 fromLocal ? DnsLogSource::LOCAL : DnsLogSource::CACHE,
                                 true, resolvedIps[0]);
                if (respLen > 0) {
                    sendto(socketFd_, response.data(), respLen, 0,
                           (struct sockaddr*)&from, fromLen);
                }
            } else if (found) {
                // Host exists but has no record of the requested type — answer
                // NODATA (NOERROR, 0 records) so the client falls back to
                // another type instead of failing. Do not forward.
                respLen = buildAnswer(response.data(), response.size(),
                                      qid, domain, qtype, qclass, {}, 300);
                logger_.logQuery(domain, qtype, clientIpStr, clientMac,
                                 fromLocal ? DnsLogSource::LOCAL : DnsLogSource::CACHE,
                                 false, "");
                if (respLen > 0) {
                    sendto(socketFd_, response.data(), respLen, 0,
                           (struct sockaddr*)&from, fromLen);
                }
            } else {
                // Step 3: check the external cache FIRST (async). The query is
                // sent to the external DNS ONLY when the cache misses (see the
                // cache-result handler / cache-wait timeout above). The DNS
                // task is never blocked on HTTP — the lookup runs in a worker.
                int slot = allocPendingSlot();
                if (slot >= 0) {
                    auto& pf = pendingForwards_[slot];
                    pf.active = true;
                    pf.waitingCache = true;
                    pf.fwdFd = -1;
                    pf.client = from;
                    pf.clientLen = fromLen;
                    pf.qid = qid;
                    pf.qtype = qtype;
                    pf.qclass = qclass;
                    pf.domain = domain;
                    pf.createdMs = now;
                    pf.sentMs = 0;
                    if (recvLen <= static_cast<ssize_t>(sizeof(pf.query))) {
                        memcpy(pf.query, buf.data(), static_cast<size_t>(recvLen));
                        pf.queryLen = static_cast<uint16_t>(recvLen);
                    } else {
                        pf.queryLen = 0;  // too large to buffer
                    }

                    if (pf.queryLen > 0 && cache_.usable()) {
                        cache_.submitLookup(domain, qtype,
                                            static_cast<uint8_t>(slot));
                        if (logTerminal_ && logger_.logForwarded()) {
                            ESP_LOGI(TAG, "DNS cache-check: %s type=%u from %s",
                                     domain.c_str(), qtype, clientIpStr);
                        }
                    } else {
                        // Cache disabled/oversized query → forward directly.
                        if (startForward(slot, now)) {
                            if (logTerminal_ && logger_.logForwarded()) {
                                ESP_LOGI(TAG, "DNS forward: %s type=%u from %s",
                                         domain.c_str(), qtype, clientIpStr);
                            }
                        } else {
                            freePendingSlot(slot);
                            respLen = buildNxdomain(response.data(), response.size(),
                                                    qid, domain, qtype, qclass);
                            logger_.logQuery(domain, qtype, clientIpStr, clientMac,
                                             DnsLogSource::FORWARDED, false, "");
                            if (respLen > 0) {
                                sendto(socketFd_, response.data(), respLen, 0,
                                       (struct sockaddr*)&from, fromLen);
                            }
                        }
                    }
                } else {
                    // All slots busy — answer NXDOMAIN rather than blocking
                    // the loop.
                    respLen = buildNxdomain(response.data(), response.size(),
                                            qid, domain, qtype, qclass);
                    logger_.logQuery(domain, qtype, clientIpStr, clientMac,
                                     DnsLogSource::FORWARDED, false, "");
                    if (respLen > 0) {
                        sendto(socketFd_, response.data(), respLen, 0,
                               (struct sockaddr*)&from, fromLen);
                    }
                }
            }
        }
    }

    // Clean up all pending forwards
    for (int i = 0; i < kMaxPendingForwards; i++) {
        if (pendingForwards_[i].active) freePendingSlot(i);
    }

    if (socketFd_ >= 0) {
        close(socketFd_);
        socketFd_ = -1;
    }
}

// ─────────────────────────────────────────────────────
// Async forward slot helpers
// ─────────────────────────────────────────────────────

int DnsServer::allocPendingSlot()
{
    for (int i = 0; i < kMaxPendingForwards; i++) {
        if (!pendingForwards_[i].active) return i;
    }
    return -1;
}

void DnsServer::freePendingSlot(int idx)
{
    if (idx < 0 || idx >= kMaxPendingForwards) return;
    auto& pf = pendingForwards_[idx];
    if (pf.fwdFd >= 0) {
        close(pf.fwdFd);
        pf.fwdFd = -1;
    }
    pf.active = false;
    pf.domain.clear();
}

void DnsServer::expirePendingSlot(int idx)
{
    if (idx < 0 || idx >= kMaxPendingForwards) return;
    auto& pf = pendingForwards_[idx];

    char clientIp[16];
    inet_ntop(AF_INET, &pf.client.sin_addr, clientIp, sizeof(clientIp));
    const std::string clientMac = resolveClientMac(pf.client.sin_addr.s_addr);
    logger_.logQuery(pf.domain, pf.qtype, clientIp, clientMac,
                     DnsLogSource::FORWARDED, false, "");
    if (logTerminal_ && logger_.logForwarded()) {
        ESP_LOGW(TAG, "DNS forward timeout: %s type=%u from %s",
                 pf.domain.c_str(), pf.qtype, clientIp);
    }

    std::vector<uint8_t> nx(DNS_MAX_MSG_SIZE);
    size_t len = buildNxdomain(nx.data(), nx.size(), pf.qid, pf.domain,
                               pf.qtype, pf.qclass);
    if (len > 0) {
        sendto(socketFd_, nx.data(), len, 0,
               (struct sockaddr*)&pf.client, pf.clientLen);
    }
    freePendingSlot(idx);
}

uint64_t DnsServer::nowMs() const
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

// Start the phase-2 upstream forward for a pending slot (called either
// directly when the cache is disabled, or after a cache miss / cache wait
// timeout). Transitions the slot from waitingCache to forwarding.
bool DnsServer::startForward(int idx, uint64_t now)
{
    if (idx < 0 || idx >= kMaxPendingForwards) return false;
    auto& pf = pendingForwards_[idx];
    if (!pf.active || pf.queryLen == 0) return false;

    pf.fwdFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (pf.fwdFd < 0) {
        ESP_LOGW(TAG, "Failed to create forward socket for %s",
                 pf.domain.c_str());
        return false;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DNS_PORT);
    dest.sin_addr.s_addr = externalDnsIp_;

    ssize_t sent = sendto(pf.fwdFd, pf.query, pf.queryLen, 0,
                          (struct sockaddr*)&dest, sizeof(dest));
    if (sent < 0) {
        ESP_LOGW(TAG, "Failed to forward DNS query for %s", pf.domain.c_str());
        close(pf.fwdFd);
        pf.fwdFd = -1;
        return false;
    }

    pf.waitingCache = false;
    pf.sentMs = now;
    return true;
}

// ─────────────────────────────────────────────────────
// DNS message parsing

// ─────────────────────────────────────────────────────
// DNS message parsing
// ─────────────────────────────────────────────────────

bool DnsServer::parseQuery(const uint8_t* buf, size_t len,
                            std::string& domain, uint16_t& type,
                            uint16_t& cls, uint16_t& id)
{
    if (len < 12) return false;

    // Header
    id = (buf[0] << 8) | buf[1];
    uint16_t flags = (buf[2] << 8) | buf[3];

    // Must be a standard query (QR=0, Opcode=0)
    if (flags & 0x8000) return false;

    uint16_t qdcount = (buf[4] << 8) | buf[5];
    if (qdcount == 0) return false;

    // Decode question name
    size_t offset = 12;
    domain = decodeDomainName(buf, offset);
    if (domain.empty()) return false;

    // QTYPE and QCLASS
    if (offset + 4 > len) return false;
    type = (buf[offset] << 8) | buf[offset + 1];
    cls = (buf[offset + 2] << 8) | buf[offset + 3];

    return true;
}

// Extract A/AAAA answer IPs from an upstream DNS reply. Only the answer
// section is walked (names may use compression pointers). Returns the IPs
// so the DNS server can store the resolved mapping in the external cache.
void DnsServer::parseForwardAnswer(const uint8_t* buf, size_t len,
                                   std::vector<std::string>& ips)
{
    if (len < 12) return;
    const uint16_t qdcount = (buf[4] << 8) | buf[5];
    const uint16_t ancount = (buf[6] << 8) | buf[7];
    if (logTerminal_) ESP_LOGI(TAG, "parseForwardAnswer: len=%u qd=%u an=%u",
                               static_cast<unsigned>(len), qdcount, ancount);

    size_t offset = 12;
    // Skip the question section (each question = name + 4 bytes).
    for (uint16_t q = 0; q < qdcount && offset < len; ++q) {
        decodeDomainName(buf, offset);
        offset += 4;
    }

    // Walk the answer records.
    for (uint16_t a = 0; a < ancount && offset + 10 <= len; ++a) {
        decodeDomainName(buf, offset);   // NAME (may be a compression pointer)
        if (offset + 10 > len) return;
        const uint16_t type = (buf[offset] << 8) | buf[offset + 1];
        const uint16_t rdlen = (buf[offset + 8] << 8) | buf[offset + 9];
        offset += 10;
        if (offset + rdlen > len) return;

        if (type == DNS_TYPE_A && rdlen == 4) {
            char ip[16];
            std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                          buf[offset], buf[offset + 1],
                          buf[offset + 2], buf[offset + 3]);
            if (logTerminal_) ESP_LOGI(TAG, "parseForwardAnswer: A %s", ip);
            ips.push_back(ip);
        } else if (type == DNS_TYPE_AAAA && rdlen == 16) {
            char ip[INET6_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET6, buf + offset, ip, sizeof(ip))) {
                if (logTerminal_) ESP_LOGI(TAG, "parseForwardAnswer: AAAA %s", ip);
                ips.push_back(ip);
            }
        }
        offset += rdlen;
    }
    if (logTerminal_) ESP_LOGI(TAG, "parseForwardAnswer: %u IP(s) extracted",
                               static_cast<unsigned>(ips.size()));
}

// ─────────────────────────────────────────────────────
// DNS message building
// ─────────────────────────────────────────────────────

size_t DnsServer::buildAnswer(uint8_t* buf, size_t bufSize,
                               uint16_t id, const std::string& domain,
                               uint16_t type, uint16_t cls,
                               const std::vector<std::string>& ips,
                               uint32_t ttl)
{
    size_t pos = 0;

    // Header
    if (pos + 12 > bufSize) return 0;
    buf[pos++] = (id >> 8) & 0xFF;
    buf[pos++] = id & 0xFF;
    buf[pos++] = (DNS_FLAG_QR | DNS_FLAG_RD | DNS_FLAG_RA) >> 8;
    buf[pos++] = (DNS_FLAG_QR | DNS_FLAG_RD | DNS_FLAG_RA) & 0xFF;
    // QDCOUNT = 1
    buf[pos++] = 0; buf[pos++] = 1;
    // ANCOUNT = number of IPs
    uint16_t ancount = static_cast<uint16_t>(ips.size());
    buf[pos++] = (ancount >> 8) & 0xFF;
    buf[pos++] = ancount & 0xFF;
    // NSCOUNT = 0
    buf[pos++] = 0; buf[pos++] = 0;
    // ARCOUNT = 0
    buf[pos++] = 0; buf[pos++] = 0;

    // Question section (copy original domain + type + class)
    size_t encLen = encodeDomainName(buf + pos, domain);
    pos += encLen;

    if (pos + 4 > bufSize) return 0;
    buf[pos++] = (type >> 8) & 0xFF;
    buf[pos++] = type & 0xFF;
    buf[pos++] = (cls >> 8) & 0xFF;
    buf[pos++] = cls & 0xFF;

    // Answer section — one RR per IP
    for (const auto& ip : ips) {
        if (pos + 16 > bufSize) break;

        // NAME = pointer to domain in question
        buf[pos++] = 0xC0;
        buf[pos++] = 0x0C; // offset 12

        // TYPE: derive from the address format (A for IPv4, AAAA for IPv6)
        // so a host is answered with whatever records it actually has.
        bool isV6 = ip.find(':') != std::string::npos;
        uint16_t rrType = isV6 ? DNS_TYPE_AAAA : DNS_TYPE_A;
        buf[pos++] = (rrType >> 8) & 0xFF;
        buf[pos++] = rrType & 0xFF;

        // CLASS
        buf[pos++] = (cls >> 8) & 0xFF;
        buf[pos++] = cls & 0xFF;

        // TTL
        uint32_t ttlN = htonl(ttl);
        memcpy(buf + pos, &ttlN, 4);
        pos += 4;

        // RDLENGTH + RDATA
        if (rrType == DNS_TYPE_A) {
            buf[pos++] = 0; buf[pos++] = 4; // length 4
            uint32_t ipAddr;
            inet_pton(AF_INET, ip.c_str(), &ipAddr);
            memcpy(buf + pos, &ipAddr, 4);
            pos += 4;
        } else { // AAAA
            buf[pos++] = 0; buf[pos++] = 16; // length 16
            inet_pton(AF_INET6, ip.c_str(), buf + pos);
            pos += 16;
        }
    }

    return pos;
}

size_t DnsServer::buildNxdomain(uint8_t* buf, size_t bufSize,
                                 uint16_t id, const std::string& domain,
                                 uint16_t type, uint16_t cls)
{
    size_t pos = 0;

    // Header
    if (pos + 12 > bufSize) return 0;
    buf[pos++] = (id >> 8) & 0xFF;
    buf[pos++] = id & 0xFF;
    buf[pos++] = (DNS_FLAG_QR | DNS_FLAG_RD | DNS_FLAG_RA) >> 8;
    buf[pos++] = (DNS_FLAG_QR | DNS_FLAG_RD | DNS_FLAG_RA | DNS_RCODE_NXDOMAIN) & 0xFF;
    buf[pos++] = 0; buf[pos++] = 1; // QDCOUNT
    buf[pos++] = 0; buf[pos++] = 0; // ANCOUNT
    buf[pos++] = 0; buf[pos++] = 0; // NSCOUNT
    buf[pos++] = 0; buf[pos++] = 0; // ARCOUNT

    // Question
    size_t encLen = encodeDomainName(buf + pos, domain);
    pos += encLen;

    if (pos + 4 > bufSize) return 0;
    buf[pos++] = (type >> 8) & 0xFF;
    buf[pos++] = type & 0xFF;
    buf[pos++] = (cls >> 8) & 0xFF;
    buf[pos++] = cls & 0xFF;

    return pos;
}

// ─────────────────────────────────────────────────────
// Domain name encoding / decoding
// ─────────────────────────────────────────────────────

size_t DnsServer::encodeDomainName(uint8_t* dst, const std::string& domain)
{
    size_t pos = 0;
    size_t labelStart = 0;

    while (labelStart < domain.length()) {
        size_t dotPos = domain.find('.', labelStart);
        size_t labelLen = (dotPos == std::string::npos)
                          ? domain.length() - labelStart
                          : dotPos - labelStart;

        if (labelLen > 63) labelLen = 63;

        dst[pos++] = static_cast<uint8_t>(labelLen);
        memcpy(dst + pos, domain.c_str() + labelStart, labelLen);
        pos += labelLen;

        if (dotPos == std::string::npos) break;
        labelStart = dotPos + 1;
    }

    dst[pos++] = 0; // root label
    return pos;
}

std::string DnsServer::decodeDomainName(const uint8_t* data, size_t& offset)
{
    std::string domain;
    bool jumped = false;
    size_t pos = offset;

    while (true) {
        uint8_t len = data[pos];
        if (len == 0) {
            pos++;
            break;
        }
        // Check for compression pointer (top 2 bits = 11)
        if ((len & 0xC0) == 0xC0) {
            if (!jumped) {
                offset = pos + 2;
                jumped = true;
            }
            pos = ((len & 0x3F) << 8) | data[pos + 1];
            continue;
        }
        pos++;
        if (!domain.empty()) domain += '.';
        domain.append(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
    }

    if (!jumped) {
        offset = pos;
    }

    return domain;
}

} // namespace dns
} // namespace dhcp
