#ifndef DHCP_DHCP_DHCPSERVER_H
#define DHCP_DHCP_DHCPSERVER_H

#include "IDhcpServer.h"
#include "DhcpRestLogger.h"
#include "DhcpAllowedList.h"
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

    /**
     * @brief Reload the allowed-computers allow-list (NVS text blob + the
     * "only allowed" switch) and rebuild the PSRAM MAC hash table.
     */
    void reloadAllowedComputers() override;

    /** @brief True when the allow-list MAC table is built and usable. */
    bool allowedListAvailable() const override { return allowedList_.available(); }

    /** @brief Number of MAC addresses in the active allow-list table. */
    size_t allowedListCount() const override { return allowedList_.count(); }

    /**
     * @brief Ask what this MAC is called (lease table, then a PTR probe).
     * See IDhcpServer::lookupClientName.
     */
    IDhcpServer::ClientNameResult lookupClientName(const uint8_t mac[6]) const override;

    /**
     * @brief (Re)apply the lease/offer table cap from the DHCP config.
     *
     * 0 means "auto" = 2× the configured pool size, clamped to 8..512; an
     * explicit value is clamped to the same range. Called from start() and
     * whenever the DHCP settings change.
     */
    void applyLeaseLimit() override;

    /** @brief Effective cap currently in force (never 0 after applyLeaseLimit). */
    uint32_t maxLeaseEntriesEffective() const override { return maxLeaseEntriesEffective_; }
    /** @brief Requests refused because the table was full (diagnostics). */
    uint32_t leaseLimitRejects() const override { return leaseLimitRejects_; }

private:
    // Internal task function
    static void serverTask(void* arg);
    void serverLoop();

    // DHCP message handling
    bool handleDhcpMessage(const uint8_t* buf, size_t len,
                           uint32_t srcAddr, uint16_t srcPort);
    void sendDhcpOffer(const uint8_t* clientMac, uint32_t transactionId,
                       uint32_t requestedIp, uint32_t relayIp,
                       const std::string& clientName);
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
    void addLease(const uint8_t* mac, uint32_t ip, const std::string& hostname);
    void removeExpiredLeases();
    bool reserveOffer(const uint8_t* mac, uint32_t ip, const std::string& hostname);
    uint32_t getCurrentTimeSec() const;

    /**
     * @brief True when a NEW entry (ip not yet in the table) may be inserted.
     *
     * Expired leases are purged first, so a table that only looks full because
     * of stale offers frees itself instead of refusing a legitimate client.
     */
    bool canAddLeaseEntry(uint32_t ip);

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

    // Lease-table cap (applyLeaseLimit): configured value (0 = auto) plus the
    // effective one actually enforced, and a refusal counter for diagnostics.
    uint32_t maxLeaseEntries_ = 0;
    uint32_t maxLeaseEntriesEffective_ = 64;
    uint32_t leaseLimitRejects_ = 0;

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

    // Allowed computers (DHCP allow-list). The MAC hash table lives in a PSRAM
    // block this object owns; the list itself stays in NVS as text, and
    // allowedStaticRefs_ mirrors the binding MACs + their Enable checkbox so the
    // policy check does not touch strings on the packet path.
    DhcpAllowedList allowedList_;
    std::vector<DhcpAllowedList::StaticRef> allowedStaticRefs_;
    void* allowedStorage_ = nullptr;
    bool allowOnly_ = false;

    /** @brief Mirror staticBindings_ into allowedStaticRefs_ (MAC + enabled). */
    void refreshAllowedStaticRefs();

    /**
     * @brief Name the client reported about itself (lease/offer entry).
     * @return "" when this MAC has no fresh entry with a name.
     */
    std::string clientHostnameByMac(const uint8_t mac[6]) const;

    /**
     * @brief Address of this MAC: lease/offer table first, then the ARP cache.
     *
     * The ARP cache is the second chance for a computer that took its address
     * somewhere else (a fixed address, or a lease from before this device was
     * installed) — without an address no probe is possible at all.
     */
    bool clientIpByMac(const uint8_t mac[6], uint32_t& ipNet) const;

    /**
     * @brief Whom to ask for a reverse lookup.
     *
     * The router (the gateway) registers the names of the addresses it hands
     * out; in manual DNS mode the address the operator configured is used
     * instead. 0 means "nowhere to ask", and the lookup then stops after step 1.
     */
    uint32_t nameServerIp() const;

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
