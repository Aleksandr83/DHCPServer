#ifndef DHCP_CORE_CONFIG_H
#define DHCP_CORE_CONFIG_H

#include <string>
#include <vector>
#include <cstdint>

namespace dhcp {
namespace core {

/**
 * @brief WiFi configuration.
 */
struct WifiConfig {
    std::string ssid;
    std::string password;
};

/**
 * @brief DHCP server configuration.
 */
struct DhcpConfig {
    bool enabled = false;
    std::string serverIp = "192.168.1.201";
    std::string startIp = "192.168.1.100";
    std::string endIp = "192.168.1.200";
    std::string subnet = "255.255.255.0";
    std::string gateway = "192.168.1.1";
    uint32_t leaseTimeSec = 86400; // 24h
    bool logTerminal = false;
    // DNS handed to DHCP clients: "auto" = built-in DNS server if running,
    // otherwise the router; "manual" = use dnsAddress
    std::string dnsMode = "auto";
    std::string dnsAddress; // used when dnsMode == "manual"
    // External REST logging of DHCP events (OFFER/ACK/NAK/RELEASE/DECLINE)
    bool logRest = false;
    std::string logUrl;
    bool logAuthEnabled = false;
    std::string logAuthUser;
    std::string logAuthPassword;
};

/**
 * @brief Static MAC→IP binding entry.
 */
struct StaticBinding {
    std::string mac;        // e.g. "24:0A:C4:01:23:45"
    std::string ip;         // e.g. "192.168.1.50"
    std::string name;       // optional friendly name
    std::string gateway;    // optional per-host gateway; empty = server default
    bool useGateway = true; // if false, DHCP sends NO gateway (option 3) to this host
    bool enabled = true;    // if false, the binding is ignored (client uses a dynamic IP)
    bool useDns = true;     // if false, the host is pointed at the external DNS (not the built-in one)
};

/**
 * @brief DNS server configuration.
 */
struct DnsConfig {
    bool enabled = true;            // master switch for the built-in DNS server
    std::string externalDns = "192.168.1.1";
    bool logTerminal = false;
    // Terminal-logging category filters (only relevant when logTerminal is on)
    bool logForwarded = true;       // queries resolved via external DNS
    bool logLocal = true;           // queries resolved from custom local hosts
    bool logCache = true;           // queries resolved from cache
    bool logRestSent = false;       // terminal filter: only queries sent to REST
    bool logRest = false;
    std::string logUrl;
    // HTTP Basic auth for the REST logging URL (empty user/pass = no auth)
    bool logAuthEnabled = false;
    std::string logAuthUser;
    std::string logAuthPassword;
    bool cacheRest = false;         // master switch for the external DNS cache
    bool cacheRestRead = true;      // read (lookup) from the cache
    bool cacheRestWrite = true;     // write (store) to the cache on forward
    std::string cacheUrl;
    // HTTP Basic auth for the external cache URL
    bool cacheAuthEnabled = false;
    std::string cacheAuthUser;
    std::string cacheAuthPassword;
    // Built-in DNS cache (hash table in PSRAM — ESP32-P4 has 32 MB).
    // Size is capped at 20 MB so the cache can be persisted to cache.dat on
    // the ~21 MB FAT partition.
    bool    cacheInternal = false;   // master switch
    uint32_t cacheInternalSizeMb = 20;  // max table size in MB (1..20)
    bool cacheInternalIgnoreTtl = false;// store TTL but never expire by it
};

/**
 * @brief Custom DNS host mapping (domain name → IP addresses).
 * Resolved locally by the built-in DNS server. Either or both of
 * ip4 / ip6 may be set.
 */
struct LocalHostEntry {
    std::string name;  // e.g. "mydevice.local"
    std::string ip4;   // IPv4 address (A record), optional
    std::string ip6;   // IPv6 address (AAAA record), optional
    bool enabled = true; // if false, the mapping is not served
};

/**
 * @brief Security / auth configuration.
 */
struct SecurityConfig {
    std::string username = "admin";
    std::string password = "admin";
    uint32_t maxAttempts = 5;
    uint32_t lockoutPeriodSec = 300; // 5 min
};

/**
 * @brief Configuration manager using NVS.
 *
 * All settings are persisted in NVS under the "dhcp" namespace.
 * Static bindings and DNS cache are stored as blobs (max 512 bytes each).
 */
class Config {
public:
    /**
     * @brief Get the singleton instance.
     * NVS must be initialized before first call.
     */
    static Config& instance();

    // ─── WiFi ────────────────────────────────────────
    WifiConfig getWifi() const;
    void setWifi(const WifiConfig& cfg);

    // ─── DHCP ────────────────────────────────────────
    DhcpConfig getDhcp() const;
    void setDhcp(const DhcpConfig& cfg);

    // ─── Static bindings ─────────────────────────────
    std::vector<StaticBinding> getStaticBindings() const;
    bool setStaticBindings(const std::vector<StaticBinding>& bindings);
    // Serialized size (bytes) of the current bindings in NVS storage.
    size_t staticBindingsBytes() const;
    static constexpr size_t kMaxBindingsBytes = 512;

    // ─── DNS ─────────────────────────────────────────
    DnsConfig getDns() const;
    void setDns(const DnsConfig& cfg);

    // ─── Local DNS hosts (domain → IP) ───────────────
    std::vector<LocalHostEntry> getLocalHosts() const;
    bool setLocalHosts(const std::vector<LocalHostEntry>& hosts);
    // Serialized size (bytes) of the current local hosts in NVS storage.
    size_t localHostsBytes() const;
    static constexpr size_t kMaxLocalHostsBytes = 512;

    // ─── DNS cache blob ──────────────────────────────
    std::vector<uint8_t> getDnsCache() const;
    bool setDnsCache(const std::vector<uint8_t>& data);
    static constexpr size_t kMaxDnsCacheBytes = 512;

    // ─── Security ────────────────────────────────────
    SecurityConfig getSecurity() const;
    void setSecurity(const SecurityConfig& cfg);

    // ─── Factory reset ───────────────────────────────
    // Erases the whole "dhcp" NVS namespace. All getters then fall back to
    // their compile-time defaults (factory state) until set* is called again.
    static bool resetAll();

private:
    Config() = default;
    ~Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // NVS helpers
    static std::string readStr(const char* key, const std::string& def);
    static void writeStr(const char* key, const std::string& val);
    static int32_t readI32(const char* key, int32_t def);
    static void writeI32(const char* key, int32_t val);
    static bool readBlob(const char* key, std::vector<uint8_t>& out);
    static bool writeBlob(const char* key, const std::vector<uint8_t>& data);
    static bool eraseKey(const char* key);
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_CONFIG_H
