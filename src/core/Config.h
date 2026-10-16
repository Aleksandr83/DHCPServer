#ifndef DHCP_CORE_CONFIG_H
#define DHCP_CORE_CONFIG_H

#include "core/NetworkDefaults.h"
#include "core/AutosavePeriod.h"
#include "core/LogFileSizes.h"
#include "core/NtpServers.h"
#include "security/CertStore.h"

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
    std::string serverIp = core::kDefaultServerIp;
    std::string startIp = core::kDefaultPoolStart;
    std::string endIp = core::kDefaultPoolEnd;
    std::string subnet = core::kDefaultSubnetMask;
    std::string gateway = core::kDefaultGateway;
    uint32_t leaseTimeSec = core::kDefaultLeaseSec; // 24h
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
    // The file log of DHCP events (stage 212): `/fat/logs/Dhcp.log`, archived to
    // the card when full. `logFile` starts and stops it and is the switch of the
    // "Log to File" card on the DHCP Logging page - its own switch, not a copy of
    // `logTerminal`: the terminal keeps the diagnostic lines only, the file keeps
    // the whole record, and the operator may well want one without the other.
    // `logFileSize` is one of the sizes the page offers, in bytes (see
    // core/LogFileSizes.h); it lives in its own NVS key (`dhcp_log_size`, 13
    // characters) because the two of them in one key would need a name longer than
    // the 15 NVS allows.
    bool logFile = false;
    uint32_t logFileSize = static_cast<uint32_t>(core::kLogFileDefaultMaxBytes);
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
    std::string externalDns = core::kDefaultExternalDns;
    bool logTerminal = false;
    // Terminal-logging category filters (only relevant when logTerminal is on)
    bool logForwarded = true;       // queries resolved via external DNS
    bool logLocal = true;           // queries resolved from custom local hosts
    bool logCache = true;           // queries resolved from cache
    bool logRestSent = false;       // terminal filter: only queries sent to REST
    bool logRest = false;
    std::string logUrl;
    // The file log of forwarded answers (stage 204): `/fat/logs/Forwarded.log`,
    // archived to the card when full. `logFile` starts and stops it, and
    // `logFileForwarded` says whether the **file** keeps the forwarded answers -
    // its own switch and not the terminal's filter above, because the operator may
    // want the terminal quiet while the file keeps the record (two readers, two
    // switches). `logFileSize` is one of the sizes the settings page offers, in
    // bytes (see core/LogFileSizes.h); it is stored in its own NVS key because
    // `dns_logfile_fwd` is 15 characters - the length NVS allows - and a longer
    // name would be rejected at runtime rather than at compile time.
    bool logFile = false;
    bool logFileForwarded = true;
    uint32_t logFileSize = static_cast<uint32_t>(core::kLogFileDefaultMaxBytes);
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
    /// @brief Autosave the internal cache on a timer (stage 153).
    bool cacheInternalAutosave = false;
    /// @brief The unit of the interval: minutes, hours or days.
    /// Days by default with an interval of one: the operator asked for "Save
    /// every: 1 day" for both autosaves, so neither block starts at an hour.
    AutosavePeriod cacheInternalAutosavePeriod = AutosavePeriod::Day;
    /// @brief Every N of that unit (1..60 minutes, 1..24 hours, 1..month days).
    uint16_t cacheInternalAutosaveInterval = 1;
    /// @brief Autosave the statistics file on a timer (stage 189).
    ///
    /// The statistics are 92 bytes and the cache is megabytes, so the two
    /// autosaves are separate settings with separate timers: the operator asked
    /// for the block that keeps the counters to look after its own file, and off
    /// by default, the way the cache's autosave is.
    bool statsAutosave = false;
    /// @brief The unit of the statistics interval: minutes, hours or days.
    /// Days by default, with an interval of one: the cache's autosave was asked
    /// for in hours (stage 153), the statistics were asked for as "every day".
    AutosavePeriod statsAutosavePeriod = AutosavePeriod::Day;
    /// @brief Every N of that unit, with the same bounds as the cache's.
    uint16_t statsAutosaveInterval = 1;
    /// @brief Automatically refresh entries from upstream DNS.
    bool cacheInternalAutoUpdate = false;
    /// @brief The unit of the interval: hours or days.
    AutosavePeriod cacheInternalAutoUpdatePeriod = AutosavePeriod::Hour;
    /// @brief Every N of that unit (1..24 hours, 1..30 days).
    uint16_t cacheInternalAutoUpdateInterval = 1;
    /// @brief Maximum number of entries to refresh in one cycle.
    uint16_t cacheInternalAutoUpdateBatch = 50;
    /// @brief Seconds to wait after a batch update before starting the next one.
    uint16_t cacheInternalAutoUpdatePause = 60;
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
    /// Rule 39: the pair the login lockout defaults to.
    static constexpr uint32_t kDefaultMaxAttempts = 5;
    static constexpr uint32_t kDefaultLockoutSec = 300;
    uint32_t maxAttempts = kDefaultMaxAttempts;
    uint32_t lockoutPeriodSec = kDefaultLockoutSec; // 5 min
    /**
     * @brief Keep the login attempts in `/fat/logs/Auth.log` (stage 205).
     *
     * Off by default: a device that started writing a new file after an update is
     * a device that changed behaviour without being asked. What is written when it
     * is on: a refused password with the attempt and the budget, a lockout with its
     * period, and a login that came after failures — a successful request with
     * nothing before it is deliberately not recorded, because the interface asks
     * for credentials on every request and a polling page would fill the file.
     */
    bool logAuth = false;
    /**
     * @brief The cap `Auth.log` rotates at (stage 205), in bytes.
     *
     * One of the three sizes the settings pages offer (see core/LogFileSizes.h),
     * stored in its own NVS key: `sec_log_size` is 12 characters, inside the 15
     * NVS allows.
     */
    uint32_t logFileSize = static_cast<uint32_t>(core::kLogFileDefaultMaxBytes);
    /**
     * @brief Serve the web interface over HTTPS as well as HTTP.
     *
     * Off by default, and it cannot be turned on while no certificate is stored:
     * an HTTPS server without a certificate does not start at all, so the switch
     * would only make the interface look broken.
     */
    bool httpsEnabled = false;
    /**
     * @brief Volume that holds the certificate pair.
     *
     * The internal flash volume by default; the card is the other option the
     * operator can pick, and it is read back through @ref
     * security::certStorageFromIndex so a value this build does not know still
     * lands somewhere sensible.
     */
    security::CertStorage certStorage = security::CertStorage::Internal;
    /**
     * @brief Name the next generated certificate is issued for (CN and DNS SAN).
     *
     * Typed by the operator on the certificates page and kept by the device, so
     * the field comes back filled instead of asking for the same name again. It is
     * a setting and not a report of the stored pair: a certificate issued under
     * another name keeps its own CN until it is generated again.
     */
    std::string certName = security::kDefaultCommonName;
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
    std::string externalNtp = kNtpDefaultServers; // upstream NTP servers (SNTP), in order
    std::string timezone = "Europe/Moscow"; // zone id (display only); empty = custom offset
    int32_t utcOffsetHours = 3;       // timezone offset (display only, MSK)
    uint32_t syncIntervalSec = 86400; // SNTP re-sync interval (24 h)
    // Answer only clients inside the device's own subnet (address + netmask
    // from the DHCP settings, see core::Subnet) — the same policy as the DNS
    // filter. ON by default: an appliance in a LAN should not be open.
    bool allowOwnSubnet = true;
    // Maximum NTP replies per second and per client address (1..100).
    uint32_t rateLimitPerSec = 5;
    // Answer only computers the DHCP side would allow as well: the entries of the
    // "Allowed computers" list (DHCP menu) and clients covered by an ENABLED
    // static binding. An NTP request carries an address and no MAC, so the owner
    // of that address is looked up in the lease table first; an address nobody
    // owns is refused - a client that configured its address by hand must not be
    // able to walk around the list (stage 196). OFF by default: an appliance that
    // started filtering after an update would stop serving time to the LAN it had
    // been serving a minute earlier.
    bool onlyAllowedComputers = false;
    bool logTerminal = false;         // log NTP requests to the terminal
    bool logRest = false;             // log NTP requests to an external REST
    // Log NTP requests to `/fat/logs/Ntp.log`, archived to the card when full
    // (stage 191). `time_log_file` is 13 characters, inside the 15 NVS allows.
    bool logFile = false;
    // The cap the file rotates at, in bytes (stage 204): one of the sizes the
    // settings page offers (see core/LogFileSizes.h). `time_log_size` is 13
    // characters, inside the 15 NVS allows.
    uint32_t logFileSize = static_cast<uint32_t>(core::kLogFileDefaultMaxBytes);
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
