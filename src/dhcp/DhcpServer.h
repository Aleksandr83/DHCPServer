#ifndef DHCP_DHCP_DHCPSERVER_H
#define DHCP_DHCP_DHCPSERVER_H

#include "IDhcpServer.h"
#include "DhcpRestLogger.h"
#include <string>
#include <vector>
#include <cstdint>
#include <map>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dhcp {
namespace dhcp {

/**
 * @brief A single DHCP lease entry.
 */
struct DhcpLease {
    uint8_t mac[6];
    uint32_t ip;        // network byte order
    uint32_t expiry;    // seconds since boot
    std::string hostname;
};

/**
 * @brief ESP-IDF DHCP server implementation using raw UDP sockets.
 *
 * Listens on UDP port 67 and handles DISCOVER/REQUEST/RELEASE messages.
 * Uses the configured IP range and static bindings from Config.
 * Runs in a dedicated FreeRTOS task.
 */
class DhcpServer : public IDhcpServer {
public:
    DhcpServer();
    ~DhcpServer() override;

    // IDhcpServer interface
    bool start() override;
    void stop() override;
    DhcpServerState state() const override { return state_; }
    bool isRunning() const override { return state_ == DhcpServerState::RUNNING; }
    uint32_t leaseCount() const override;
    bool getMacByIp(uint32_t ip, uint8_t mac[6]) const override;
    std::string stateString() const override;

    /**
     * @brief Get current leases (for monitoring / REST API).
     */
    std::vector<DhcpLease> getLeases() const;

    /**
     * @brief Enable or disable terminal logging.
     */
    void setLogTerminal(bool enabled) override { logTerminal_ = enabled; }

    /**
     * @brief Configure external REST logging of DHCP events.
     */
    void setRestLogging(bool enabled, const std::string& url,
                        bool authEnabled, const std::string& user,
                        const std::string& password) override;

    /**
     * @brief Set whether the built-in DNS server is running (affects DNS
     * advertised to clients in "auto" mode).
     */
    void setDnsServerRunning(bool running) override { dnsServerRunning_ = running; }

    /**
     * @brief Reload static bindings from NVS (applies gateway/use_gateway
     * changes without a reboot).
     */
    void reloadStaticBindings() override;

private:
    // Internal task function
    static void serverTask(void* arg);
    void serverLoop();

    // DHCP message handling
    bool handleDhcpMessage(const uint8_t* buf, size_t len,
                           uint32_t srcAddr, uint16_t srcPort);
    void sendDhcpOffer(const uint8_t* clientMac, uint32_t transactionId,
                       uint32_t requestedIp, uint32_t relayIp);
    void sendDhcpAck(const uint8_t* clientMac, uint32_t transactionId,
                     uint32_t assignedIp, uint32_t relayIp);
    void sendDhcpNak(const uint8_t* clientMac, uint32_t transactionId,
                     uint32_t relayIp);

    // IP management
    uint32_t selectIp(const uint8_t* clientMac);
    bool isIpInRange(uint32_t ip) const;
    bool isStaticBindingForMac(const uint8_t* mac, uint32_t ip) const;
    bool probeIp(uint32_t ip, uint8_t ownerMac[6]) const;
    bool arpProbeIp(uint32_t ip, uint8_t ownerMac[6]) const;
    bool icmpProbeIp(uint32_t ip) const;
    uint32_t ipStrToU32(const std::string& ip) const;
    uint32_t resolveDnsServer(const uint8_t* clientMac) const;
    uint32_t resolveRouter(const uint8_t* clientMac, bool& sendRouter) const;

    // Lease management
    void addLease(const uint8_t* mac, uint32_t ip);
    void removeExpiredLeases();
    void reserveOffer(const uint8_t* mac, uint32_t ip);
    uint32_t getCurrentTimeSec() const;

    // How long an offered (not yet confirmed) IP stays reserved
    static constexpr uint32_t kOfferHoldSec = 60;

    // Socket helpers
    int createSocket();
    int sendUdp(uint32_t destIp, uint16_t destPort,
                const uint8_t* data, size_t len);

    DhcpServerState state_ = DhcpServerState::STOPPED;
    TaskHandle_t taskHandle_ = nullptr;
    int socketFd_ = -1;
    bool stopRequested_ = false;

    // Server IP info (cached from WiFi)
    uint32_t serverIp_ = 0;
    uint32_t serverNetmask_ = 0;
    uint32_t serverGateway_ = 0;

    // Range config (cached on start)
    uint32_t rangeStart_ = 0;
    uint32_t rangeEnd_ = 0;
    uint32_t leaseTimeSec_ = 86400;
    bool logTerminal_ = false;

    // DNS handed to clients (cached on start)
    std::string dnsMode_ = "auto";   // "auto" | "manual"
    uint32_t dnsManualIp_ = 0;       // manual DNS address (net byte order)
    bool dnsServerRunning_ = false;  // built-in DNS server running state

    // Leases: IP (net order) -> Lease
    mutable std::map<uint32_t, DhcpLease> leases_;

    // Static bindings (cached)
    struct StaticEntry {
        uint8_t mac[6];
        uint32_t ip;
        uint32_t gatewayIp;  // 0 = not set (use server default)
        bool useGateway;     // if false, no gateway (option 3) is sent
        bool enabled = true; // if false, the binding is ignored
        bool useDns = true;  // if false, point the host at the external DNS
    };
    std::vector<StaticEntry> staticBindings_;

    // Async REST event logger (OFFER/ACK/NAK/RELEASE/DECLINE)
    DhcpRestLogger restLogger_;

    // Formatting helpers for REST event logging
    static std::string macToStr(const uint8_t* mac);
    static std::string ipToStr(uint32_t ipNet);
    void logDhcpRest(const char* event, const uint8_t* mac, uint32_t ipNet,
                     uint32_t maskNet, uint32_t gatewayNet, bool gatewaySent,
                     uint32_t dnsNet, int32_t leaseTime);
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DHCPSERVER_H
