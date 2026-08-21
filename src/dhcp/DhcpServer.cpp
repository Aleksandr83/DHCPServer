#include "DhcpServer.h"
#include "../core/Config.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "apps/ping/ping_sock.h"
#include "freertos/semphr.h"

static const char* TAG = "DhcpServer";

// Helper macro for IP address formatting (replacement for lwip IPSTR/IP2STR)
#define IP_FMT             "%d.%d.%d.%d"
#define IP_FMT_ARGS(ip)    ((unsigned)((uint8_t*)&(ip))[0]), \
                           ((unsigned)((uint8_t*)&(ip))[1]), \
                           ((unsigned)((uint8_t*)&(ip))[2]), \
                           ((unsigned)((uint8_t*)&(ip))[3])

// ─── DHCP protocol constants ─────────────────────────
#define DHCP_SERVER_PORT      67
#define DHCP_CLIENT_PORT      68
#define DHCP_MAGIC_COOKIE     0x63825363

// DHCP message types
#define DHCP_DISCOVER         1
#define DHCP_OFFER            2
#define DHCP_REQUEST          3
#define DHCP_DECLINE          4
#define DHCP_ACK              5
#define DHCP_NAK              6
#define DHCP_RELEASE          7

// DHCP option codes
#define DHCP_OPT_PAD          0
#define DHCP_OPT_SUBNET_MASK  1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS_SERVER   6
#define DHCP_OPT_HOSTNAME     12
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_REQ_ADDR     50
#define DHCP_OPT_END          255

// Minimum DHCP message size: op..cookie = 240 bytes
#define DHCP_MIN_MSGSIZE      240

// ─── DHCP message structure ──────────────────────────
#pragma pack(push, 1)
struct DhcpMessage {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    char     sname[64];
    char     file[128];
    uint32_t cookie;
    uint8_t  options[308];
};
#pragma pack(pop)

static_assert(sizeof(DhcpMessage) == 548, "DhcpMessage must be 548 bytes");

namespace dhcp {
namespace dhcp {

// ─────────────────────────────────────────────────────
// ARP conflict probe (runs on the lwIP tcpip thread)
// ─────────────────────────────────────────────────────

struct ArpProbeCtx {
    struct netif* nif;
    ip4_addr_t ip;
    SemaphoreHandle_t done;
    int found;        // 1 if a device answered (IP in ARP cache)
    int macValid;     // 1 if ownerMac is valid
    uint8_t ownerMac[6];
    int stage;        // 0 = send ARP request, 1 = check ARP cache
};

static void arpProbeCb(void* arg)
{
    ArpProbeCtx* c = static_cast<ArpProbeCtx*>(arg);
    if (c->stage == 0) {
        etharp_request(c->nif, &c->ip);
    } else {
        struct eth_addr* mac = nullptr;
        const ip4_addr_t* resolvedIp = nullptr;
        if (etharp_find_addr(c->nif, &c->ip, &mac, &resolvedIp) == 1 && mac != nullptr) {
            memcpy(c->ownerMac, mac->addr, 6);
            c->found = 1;
            c->macValid = 1;
        } else {
            c->found = 0;
            c->macValid = 0;
        }
    }
    xSemaphoreGive(c->done);
}

bool DhcpServer::probeIp(uint32_t ip, uint8_t ownerMac[6]) const
{
    // ARP probe first — it also gives us the owner MAC when the device is on
    // the local link.
    if (arpProbeIp(ip, ownerMac)) {
        return true;
    }
    // ARP may miss devices that are reachable only via routing / behind a
    // bridge (they answer ICMP but not ARP on our segment). Complement with an
    // ICMP echo probe.
    if (icmpProbeIp(ip)) {
        // The device answered ICMP; fetch its MAC from the ARP cache (the ping
        // forced ARP resolution on the local link). If unavailable, the owner
        // is reported as unknown.
        uint8_t mac[6];
        if (arpProbeIp(ip, mac)) {
            if (ownerMac != nullptr) memcpy(ownerMac, mac, 6);
        } else if (ownerMac != nullptr) {
            memset(ownerMac, 0, 6);
        }
        return true;
    }
    return false;
}

bool DhcpServer::arpProbeIp(uint32_t ip, uint8_t ownerMac[6]) const
{
    // ARP-probe the candidate IP to detect conflicts (RFC 5227). All lwIP
    // core access is done via tcpip_callback because CONFIG_LWIP_TCPIP_CORE_
    // LOCKING is off in this build. ownerMac (optional) receives the MAC of
    // the device that currently holds the IP, when one answers.
    esp_netif_t* en = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!en) return false;
    int nifIdx = esp_netif_get_netif_impl_index(en);
    if (nifIdx < 0) return false;
    struct netif* nif = netif_get_by_index(static_cast<uint8_t>(nifIdx));
    if (!nif) return false;

    ArpProbeCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.nif = nif;
    ip4_addr_set_u32(&ctx.ip, ip);
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) return false;

    // Stage 0: send ARP request for the candidate IP
    ctx.stage = 0;
    err_t cbErr = tcpip_callback(arpProbeCb, &ctx);
    if (cbErr == ERR_OK) {
        xSemaphoreTake(ctx.done, pdMS_TO_TICKS(1000));
    } else {
        ESP_LOGW(TAG, "ARP probe: tcpip_callback failed (stage 0, err=%d)", cbErr);
    }
    // Give the owner time to answer (tcpip thread processes the reply)
    vTaskDelay(pdMS_TO_TICKS(200));

    // Stage 1: check whether the IP now sits in the ARP cache
    ctx.stage = 1;
    cbErr = tcpip_callback(arpProbeCb, &ctx);
    if (cbErr == ERR_OK) {
        xSemaphoreTake(ctx.done, pdMS_TO_TICKS(1000));
    } else {
        ESP_LOGW(TAG, "ARP probe: tcpip_callback failed (stage 1, err=%d)", cbErr);
    }

    bool inUse = ctx.found != 0;
    if (inUse && ownerMac != nullptr) {
        memcpy(ownerMac, ctx.ownerMac, 6);
    }
    vSemaphoreDelete(ctx.done);
    if (logTerminal_) {
        ESP_LOGI(TAG, "ARP probe " IP_FMT ": %s", IP_FMT_ARGS(ip),
                 inUse ? "in use" : "free");
    }
    return inUse;
}

// ─────────────────────────────────────────────────────
// ICMP conflict probe (esp_ping)
// ─────────────────────────────────────────────────────

struct PingProbeCtx {
    SemaphoreHandle_t done;
    int success;
};

static void pingSuccessCb(esp_ping_handle_t hdl, void* args)
{
    PingProbeCtx* c = static_cast<PingProbeCtx*>(args);
    c->success = 1;
}

static void pingEndCb(esp_ping_handle_t hdl, void* args)
{
    PingProbeCtx* c = static_cast<PingProbeCtx*>(args);
    xSemaphoreGive(c->done);
}

bool DhcpServer::icmpProbeIp(uint32_t ip) const
{
    esp_netif_t* en = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!en) return false;
    int nifIdx = esp_netif_get_netif_impl_index(en);
    if (nifIdx < 0) return false;

    PingProbeCtx ctx;
    ctx.done = xSemaphoreCreateBinary();
    ctx.success = 0;
    if (!ctx.done) return false;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 1;
    cfg.interval_ms = 0;
    cfg.timeout_ms = 800;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip;
    cfg.interface = static_cast<uint32_t>(nifIdx);

    esp_ping_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cb_args = &ctx;
    cbs.on_ping_success = pingSuccessCb;
    cbs.on_ping_end = pingEndCb;

    esp_ping_handle_t hdl = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        vSemaphoreDelete(ctx.done);
        return false;
    }
    if (esp_ping_start(hdl) != ESP_OK) {
        esp_ping_delete_session(hdl);
        vSemaphoreDelete(ctx.done);
        return false;
    }

    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(3000));
    bool inUse = ctx.success != 0;

    esp_ping_delete_session(hdl);
    vSemaphoreDelete(ctx.done);

    if (logTerminal_) {
        ESP_LOGI(TAG, "ICMP probe " IP_FMT ": %s", IP_FMT_ARGS(ip),
                 inUse ? "in use" : "free");
    }
    return inUse;
}

// ─────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────

DhcpServer::DhcpServer()
{
}

DhcpServer::~DhcpServer()
{
    stop();
}

// ─────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────

bool DhcpServer::start()
{
    if (state_ == DhcpServerState::RUNNING) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    // If recovering from ERROR, clean up first
    stopRequested_ = false;
    if (taskHandle_) {
        taskHandle_ = nullptr;
    }
    if (socketFd_ >= 0) {
        close(socketFd_);
        socketFd_ = -1;
    }

    // Cache config
    auto dhcpCfg = core::Config::instance().getDhcp();
    rangeStart_ = ipStrToU32(dhcpCfg.startIp);
    rangeEnd_ = ipStrToU32(dhcpCfg.endIp);
    leaseTimeSec_ = dhcpCfg.leaseTimeSec;
    logTerminal_ = dhcpCfg.logTerminal;
    dnsMode_ = dhcpCfg.dnsMode;
    dnsManualIp_ = ipStrToU32(dhcpCfg.dnsAddress);
    if (logTerminal_) ESP_LOGI(TAG, "Terminal logging enabled");
    // External REST logging of DHCP events
    restLogger_.setAuth(dhcpCfg.logAuthEnabled, dhcpCfg.logAuthUser,
                        dhcpCfg.logAuthPassword);
    restLogger_.setEnabled(dhcpCfg.logRest);
    restLogger_.setUrl(dhcpCfg.logUrl);
    ESP_LOGI(TAG, "REST logging: enabled=%d url=%s auth=%d",
             dhcpCfg.logRest ? 1 : 0,
             dhcpCfg.logUrl.empty() ? "-" : dhcpCfg.logUrl.c_str(),
             dhcpCfg.logAuthEnabled ? 1 : 0);
    ESP_LOGI(TAG, "DNS mode: %s (manual=%s, built-in DNS running=%d)",
             dnsMode_.c_str(), dhcpCfg.dnsAddress.c_str(), dnsServerRunning_ ? 1 : 0);

    // Cache server IP info from netif
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (netif) {
        esp_netif_ip_info_t ipInfo;
        esp_netif_get_ip_info(netif, &ipInfo);
        serverIp_ = ipInfo.ip.addr;
        serverNetmask_ = ipInfo.netmask.addr;
        serverGateway_ = ipInfo.gw.addr;
        ESP_LOGD(TAG, "netif IP: " IP_FMT " / " IP_FMT " / " IP_FMT,
                 IP_FMT_ARGS(serverIp_), IP_FMT_ARGS(serverNetmask_), IP_FMT_ARGS(serverGateway_));
    } else {
        ESP_LOGW(TAG, "netif handle not available");
    }

    if (serverIp_ == 0) {
        ESP_LOGW(TAG, "netif returned no IP, falling back to config string");
        serverIp_ = ipStrToU32(dhcpCfg.serverIp);
        serverNetmask_ = ipStrToU32(dhcpCfg.subnet);
        serverGateway_ = ipStrToU32(dhcpCfg.gateway);
    }

    if (serverIp_ == 0) {
        ESP_LOGE(TAG, "No valid server IP (WiFi not connected?)");
        state_ = DhcpServerState::ERROR;
        return false;
    }

    // Load static bindings
    reloadStaticBindings();

    state_ = DhcpServerState::RUNNING;
    stopRequested_ = false;

    // Create server task
    BaseType_t res = xTaskCreatePinnedToCore(
        serverTask, "dhcp_server", 8192, this,
        configMAX_PRIORITIES - 2, &taskHandle_, 0);

    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create DHCP server task");
        state_ = DhcpServerState::ERROR;
        return false;
    }

    ESP_LOGI(TAG, "DHCP server started on " IP_FMT ", range " IP_FMT " - " IP_FMT,
             IP_FMT_ARGS(serverIp_),
             IP_FMT_ARGS(rangeStart_),
             IP_FMT_ARGS(rangeEnd_));
    return true;
}

void DhcpServer::stop()
{
    if (state_ == DhcpServerState::STOPPED) return;

    stopRequested_ = true;

    if (taskHandle_) {
        // Close socket to unblock recvfrom
        if (socketFd_ >= 0) {
            close(socketFd_);
            socketFd_ = -1;
        }
        // Wait for task to finish
        vTaskDelay(pdMS_TO_TICKS(100));
        taskHandle_ = nullptr;
    }

    state_ = DhcpServerState::STOPPED;
    leases_.clear();
    ESP_LOGI(TAG, "DHCP server stopped");
}

void DhcpServer::reloadStaticBindings()
{
    staticBindings_.clear();
    auto bindings = core::Config::instance().getStaticBindings();
    for (const auto& b : bindings) {
        StaticEntry entry;
        // Parse MAC
        std::sscanf(b.mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                    &entry.mac[0], &entry.mac[1], &entry.mac[2],
                    &entry.mac[3], &entry.mac[4], &entry.mac[5]);
        entry.ip = ipStrToU32(b.ip);
        entry.gatewayIp = ipStrToU32(b.gateway);
        entry.useGateway = b.useGateway;
        entry.enabled = b.enabled;
        entry.useDns = b.useDns;
        staticBindings_.push_back(entry);

        ESP_LOGI(TAG, "Binding: mac=%s ip=%s gw=%s useGateway=%d enabled=%d useDns=%d",
                 b.mac.c_str(), b.ip.c_str(), b.gateway.c_str(),
                 b.useGateway ? 1 : 0, b.enabled ? 1 : 0, b.useDns ? 1 : 0);
    }
    ESP_LOGI(TAG, "Static bindings reloaded (%zu entries)", staticBindings_.size());
}

// ─────────────────────────────────────────────────────
// Task
// ─────────────────────────────────────────────────────

void DhcpServer::serverTask(void* arg)
{
    DhcpServer* self = static_cast<DhcpServer*>(arg);
    self->serverLoop();
    self->state_ = DhcpServerState::ERROR;
    self->taskHandle_ = nullptr;
    ESP_LOGE(TAG, "DHCP server task terminated unexpectedly");
    vTaskDelete(nullptr);
}

void DhcpServer::serverLoop()
{
    socketFd_ = createSocket();
    if (socketFd_ < 0) {
        ESP_LOGE(TAG, "Failed to create DHCP socket");
        state_ = DhcpServerState::ERROR;
        return;
    }

    // Set receive timeout so we can check stopRequested_ periodically
    struct timeval rcvTimeout = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(socketFd_, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeout, sizeof(rcvTimeout));

    ESP_LOGI(TAG, "DHCP server listening on port 67");

    uint8_t buf[1024];
    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    uint32_t heartbeatCounter = 0;

    while (!stopRequested_) {
        ssize_t recvLen = recvfrom(socketFd_, buf, sizeof(buf), 0,
                                   (struct sockaddr*)&from, &fromLen);
        if (recvLen < 0) {
            // Timeout or error — check if we should stop
            removeExpiredLeases();
            // Heartbeat log every ~30 iterations (60 seconds)
            heartbeatCounter++;
            if (heartbeatCounter % 30 == 0 && logTerminal_) {
                ESP_LOGD(TAG, "Server alive, leases: %u", static_cast<unsigned>(leases_.size()));
            }
            continue;
        }

        removeExpiredLeases();

        if (logTerminal_) {
            ESP_LOGI(TAG, "DHCP packet received from " IP_FMT ":%u, len=%d",
                     IP_FMT_ARGS(from.sin_addr.s_addr),
                     ntohs(from.sin_port), static_cast<int>(recvLen));
        }

        handleDhcpMessage(buf, static_cast<size_t>(recvLen),
                          from.sin_addr.s_addr, ntohs(from.sin_port));
    }

    if (socketFd_ >= 0) {
        close(socketFd_);
        socketFd_ = -1;
    }
}

// ─────────────────────────────────────────────────────
// Socket
// ─────────────────────────────────────────────────────

int DhcpServer::createSocket()
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return -1;
    }

    int broadcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DHCP_SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(fd);
        return -1;
    }

    return fd;
}

int DhcpServer::sendUdp(uint32_t destIp, uint16_t destPort,
                         const uint8_t* data, size_t len)
{
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(destPort);
    dest.sin_addr.s_addr = destIp;

    return sendto(socketFd_, data, len, 0,
                  (struct sockaddr*)&dest, sizeof(dest));
}

// ─────────────────────────────────────────────────────
// DHCP message handling
// ─────────────────────────────────────────────────────

bool DhcpServer::handleDhcpMessage(const uint8_t* buf, size_t len,
                                    uint32_t srcAddr, uint16_t srcPort)
{
    if (len < DHCP_MIN_MSGSIZE) return false;

    const DhcpMessage* msg = reinterpret_cast<const DhcpMessage*>(buf);

    // Verify magic cookie
    if (ntohl(msg->cookie) != DHCP_MAGIC_COOKIE) return false;

    // Extract DHCP message type from options
    uint8_t msgType = 0;
    uint32_t requestedIp = 0;
    uint32_t serverId = 0;

    const uint8_t* opt = msg->options;
    while (*opt != DHCP_OPT_END && (opt - msg->options) < (int)sizeof(msg->options)) {
        if (*opt == DHCP_OPT_PAD) {
            opt++;
            continue;
        }
        uint8_t optLen = *(opt + 1);
        if (*opt == DHCP_OPT_MSG_TYPE && optLen == 1) {
            msgType = *(opt + 2);
        } else if (*opt == DHCP_OPT_REQ_ADDR && optLen == 4) {
            memcpy(&requestedIp, opt + 2, 4);
        } else if (*opt == DHCP_OPT_SERVER_ID && optLen == 4) {
            memcpy(&serverId, opt + 2, 4);
        }
        opt += optLen + 2;
    }

    if (msgType == 0) return false;

    // Log incoming DHCP messages if terminal logging enabled
    if (logTerminal_) {
        const char* typeStr = "UNKNOWN";
        switch (msgType) {
            case DHCP_DISCOVER: typeStr = "DISCOVER"; break;
            case DHCP_REQUEST:  typeStr = "REQUEST";  break;
            case DHCP_RELEASE:  typeStr = "RELEASE";  break;
            case DHCP_DECLINE:  typeStr = "DECLINE";  break;
        }
        ESP_LOGI(TAG, "DHCP %s from " IP_FMT " MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                 typeStr, IP_FMT_ARGS(srcAddr),
                 msg->chaddr[0], msg->chaddr[1], msg->chaddr[2],
                 msg->chaddr[3], msg->chaddr[4], msg->chaddr[5]);
    }

    switch (msgType) {
    case DHCP_DISCOVER:
        if (logTerminal_) ESP_LOGI(TAG, "DHCP DISCOVER — broadcasting OFFER");
        sendDhcpOffer(msg->chaddr, msg->xid, requestedIp, msg->giaddr);
        break;

    case DHCP_REQUEST:
        // If server_id is set and matches us, or if ciaddr is set (rebinding)
        if (serverId == 0 || serverId == serverIp_) {
            uint32_t assignIp = requestedIp ? requestedIp : msg->ciaddr;
            // Accept static-binding IPs for this client even if they are
            // outside the dynamic range (e.g. .42 with range .100-.200).
            bool valid = assignIp && assignIp != serverIp_ &&
                         (isIpInRange(assignIp) ||
                          isStaticBindingForMac(msg->chaddr, assignIp));
            if (valid) {
                bool ack = false;
                if (isStaticBindingForMac(msg->chaddr, assignIp)) {
                    // Static binding for this client — always honor it
                    ack = true;
                } else {
                    auto it = leases_.find(assignIp);
                    if (it != leases_.end()) {
                        // Existing lease: ACK only if it belongs to this client
                        ack = (memcmp(it->second.mac, msg->chaddr, 6) == 0);
                    } else {
                        // No lease — ARP-probe to detect a conflict
                        uint8_t ownerMac[6];
                        bool inUse = probeIp(assignIp, ownerMac);
                        if (!inUse) {
                            ack = true;
                        } else {
                            // IP present on the network: OK only if the
                            // requesting client is itself the owner
                            ack = (memcmp(ownerMac, msg->chaddr, 6) == 0);
                            if (logTerminal_) {
                                ESP_LOGI(TAG, "DHCP REQUEST " IP_FMT " owned by %02x:%02x:%02x:%02x:%02x:%02x, requester %02x:%02x:%02x:%02x:%02x:%02x",
                                         IP_FMT_ARGS(assignIp),
                                         ownerMac[0], ownerMac[1], ownerMac[2],
                                         ownerMac[3], ownerMac[4], ownerMac[5],
                                         msg->chaddr[0], msg->chaddr[1], msg->chaddr[2],
                                         msg->chaddr[3], msg->chaddr[4], msg->chaddr[5]);
                            }
                        }
                    }
                }
                if (ack) {
                    if (logTerminal_) ESP_LOGI(TAG, "DHCP REQUEST — sending ACK");
                    sendDhcpAck(msg->chaddr, msg->xid, assignIp, msg->giaddr);
                    addLease(msg->chaddr, assignIp);
                } else {
                    if (logTerminal_) ESP_LOGI(TAG, "DHCP REQUEST — sending NAK (conflict)");
                    sendDhcpNak(msg->chaddr, msg->xid, msg->giaddr);
                }
            } else {
                if (logTerminal_) ESP_LOGI(TAG, "DHCP REQUEST — sending NAK");
                sendDhcpNak(msg->chaddr, msg->xid, msg->giaddr);
            }
        }
        break;

    case DHCP_RELEASE:
        // Remove lease
        {
            auto it = leases_.find(msg->ciaddr);
            if (it != leases_.end()) {
                logDhcpRest("RELEASE", it->second.mac, msg->ciaddr,
                            0, 0, false, 0, 0);
                leases_.erase(it);
                ESP_LOGI(TAG, "Lease released for " IP_FMT, IP_FMT_ARGS(msg->ciaddr));
            }
        }
        break;

    case DHCP_DECLINE:
        ESP_LOGW(TAG, "DHCP DECLINE for " IP_FMT, IP_FMT_ARGS(requestedIp));
        logDhcpRest("DECLINE", msg->chaddr, requestedIp, 0, 0, false, 0, 0);
        break;

    default:
        break;
    }

    return true;
}

// ─────────────────────────────────────────────────────
// Send DHCP messages
// ─────────────────────────────────────────────────────

void DhcpServer::sendDhcpOffer(const uint8_t* clientMac, uint32_t transactionId,
                                uint32_t requestedIp, uint32_t relayIp)
{
    uint32_t offerIp = 0;
    if (requestedIp &&
        (isIpInRange(requestedIp) || isStaticBindingForMac(clientMac, requestedIp))) {
        // Honor a requested IP unless it is already used on the network by
        // another device. Static bindings for this client are always honored.
        if (isStaticBindingForMac(clientMac, requestedIp) ||
            !probeIp(requestedIp, nullptr)) {
            offerIp = requestedIp;
        } else {
            ESP_LOGI(TAG, "Requested IP " IP_FMT " in use on network, picking another",
                     IP_FMT_ARGS(requestedIp));
        }
    }
    if (offerIp == 0) {
        offerIp = selectIp(clientMac);
    }

    if (offerIp == 0) {
        ESP_LOGW(TAG, "No available IP for offer");
        return;
    }

    // Reserve the offered IP so concurrent DISCOVERs don't get the same one
    reserveOffer(clientMac, offerIp);

    DhcpMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.op = 2;              // BOOTREPLY
    msg.htype = 1;           // Ethernet
    msg.hlen = 6;
    msg.hops = 0;
    msg.xid = transactionId;
    msg.secs = 0;
    msg.flags = htons(0x8000); // Broadcast flag
    msg.ciaddr = 0;
    msg.yiaddr = offerIp;
    msg.siaddr = serverIp_;
    msg.giaddr = relayIp;
    memcpy(msg.chaddr, clientMac, 6);
    msg.cookie = htonl(DHCP_MAGIC_COOKIE);

    // Build options
    uint8_t* opt = msg.options;

    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_OFFER;

    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    memcpy(opt, &serverIp_, 4); opt += 4;

    *opt++ = DHCP_OPT_SUBNET_MASK;
    *opt++ = 4;
    memcpy(opt, &serverNetmask_, 4); opt += 4;

    // Router (gateway) — per-client override from the static binding
    bool sendRouter = true;
    uint32_t routerIp = resolveRouter(clientMac, sendRouter);
    if (sendRouter) {
        *opt++ = DHCP_OPT_ROUTER;
        *opt++ = 4;
        memcpy(opt, &routerIp, 4); opt += 4;
    }

    *opt++ = DHCP_OPT_DNS_SERVER;
    *opt++ = 4;
    uint32_t dnsIp = resolveDnsServer(clientMac);
    memcpy(opt, &dnsIp, 4); opt += 4;

    *opt++ = DHCP_OPT_LEASE_TIME;
    *opt++ = 4;
    uint32_t leaseN = htonl(leaseTimeSec_);
    memcpy(opt, &leaseN, 4); opt += 4;

    *opt++ = DHCP_OPT_END;

    uint32_t destIp = (relayIp != 0) ? relayIp : htonl(0xFFFFFFFF); // broadcast
    sendUdp(destIp, DHCP_CLIENT_PORT, reinterpret_cast<uint8_t*>(&msg),
            sizeof(DhcpMessage) - sizeof(msg.options) + (opt - msg.options));

    ESP_LOGI(TAG, "DHCP OFFER " IP_FMT " to %02x:%02x:%02x:%02x:%02x:%02x"
             " | mask=" IP_FMT " router=" IP_FMT "(sent=%d)"
             " dns=" IP_FMT " lease=%lus",
             IP_FMT_ARGS(offerIp),
             clientMac[0], clientMac[1], clientMac[2],
             clientMac[3], clientMac[4], clientMac[5],
             IP_FMT_ARGS(serverNetmask_),
             IP_FMT_ARGS(routerIp), sendRouter ? 1 : 0,
             IP_FMT_ARGS(dnsIp), (unsigned long)leaseTimeSec_);
    logDhcpRest("OFFER", clientMac, offerIp, serverNetmask_,
                routerIp, sendRouter, dnsIp,
                static_cast<int32_t>(leaseTimeSec_));
}

void DhcpServer::sendDhcpAck(const uint8_t* clientMac, uint32_t transactionId,
                              uint32_t assignedIp, uint32_t relayIp)
{
    DhcpMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.op = 2;
    msg.htype = 1;
    msg.hlen = 6;
    msg.hops = 0;
    msg.xid = transactionId;
    msg.secs = 0;
    msg.flags = htons(0x8000);
    msg.ciaddr = 0;
    msg.yiaddr = assignedIp;
    msg.siaddr = serverIp_;
    msg.giaddr = relayIp;
    memcpy(msg.chaddr, clientMac, 6);
    msg.cookie = htonl(DHCP_MAGIC_COOKIE);

    uint8_t* opt = msg.options;

    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_ACK;

    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    memcpy(opt, &serverIp_, 4); opt += 4;

    *opt++ = DHCP_OPT_SUBNET_MASK;
    *opt++ = 4;
    memcpy(opt, &serverNetmask_, 4); opt += 4;

    // Router (gateway) — per-client override from the static binding
    bool sendRouter = true;
    uint32_t routerIp = resolveRouter(clientMac, sendRouter);
    if (sendRouter) {
        *opt++ = DHCP_OPT_ROUTER;
        *opt++ = 4;
        memcpy(opt, &routerIp, 4); opt += 4;
    }

    *opt++ = DHCP_OPT_DNS_SERVER;
    *opt++ = 4;
    uint32_t dnsIp = resolveDnsServer(clientMac);
    memcpy(opt, &dnsIp, 4); opt += 4;

    *opt++ = DHCP_OPT_LEASE_TIME;
    *opt++ = 4;
    uint32_t leaseN = htonl(leaseTimeSec_);
    memcpy(opt, &leaseN, 4); opt += 4;

    *opt++ = DHCP_OPT_END;

    uint32_t destIp = (relayIp != 0) ? relayIp : htonl(0xFFFFFFFF);
    sendUdp(destIp, DHCP_CLIENT_PORT, reinterpret_cast<uint8_t*>(&msg),
            sizeof(DhcpMessage) - sizeof(msg.options) + (opt - msg.options));

    ESP_LOGI(TAG, "DHCP ACK " IP_FMT " to %02x:%02x:%02x:%02x:%02x:%02x"
             " | mask=" IP_FMT " router=" IP_FMT "(sent=%d)"
             " dns=" IP_FMT " lease=%lus",
             IP_FMT_ARGS(assignedIp),
             clientMac[0], clientMac[1], clientMac[2],
             clientMac[3], clientMac[4], clientMac[5],
             IP_FMT_ARGS(serverNetmask_),
             IP_FMT_ARGS(routerIp), sendRouter ? 1 : 0,
             IP_FMT_ARGS(dnsIp), (unsigned long)leaseTimeSec_);
    logDhcpRest("ACK", clientMac, assignedIp, serverNetmask_,
                routerIp, sendRouter, dnsIp,
                static_cast<int32_t>(leaseTimeSec_));
}

void DhcpServer::sendDhcpNak(const uint8_t* clientMac, uint32_t transactionId,
                              uint32_t relayIp)
{
    DhcpMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.op = 2;
    msg.htype = 1;
    msg.hlen = 6;
    msg.xid = transactionId;
    msg.flags = htons(0x8000);
    msg.giaddr = relayIp;
    memcpy(msg.chaddr, clientMac, 6);
    msg.cookie = htonl(DHCP_MAGIC_COOKIE);

    uint8_t* opt = msg.options;
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_NAK;

    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    memcpy(opt, &serverIp_, 4); opt += 4;

    *opt++ = DHCP_OPT_END;

    uint32_t destIp = (relayIp != 0) ? relayIp : htonl(0xFFFFFFFF);
    sendUdp(destIp, DHCP_CLIENT_PORT, reinterpret_cast<uint8_t*>(&msg),
            sizeof(DhcpMessage) - sizeof(msg.options) + (opt - msg.options));

    ESP_LOGW(TAG, "DHCP NAK to %02x:%02x:%02x:%02x:%02x:%02x",
             clientMac[0], clientMac[1], clientMac[2],
             clientMac[3], clientMac[4], clientMac[5]);
    logDhcpRest("NAK", clientMac, 0, 0, 0, false, 0, 0);
}

// ─────────────────────────────────────────────────────
// IP management
// ─────────────────────────────────────────────────────

uint32_t DhcpServer::selectIp(const uint8_t* clientMac)
{
    // 1. Check static bindings (only enabled ones assign a fixed IP)
    for (const auto& entry : staticBindings_) {
        if (!entry.enabled) continue;
        if (memcmp(entry.mac, clientMac, 6) == 0) {
            if (leases_.find(entry.ip) == leases_.end()) {
                return entry.ip;
            }
        }
    }

    // 2. Check if client already has a lease
    for (const auto& [ip, lease] : leases_) {
        if (memcmp(lease.mac, clientMac, 6) == 0) {
            return ip;
        }
    }

    // 3. Find first available IP in range; ARP-probe to skip addresses
    //    already used on the network (e.g. after a server reboot).
    for (uint32_t ip = rangeStart_; ip <= rangeEnd_; ip = htonl(ntohl(ip) + 1)) {
        if (ip == serverIp_) continue;
        if (leases_.find(ip) == leases_.end()) {
            if (probeIp(ip, nullptr)) {
                if (logTerminal_) {
                    ESP_LOGI(TAG, "IP " IP_FMT " in use on network, skipping",
                             IP_FMT_ARGS(ip));
                }
                continue;
            }
            return ip;
        }
    }

    return 0; // no available IP
}

bool DhcpServer::isIpInRange(uint32_t ip) const
{
    if (ip == serverIp_) return false;
    uint32_t start = ntohl(rangeStart_);
    uint32_t end = ntohl(rangeEnd_);
    uint32_t val = ntohl(ip);
    return val >= start && val <= end;
}

bool DhcpServer::isStaticBindingForMac(const uint8_t* mac, uint32_t ip) const
{
    for (const auto& entry : staticBindings_) {
        if (!entry.enabled) continue;
        if (entry.ip == ip && memcmp(entry.mac, mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t DhcpServer::resolveDnsServer(const uint8_t* clientMac) const
{
    // Per-client override from the static binding: if the "use DNS" checkbox is
    // off for this host, point it at the EXTERNAL DNS (configured DNS address,
    // else the router) instead of the built-in DNS server.
    if (clientMac) {
        for (const auto& entry : staticBindings_) {
            if (!entry.enabled) continue;
            if (memcmp(entry.mac, clientMac, 6) == 0) {
                if (!entry.useDns) {
                    return (dnsManualIp_ != 0) ? dnsManualIp_ : serverGateway_;
                }
                break;
            }
        }
    }
    if (dnsMode_ == "manual") {
        // Manual: use configured DNS address (fall back to router if invalid)
        if (dnsManualIp_ != 0 && dnsManualIp_ != serverIp_) {
            return dnsManualIp_;
        }
        return serverGateway_;
    }
    // Auto: built-in DNS server if running, otherwise the router
    if (dnsServerRunning_) return serverIp_;
    return serverGateway_;
}

uint32_t DhcpServer::resolveRouter(const uint8_t* clientMac, bool& sendRouter) const
{
    sendRouter = true;
    for (const auto& entry : staticBindings_) {
        if (!entry.enabled) continue;
        if (memcmp(entry.mac, clientMac, 6) == 0) {
            if (!entry.useGateway) {
                // Per-binding: no gateway for this host
                sendRouter = false;
                return 0;
            }
            // The per-binding gateway text field was removed from the UI — the
            // checkbox now only controls whether the gateway is sent at all;
            // when enabled we always advertise the server's router.
            return serverGateway_;
        }
    }
    return serverGateway_;
}

uint32_t DhcpServer::ipStrToU32(const std::string& ip) const
{
    uint32_t addr;
    inet_pton(AF_INET, ip.c_str(), &addr);
    return addr;
}

// ─────────────────────────────────────────────────────
// Lease management
// ─────────────────────────────────────────────────────

void DhcpServer::addLease(const uint8_t* mac, uint32_t ip)
{
    DhcpLease lease;
    memcpy(lease.mac, mac, 6);
    lease.ip = ip;
    lease.expiry = getCurrentTimeSec() + leaseTimeSec_;
    leases_[ip] = lease;

    if (logTerminal_) {
        ESP_LOGI(TAG, "Lease added: " IP_FMT " -> %02x:%02x:%02x:%02x:%02x:%02x (expires in %lu s)",
                 IP_FMT_ARGS(ip),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned long)leaseTimeSec_);
    }
}

void DhcpServer::reserveOffer(const uint8_t* mac, uint32_t ip)
{
    // Reserve the offered IP for a short hold so concurrent DISCOVERs from
    // different clients don't get offered the same address. The reservation
    // becomes a full lease on ACK and expires (via removeExpiredLeases) if the
    // client never completes the handshake.
    // Do NOT shorten an existing confirmed lease (e.g. a client re-DISCOVERing
    // before its lease expires) — keep the longer expiry.
    auto existing = leases_.find(ip);
    if (existing != leases_.end() &&
        existing->second.expiry > getCurrentTimeSec() + kOfferHoldSec) {
        return;
    }

    DhcpLease lease;
    memcpy(lease.mac, mac, 6);
    lease.ip = ip;
    lease.expiry = getCurrentTimeSec() + kOfferHoldSec;
    leases_[ip] = lease;

    if (logTerminal_) {
        ESP_LOGI(TAG, "IP " IP_FMT " reserved (offer) for %02x:%02x:%02x:%02x:%02x:%02x",
                 IP_FMT_ARGS(ip),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void DhcpServer::removeExpiredLeases()
{
    uint32_t now = getCurrentTimeSec();
    for (auto it = leases_.begin(); it != leases_.end(); ) {
        if (it->second.expiry <= now) {
            ESP_LOGD(TAG, "Lease expired for " IP_FMT, IP_FMT_ARGS(it->first));
            it = leases_.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t DhcpServer::getCurrentTimeSec() const
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

uint32_t DhcpServer::leaseCount() const
{
    return static_cast<uint32_t>(leases_.size());
}

std::string DhcpServer::stateString() const
{
    switch (state_) {
        case DhcpServerState::RUNNING: return "running";
        case DhcpServerState::STOPPED: return "stopped";
        case DhcpServerState::ERROR:   return "error";
        default:                       return "unknown";
    }
}

std::vector<DhcpLease> DhcpServer::getLeases() const
{
    std::vector<DhcpLease> result;
    for (const auto& [ip, lease] : leases_) {
        result.push_back(lease);
    }
    return result;
}

bool DhcpServer::getMacByIp(uint32_t ip, uint8_t mac[6]) const
{
    auto it = leases_.find(ip);
    if (it == leases_.end()) return false;
    memcpy(mac, it->second.mac, 6);
    return true;
}

// ─────────────────────────────────────────────────────
// REST event logging
// ─────────────────────────────────────────────────────

void DhcpServer::setRestLogging(bool enabled, const std::string& url,
                                bool authEnabled, const std::string& user,
                                const std::string& password)
{
    restLogger_.setAuth(authEnabled, user, password);
    restLogger_.setEnabled(enabled);
    restLogger_.setUrl(url);
}

std::string DhcpServer::macToStr(const uint8_t* mac)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

std::string DhcpServer::ipToStr(uint32_t ipNet)
{
    // IPs are stored as uint32_t in network byte order — the 4 bytes in
    // memory are already in the correct dotted order. Reading them by index
    // (like lwIP's ip4_addr1_16) prints them correctly; big-endian shifts
    // would reverse the octets (e.g. 101.1.168.192 instead of 192.168.1.101).
    char buf[16];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ipNet);
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (unsigned)b[0], (unsigned)b[1],
                  (unsigned)b[2], (unsigned)b[3]);
    return std::string(buf);
}

void DhcpServer::logDhcpRest(const char* event, const uint8_t* mac, uint32_t ipNet,
                             uint32_t maskNet, uint32_t gatewayNet, bool gatewaySent,
                             uint32_t dnsNet, int32_t leaseTime)
{
    restLogger_.logEvent(event,
                         macToStr(mac),
                         ipNet ? ipToStr(ipNet) : "",
                         maskNet ? ipToStr(maskNet) : "",
                         gatewaySent ? ipToStr(gatewayNet) : "",
                         dnsNet ? ipToStr(dnsNet) : "",
                         leaseTime);
}

} // namespace dhcp
} // namespace dhcp
