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
    // Hard cap on the lease/offer table (DoS hardening: a DHCP starvation
    // flood with random MACs must not grow it without bound). 0 = auto
    // (2× the pool size, clamped 8..512); otherwise clamped to 8..512.
    uint32_t maxLeaseEntries = 0;
    // "Assign addresses only to allowed computers": while this is ON a client
    // that is neither in the allowed-computers list nor covered by an ENABLED
    // static binding is refused outright (no OFFER, no ACK), so it loses its
    // address at the next renewal. While it is OFF the list is ignored
    // completely and addresses are handed out as before.
    // OFF by default: an appliance that filters clients after a firmware update
    // would take a LAN hostage.
    bool allowOnly = false;
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
    // Keep the counters the main page shows across a restart: they are written to
    // Statistica.dat on the internal volume before a reboot and read back at boot.
    bool cacheInternalSaveStats = true; // default ON (the operator asked for it)
    // Keep the cache itself across a restart: cache.dat is written before a reboot
    // (seconds on a full table) and loaded again at boot. Same policy as above —
    // a planned restart preserves it, a power cut does not.
    bool cacheInternalSaveCache = true; // default ON (the operator asked for it)
    // MD5 of cache.dat as this device last wrote it (32 lowercase hex characters,
    // empty = never written). Checked before the file is loaded: a mismatch means
    // a half-written file, a damaged block, or a file replaced behind the
    // firmware's back, and in all three cases loading it would quietly fill the
    // cache with whatever the file happens to hold. Device state, not a setting:
    // it is deliberately absent from the settings export/import.
    std::string cacheInternalFileMd5;
    // Do not forward queries of any type other than A/AAAA to the external
    // cache/upstream DNS — answer NODATA instead (client falls back to A/AAAA).
    bool blockForwardNonAA = false;
    // Answer only clients inside the device's own subnet (address + netmask
    // taken from the DHCP settings, see core::Subnet). Queries from other
    // addresses are silently dropped, so the device is not an open resolver.
    // ON by default — an appliance in a LAN should not be an open resolver.
    bool allowOwnSubnet = true;
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
 * @brief Time (NTP) server configuration.
 *
 * The device synchronises its clock from an external NTP server (SNTP) and
 * serves UTC time to LAN clients over NTP (UDP port 123). The timezone offset
 * is used only for local display/logging on the device itself.
 */
struct TimeConfig {
    bool enabled = false;             // master switch for the NTP server
    bool syncEnabled = true;          // SNTP client: sync the device clock
    std::string externalNtp = "pool.ntp.org"; // upstream NTP server (SNTP)
    std::string timezone = "Europe/Moscow"; // zone id (display only); empty = custom offset
    int32_t utcOffsetHours = 3;       // timezone offset (display only, MSK)
    uint32_t syncIntervalSec = 86400; // SNTP re-sync interval (24 h)
    // Answer only clients inside the device's own subnet (address + netmask
    // from the DHCP settings, see core::Subnet) — the same policy as the DNS
    // filter. ON by default: an appliance in a LAN should not be open.
    bool allowOwnSubnet = true;
    // Maximum NTP replies per second and per client address (1..100).
    uint32_t rateLimitPerSec = 5;
    bool logTerminal = false;         // log NTP requests to the terminal
    bool logRest = false;             // log NTP requests to an external REST
    std::string logUrl;               // REST logging URL
    bool logAuthEnabled = false;      // HTTP Basic auth for REST logging
    std::string logAuthUser;
    std::string logAuthPassword;
};

/**
 * @brief File explorer configuration (FAT volumes on the ESP32-P4).
 *
 * The explorer can serve whole file trees, so it is LAN-only by default, like
 * the built-in DNS and NTP servers: clients outside the device's own subnet
 * (address + netmask from the DHCP settings, see core::Subnet) are refused
 * with `403` instead of being allowed to browse or upload.
 */
struct FileConfig {
    // Answer/accept file operations only from the device's own subnet. ON by
    // default for the same reason as the DNS/NTP filters.
    bool allowOwnSubnet = true;
};

/**
 * @brief Configuration manager using NVS.
 *
 * All settings are persisted in NVS under the "dhcp" namespace.
 * Static bindings, local hosts and the DNS cache are stored as blobs (max 512
 * bytes each); the allow-list budget is larger — see kMaxAllowedBytes.
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

    // ─── Allowed computers (DHCP allow-list) ────────
    // The list is ONE NVS text blob in the codec of dhcp::dhcp::DhcpAllowedList
    // ("mac|name|enabled" per line, at most 25 entries). It is stored as text
    // rather than as a structure so the codec lives in exactly one place — the
    // module that also builds the MAC hash table from it.
    std::string getAllowedComputers() const;              // raw text ("" = unset)
    bool setAllowedComputers(const std::string& text);    // false: over the limit
    // Serialized size (bytes) of the current list in NVS storage.
    size_t allowedComputersBytes() const;
    static constexpr size_t kMaxAllowedBytes = 1024;

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

    // ─── Time (NTP) server ───────────────────────────
    TimeConfig getTime() const;
    void setTime(const TimeConfig& cfg);

    // ─── File explorer (FAT volumes) ─────────────────
    FileConfig getFiles() const;
    void setFiles(const FileConfig& cfg);

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
