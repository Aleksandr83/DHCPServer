#include "Config.h"
#include <cstring>
#include <sstream>
#include <vector>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char* TAG = "Config";
static const char* NVS_NAMESPACE = "dhcp";

// ─── NVS key names ──────────────────────────────────
static const char* KEY_WIFI_SSID      = "wifi_ssid";
static const char* KEY_WIFI_PASS      = "wifi_pass";
static const char* KEY_DHCP_ENABLED   = "dhcp_enabled";
static const char* KEY_DHCP_START     = "dhcp_start";
static const char* KEY_DHCP_END       = "dhcp_end";
static const char* KEY_DHCP_SUBNET    = "dhcp_subnet";
static const char* KEY_DHCP_GW        = "dhcp_gw";
static const char* KEY_DHCP_SERVER_IP = "dhcp_srv_ip";
static const char* KEY_DHCP_LEASE     = "dhcp_lease";
static const char* KEY_DHCP_MAX_LEASES = "dhcp_max_ent";
static const char* KEY_DHCP_LOG_TERM   = "dhcp_log_term";
// Allowed computers / "only allowed" policy (NVS keys limited to 15 chars).
static const char* KEY_DHCP_ALLOW_ONLY = "dhcp_allow_only";
static const char* KEY_DHCP_ALLOWED    = "dhcp_allowed";
static const char* KEY_DHCP_BINDINGS  = "dhcp_bindings";
static const char* KEY_DHCP_DNS_MODE  = "dhcp_dns_mode";
static const char* KEY_DHCP_DNS_ADDR  = "dhcp_dns_addr";
static const char* KEY_DHCP_LOG_REST  = "dhcp_log_rest";
static const char* KEY_DHCP_LOG_URL   = "dhcp_log_url";
static const char* KEY_DHCP_LOG_AUTH  = "dhcp_log_auth";
static const char* KEY_DHCP_LOG_AUTH_U = "dhcp_log_auth_u";
static const char* KEY_DHCP_LOG_AUTH_P = "dhcp_log_auth_p";
static const char* KEY_DNS_ENABLED    = "dns_enabled";
static const char* KEY_DNS_EXTERNAL   = "dns_external";
static const char* KEY_DNS_LOG_TERM   = "dns_log_term";
static const char* KEY_DNS_LOG_FWD    = "dns_log_fwd";
static const char* KEY_DNS_LOG_LOCAL  = "dns_log_local";
static const char* KEY_DNS_LOG_CACHE  = "dns_log_cache";
static const char* KEY_DNS_LOG_RST_S  = "dns_rest_sent";
static const char* KEY_DNS_LOG_REST   = "dns_log_rest";
static const char* KEY_DNS_LOG_URL    = "dns_log_url";
static const char* KEY_DNS_LOG_AUTH   = "dns_log_auth";
static const char* KEY_DNS_LOG_AUTH_U = "dns_log_auth_u";
static const char* KEY_DNS_LOG_AUTH_P = "dns_log_auth_p";
static const char* KEY_DNS_CACHE_REST = "dns_cache_rest";
static const char* KEY_DNS_CACHE_READ = "dns_cache_read";
static const char* KEY_DNS_CACHE_WRITE = "dns_cache_writ";
static const char* KEY_DNS_CACHE_URL  = "dns_cache_url";
static const char* KEY_DNS_CACHE_AUTH   = "dns_cache_auth";
// NVS keys are limited to 15 chars — "dns_cache_auth_u/p" (16) failed with
// ESP_ERR_NVS_KEY_TOO_LONG, silently dropping the cached credentials.
static const char* KEY_DNS_CACHE_AUTH_U = "dns_cach_auth_u";
static const char* KEY_DNS_CACHE_AUTH_P = "dns_cach_auth_p";
static const char* KEY_DNS_IC_ENABLE  = "dns_ic_enable";
static const char* KEY_DNS_IC_SIZE_MB = "dns_ic_size_mb";
static const char* KEY_DNS_IC_IGN_TTL = "dns_ic_ign_ttl";
static const char* KEY_DNS_IC_STATS   = "dns_ic_stats";
static const char* KEY_DNS_IC_SAVE_CACHE = "dns_ic_save_cache";
static const char* KEY_DNS_IC_FILE_MD5 = "dns_ic_file_md5";
static const char* KEY_DNS_BLK_NON_AA = "dns_blk_nonaa";
static const char* KEY_DNS_ALLOW_LAN = "dns_allow_lan";
static const char* KEY_DNS_HOSTS      = "dns_hosts";
static const char* KEY_DNS_CACHE_DATA = "dns_cache";
static const char* KEY_SEC_USER       = "sec_user";
static const char* KEY_SEC_PASS       = "sec_pass";
static const char* KEY_SEC_MAX_ATT    = "sec_max_att";
static const char* KEY_SEC_LOCKOUT    = "sec_lockout";
// Time (NTP) server settings (NVS keys limited to 15 chars).
static const char* KEY_TIME_ENABLED   = "time_enabled";
static const char* KEY_TIME_SYNC_EN   = "time_sync_en";
static const char* KEY_TIME_ALLOW_LAN = "time_allow_lan";
static const char* KEY_TIME_RATE      = "time_rate";
static const char* KEY_TIME_NTP       = "time_ntp";
static const char* KEY_TIME_TZ_NAME   = "time_tz_name";
static const char* KEY_TIME_TZ_H      = "time_tz_h";
static const char* KEY_TIME_SYNC_S    = "time_sync_s";
static const char* KEY_TIME_LOG_TERM  = "time_log_term";
static const char* KEY_TIME_LOG_REST  = "time_log_rest";
static const char* KEY_TIME_LOG_URL   = "time_log_url";
static const char* KEY_TIME_LOG_AUTH  = "time_log_auth";
static const char* KEY_TIME_LOG_AUTH_U = "time_log_auth_u";
static const char* KEY_TIME_LOG_AUTH_P = "time_log_auth_p";
// File explorer settings (NVS keys limited to 15 chars).
static const char* KEY_FILES_ALLOW_LAN = "files_allow_lan";

namespace dhcp {
namespace core {

// ─── Singleton ──────────────────────────────────────

Config& Config::instance()
{
    static Config inst;
    return inst;
}

// ─── NVS helpers ────────────────────────────────────

static nvs_handle_t openNvs()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return 0;
    }
    return handle;
}

std::string Config::readStr(const char* key, const std::string& def)
{
    nvs_handle_t h = openNvs();
    if (!h) return def;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return def;
    }
    std::string val(len, '\0');
    err = nvs_get_str(h, key, &val[0], &len);
    nvs_close(h);
    if (err != ESP_OK) return def;
    // Remove trailing null that NVS may include
    val.resize(strlen(val.c_str()));
    return val;
}

void Config::writeStr(const char* key, const std::string& val)
{
    nvs_handle_t h = openNvs();
    if (!h) return;
    esp_err_t err = nvs_set_str(h, key, val.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed (%s)", key, esp_err_to_name(err));
    } else {
        nvs_commit(h);
    }
    nvs_close(h);
}

int32_t Config::readI32(const char* key, int32_t def)
{
    nvs_handle_t h = openNvs();
    if (!h) return def;
    int32_t val = def;
    esp_err_t err = nvs_get_i32(h, key, &val);
    nvs_close(h);
    return (err == ESP_OK) ? val : def;
}

void Config::writeI32(const char* key, int32_t val)
{
    nvs_handle_t h = openNvs();
    if (!h) return;
    esp_err_t err = nvs_set_i32(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s) failed (%s)", key, esp_err_to_name(err));
    } else {
        nvs_commit(h);
    }
    nvs_close(h);
}

bool Config::readBlob(const char* key, std::vector<uint8_t>& out)
{
    nvs_handle_t h = openNvs();
    if (!h) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, nullptr, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return false;
    }
    out.resize(len);
    err = nvs_get_blob(h, key, out.data(), &len);
    nvs_close(h);
    return (err == ESP_OK);
}

bool Config::writeBlob(const char* key, const std::vector<uint8_t>& data)
{
    nvs_handle_t h = openNvs();
    if (!h) return false;
    esp_err_t err = nvs_set_blob(h, key, data.data(), data.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s) failed (%s)", key, esp_err_to_name(err));
        nvs_close(h);
        return false;
    }
    nvs_commit(h);
    nvs_close(h);
    return true;
}

bool Config::eraseKey(const char* key)
{
    nvs_handle_t h = openNvs();
    if (!h) return false;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

bool Config::resetAll()
{
    nvs_handle_t h = openNvs();
    if (!h) return false;
    esp_err_t err = nvs_erase_all(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_all failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "All settings erased (factory reset)");
    return true;
}

// ─── WiFi ───────────────────────────────────────────

WifiConfig Config::getWifi() const
{
    WifiConfig cfg;
    cfg.ssid = readStr(KEY_WIFI_SSID, "");
    cfg.password = readStr(KEY_WIFI_PASS, "");
    return cfg;
}

void Config::setWifi(const WifiConfig& cfg)
{
    writeStr(KEY_WIFI_SSID, cfg.ssid);
    writeStr(KEY_WIFI_PASS, cfg.password);
}

// ─── DHCP ───────────────────────────────────────────

DhcpConfig Config::getDhcp() const
{
    DhcpConfig cfg;
    cfg.enabled = readI32(KEY_DHCP_ENABLED, 0) != 0;
    cfg.startIp = readStr(KEY_DHCP_START, core::kDefaultPoolStart);
    cfg.endIp = readStr(KEY_DHCP_END, core::kDefaultPoolEnd);
    cfg.subnet = readStr(KEY_DHCP_SUBNET, core::kDefaultSubnetMask);
    cfg.gateway = readStr(KEY_DHCP_GW, core::kDefaultGateway);
    cfg.serverIp = readStr(KEY_DHCP_SERVER_IP, core::kDefaultServerIp);
    cfg.leaseTimeSec = static_cast<uint32_t>(readI32(KEY_DHCP_LEASE, static_cast<int32_t>(core::kDefaultLeaseSec)));
    {
        int32_t maxEntries = readI32(KEY_DHCP_MAX_LEASES, 0);
        if (maxEntries != 0) {           // 0 = auto (2x pool size)
            if (maxEntries < core::kMinLeaseEntries) maxEntries = core::kMinLeaseEntries;
            if (maxEntries > core::kMaxLeaseEntries) maxEntries = core::kMaxLeaseEntries;
        }
        cfg.maxLeaseEntries = static_cast<uint32_t>(maxEntries);
    }
    cfg.allowOnly = readI32(KEY_DHCP_ALLOW_ONLY, 0) != 0;   // OFF by default
    cfg.logTerminal = readI32(KEY_DHCP_LOG_TERM, 0) != 0;
    cfg.dnsMode = readStr(KEY_DHCP_DNS_MODE, "auto");
    cfg.dnsAddress = readStr(KEY_DHCP_DNS_ADDR, "");
    cfg.logRest = readI32(KEY_DHCP_LOG_REST, 0) != 0;
    cfg.logUrl = readStr(KEY_DHCP_LOG_URL, "");
    cfg.logAuthEnabled = readI32(KEY_DHCP_LOG_AUTH, 0) != 0;
    cfg.logAuthUser = readStr(KEY_DHCP_LOG_AUTH_U, "");
    cfg.logAuthPassword = readStr(KEY_DHCP_LOG_AUTH_P, "");
    return cfg;
}

void Config::setDhcp(const DhcpConfig& cfg)
{
    writeI32(KEY_DHCP_ENABLED, cfg.enabled ? 1 : 0);
    writeStr(KEY_DHCP_START, cfg.startIp);
    writeStr(KEY_DHCP_END, cfg.endIp);
    writeStr(KEY_DHCP_SUBNET, cfg.subnet);
    writeStr(KEY_DHCP_GW, cfg.gateway);
    writeStr(KEY_DHCP_SERVER_IP, cfg.serverIp);
    writeI32(KEY_DHCP_LEASE, static_cast<int32_t>(cfg.leaseTimeSec));
    writeI32(KEY_DHCP_MAX_LEASES, static_cast<int32_t>(cfg.maxLeaseEntries));
    writeI32(KEY_DHCP_ALLOW_ONLY, cfg.allowOnly ? 1 : 0);
    writeI32(KEY_DHCP_LOG_TERM, cfg.logTerminal ? 1 : 0);
    writeStr(KEY_DHCP_DNS_MODE, cfg.dnsMode.empty() ? "auto" : cfg.dnsMode);
    writeStr(KEY_DHCP_DNS_ADDR, cfg.dnsAddress);
    writeI32(KEY_DHCP_LOG_REST, cfg.logRest ? 1 : 0);
    writeStr(KEY_DHCP_LOG_URL, cfg.logUrl);
    writeI32(KEY_DHCP_LOG_AUTH, cfg.logAuthEnabled ? 1 : 0);
    writeStr(KEY_DHCP_LOG_AUTH_U, cfg.logAuthUser);
    writeStr(KEY_DHCP_LOG_AUTH_P, cfg.logAuthPassword);
}

// ─── Static bindings ────────────────────────────────

std::vector<StaticBinding> Config::getStaticBindings() const
{
    std::vector<StaticBinding> result;
    std::vector<uint8_t> blob;
    if (!readBlob(KEY_DHCP_BINDINGS, blob)) return result;

    // Format: "mac1|ip1|name1|gateway1|useGateway1|enabled1|useDns1\n..."
    // (backward compatible: "mac|ip" or "mac|ip|name")
    std::string text(blob.begin(), blob.end());
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        // Split on '|'
        std::vector<std::string> f;
        size_t start = 0;
        while (true) {
            size_t p = line.find('|', start);
            if (p == std::string::npos) {
                f.push_back(line.substr(start));
                break;
            }
            f.push_back(line.substr(start, p - start));
            start = p + 1;
        }
        StaticBinding b;
        if (f.size() >= 1) b.mac = f[0];
        if (f.size() >= 2) b.ip = f[1];
        if (f.size() >= 3) b.name = f[2];
        if (f.size() >= 4) b.gateway = f[3];
        if (f.size() >= 5) b.useGateway = (f[4] != "0");
        if (f.size() >= 6) b.enabled = (f[5] != "0");
        if (f.size() >= 7) b.useDns = (f[6] != "0");
        if (!b.mac.empty() && !b.ip.empty()) {
            result.push_back(b);
        }
    }
    return result;
}

bool Config::setStaticBindings(const std::vector<StaticBinding>& bindings)
{
    if (bindings.empty()) {
        return eraseKey(KEY_DHCP_BINDINGS);
    }
    std::string text;
    for (const auto& b : bindings) {
        if (!text.empty()) text += '\n';
        text += b.mac + '|' + b.ip + '|' + b.name + '|' + b.gateway + '|' +
                (b.useGateway ? "1" : "0") + '|' + (b.enabled ? "1" : "0") + '|' +
                (b.useDns ? "1" : "0");
    }
    if (text.size() > kMaxBindingsBytes) {
        ESP_LOGE(TAG, "Static bindings too large: %zu > %zu",
                 text.size(), kMaxBindingsBytes);
        return false;
    }
    std::vector<uint8_t> blob(text.begin(), text.end());
    return writeBlob(KEY_DHCP_BINDINGS, blob);
}

size_t Config::staticBindingsBytes() const
{
    // Serialize exactly like setStaticBindings() so the reported usage matches
    // the NVS blob size that the kMaxBindingsBytes limit is applied to.
    auto bindings = getStaticBindings();
    std::string text;
    for (const auto& b : bindings) {
        if (!text.empty()) text += '\n';
        text += b.mac + '|' + b.ip + '|' + b.name + '|' + b.gateway + '|' +
                (b.useGateway ? "1" : "0") + '|' + (b.enabled ? "1" : "0") + '|' +
                (b.useDns ? "1" : "0");
    }
    return text.size();
}

// ─── Allowed computers (DHCP allow-list) ────────────

std::string Config::getAllowedComputers() const
{
    std::vector<uint8_t> blob;
    if (!readBlob(KEY_DHCP_ALLOWED, blob)) return std::string();
    return std::string(blob.begin(), blob.end());
}

bool Config::setAllowedComputers(const std::string& text)
{
    if (text.empty()) {
        return eraseKey(KEY_DHCP_ALLOWED);
    }
    if (text.size() > kMaxAllowedBytes) {
        ESP_LOGE(TAG, "Allowed computers too large: %zu > %zu",
                 text.size(), kMaxAllowedBytes);
        return false;
    }
    std::vector<uint8_t> blob(text.begin(), text.end());
    return writeBlob(KEY_DHCP_ALLOWED, blob);
}

size_t Config::allowedComputersBytes() const
{
    return getAllowedComputers().size();
}

// ─── DNS ────────────────────────────────────────────

DnsConfig Config::getDns() const
{
    DnsConfig cfg;
    cfg.enabled = readI32(KEY_DNS_ENABLED, 1) != 0;
    cfg.externalDns = readStr(KEY_DNS_EXTERNAL, core::kDefaultExternalDns);
    cfg.logTerminal = readI32(KEY_DNS_LOG_TERM, 0) != 0;
    cfg.logForwarded = readI32(KEY_DNS_LOG_FWD, 1) != 0;
    cfg.logLocal = readI32(KEY_DNS_LOG_LOCAL, 1) != 0;
    cfg.logCache = readI32(KEY_DNS_LOG_CACHE, 1) != 0;
    cfg.logRestSent = readI32(KEY_DNS_LOG_RST_S, 0) != 0;
    cfg.logRest = readI32(KEY_DNS_LOG_REST, 0) != 0;
    cfg.logUrl = readStr(KEY_DNS_LOG_URL, "");
    cfg.logAuthEnabled = readI32(KEY_DNS_LOG_AUTH, 0) != 0;
    cfg.logAuthUser = readStr(KEY_DNS_LOG_AUTH_U, "");
    cfg.logAuthPassword = readStr(KEY_DNS_LOG_AUTH_P, "");
    cfg.cacheRest = readI32(KEY_DNS_CACHE_REST, 0) != 0;
    cfg.cacheRestRead = readI32(KEY_DNS_CACHE_READ, 1) != 0;
    cfg.cacheRestWrite = readI32(KEY_DNS_CACHE_WRITE, 1) != 0;
    cfg.cacheUrl = readStr(KEY_DNS_CACHE_URL, "");
    cfg.cacheAuthEnabled = readI32(KEY_DNS_CACHE_AUTH, 0) != 0;
    cfg.cacheAuthUser = readStr(KEY_DNS_CACHE_AUTH_U, "");
    cfg.cacheAuthPassword = readStr(KEY_DNS_CACHE_AUTH_P, "");
    cfg.cacheInternal = readI32(KEY_DNS_IC_ENABLE, 0) != 0;
    {
        uint32_t sz = static_cast<uint32_t>(readI32(KEY_DNS_IC_SIZE_MB, 20));
        if (sz < 1) sz = 1;
        if (sz > 20) sz = 20;  // cap 20 MB (fits cache.dat on the FAT partition)
        cfg.cacheInternalSizeMb = sz;
    }
    cfg.cacheInternalIgnoreTtl = readI32(KEY_DNS_IC_IGN_TTL, 0) != 0;
    cfg.cacheInternalSaveStats = readI32(KEY_DNS_IC_STATS, 1) != 0;  // default ON
    cfg.cacheInternalSaveCache = readI32(KEY_DNS_IC_SAVE_CACHE, 1) != 0;  // default ON
    cfg.cacheInternalFileMd5 = readStr(KEY_DNS_IC_FILE_MD5, "");
    cfg.blockForwardNonAA = readI32(KEY_DNS_BLK_NON_AA, 0) != 0;
    cfg.allowOwnSubnet = readI32(KEY_DNS_ALLOW_LAN, 1) != 0;   // default ON
    return cfg;
}

void Config::setDns(const DnsConfig& cfg)
{
    writeI32(KEY_DNS_ENABLED, cfg.enabled ? 1 : 0);
    writeStr(KEY_DNS_EXTERNAL, cfg.externalDns);
    writeI32(KEY_DNS_LOG_TERM, cfg.logTerminal ? 1 : 0);
    writeI32(KEY_DNS_LOG_FWD, cfg.logForwarded ? 1 : 0);
    writeI32(KEY_DNS_LOG_LOCAL, cfg.logLocal ? 1 : 0);
    writeI32(KEY_DNS_LOG_CACHE, cfg.logCache ? 1 : 0);
    writeI32(KEY_DNS_LOG_RST_S, cfg.logRestSent ? 1 : 0);
    writeI32(KEY_DNS_LOG_REST, cfg.logRest ? 1 : 0);
    writeStr(KEY_DNS_LOG_URL, cfg.logUrl);
    writeI32(KEY_DNS_LOG_AUTH, cfg.logAuthEnabled ? 1 : 0);
    writeStr(KEY_DNS_LOG_AUTH_U, cfg.logAuthUser);
    writeStr(KEY_DNS_LOG_AUTH_P, cfg.logAuthPassword);
    writeI32(KEY_DNS_CACHE_REST, cfg.cacheRest ? 1 : 0);
    writeI32(KEY_DNS_CACHE_READ, cfg.cacheRestRead ? 1 : 0);
    writeI32(KEY_DNS_CACHE_WRITE, cfg.cacheRestWrite ? 1 : 0);
    writeStr(KEY_DNS_CACHE_URL, cfg.cacheUrl);
    writeI32(KEY_DNS_CACHE_AUTH, cfg.cacheAuthEnabled ? 1 : 0);
    writeStr(KEY_DNS_CACHE_AUTH_U, cfg.cacheAuthUser);
    writeStr(KEY_DNS_CACHE_AUTH_P, cfg.cacheAuthPassword);
    writeI32(KEY_DNS_IC_ENABLE, cfg.cacheInternal ? 1 : 0);
    writeI32(KEY_DNS_IC_SIZE_MB, static_cast<int32_t>(cfg.cacheInternalSizeMb));
    writeI32(KEY_DNS_IC_IGN_TTL, cfg.cacheInternalIgnoreTtl ? 1 : 0);
    writeI32(KEY_DNS_IC_STATS, cfg.cacheInternalSaveStats ? 1 : 0);
    writeI32(KEY_DNS_IC_SAVE_CACHE, cfg.cacheInternalSaveCache ? 1 : 0);
    writeStr(KEY_DNS_IC_FILE_MD5, cfg.cacheInternalFileMd5);
    writeI32(KEY_DNS_BLK_NON_AA, cfg.blockForwardNonAA ? 1 : 0);
    writeI32(KEY_DNS_ALLOW_LAN, cfg.allowOwnSubnet ? 1 : 0);
}

// ─── Local DNS hosts ────────────────────────────────

std::vector<LocalHostEntry> Config::getLocalHosts() const
{
    std::vector<LocalHostEntry> result;
    std::vector<uint8_t> blob;
    if (!readBlob(KEY_DNS_HOSTS, blob)) return result;

    // Format: "name1|ip4|ip6|enabled\n..."
    // (backward compatible: "name|ip" -> ip4, or "name|ip4|ip6" -> enabled=true)
    std::string text(blob.begin(), blob.end());
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        auto p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        auto p2 = line.find('|', p1 + 1);
        LocalHostEntry e;
        e.name = line.substr(0, p1);
        if (p2 != std::string::npos) {
            auto p3 = line.find('|', p2 + 1);
            if (p3 != std::string::npos) {
                e.ip4 = line.substr(p1 + 1, p2 - p1 - 1);
                e.ip6 = line.substr(p2 + 1, p3 - p2 - 1);
                e.enabled = line.substr(p3 + 1) != "0";
            } else {
                e.ip4 = line.substr(p1 + 1, p2 - p1 - 1);
                e.ip6 = line.substr(p2 + 1);
            }
        } else {
            e.ip4 = line.substr(p1 + 1);
        }
        if (!e.name.empty() && (!e.ip4.empty() || !e.ip6.empty())) {
            result.push_back(e);
        }
    }
    return result;
}

bool Config::setLocalHosts(const std::vector<LocalHostEntry>& hosts)
{
    if (hosts.empty()) {
        return eraseKey(KEY_DNS_HOSTS);
    }
    std::string text;
    for (const auto& h : hosts) {
        if (!text.empty()) text += '\n';
        text += h.name + '|' + h.ip4 + '|' + h.ip6 + '|' + (h.enabled ? "1" : "0");
    }
    if (text.size() > kMaxLocalHostsBytes) {
        ESP_LOGE(TAG, "Local hosts too large: %zu > %zu",
                 text.size(), kMaxLocalHostsBytes);
        return false;
    }
    std::vector<uint8_t> blob(text.begin(), text.end());
    return writeBlob(KEY_DNS_HOSTS, blob);
}

size_t Config::localHostsBytes() const
{
    // Serialize exactly like setLocalHosts() so the reported usage matches
    // the NVS blob size that the kMaxLocalHostsBytes limit is applied to.
    auto hosts = getLocalHosts();
    std::string text;
    for (const auto& h : hosts) {
        if (!text.empty()) text += '\n';
        text += h.name + '|' + h.ip4 + '|' + h.ip6 + '|' + (h.enabled ? "1" : "0");
    }
    return text.size();
}

// ─── DNS cache blob ─────────────────────────────────

std::vector<uint8_t> Config::getDnsCache() const
{
    std::vector<uint8_t> data;
    readBlob(KEY_DNS_CACHE_DATA, data);
    return data;
}

bool Config::setDnsCache(const std::vector<uint8_t>& data)
{
    if (data.size() > kMaxDnsCacheBytes) {
        ESP_LOGE(TAG, "DNS cache too large: %zu > %zu",
                 data.size(), kMaxDnsCacheBytes);
        return false;
    }
    return writeBlob(KEY_DNS_CACHE_DATA, data);
}

// ─── Security ───────────────────────────────────────

SecurityConfig Config::getSecurity() const
{
    SecurityConfig cfg;
    cfg.username = readStr(KEY_SEC_USER, "admin");
    cfg.password = readStr(KEY_SEC_PASS, "admin");
    cfg.maxAttempts = static_cast<uint32_t>(readI32(KEY_SEC_MAX_ATT, static_cast<int32_t>(SecurityConfig::kDefaultMaxAttempts)));
    cfg.lockoutPeriodSec = static_cast<uint32_t>(readI32(KEY_SEC_LOCKOUT, static_cast<int32_t>(SecurityConfig::kDefaultLockoutSec)));
    return cfg;
}

void Config::setSecurity(const SecurityConfig& cfg)
{
    writeStr(KEY_SEC_USER, cfg.username);
    writeStr(KEY_SEC_PASS, cfg.password);
    writeI32(KEY_SEC_MAX_ATT, static_cast<int32_t>(cfg.maxAttempts));
    writeI32(KEY_SEC_LOCKOUT, static_cast<int32_t>(cfg.lockoutPeriodSec));
}

// ─── Time (NTP) server ──────────────────────────────

TimeConfig Config::getTime() const
{
    TimeConfig cfg;
    cfg.enabled = readI32(KEY_TIME_ENABLED, 0) != 0;
    cfg.syncEnabled = readI32(KEY_TIME_SYNC_EN, 1) != 0;
    cfg.externalNtp = readStr(KEY_TIME_NTP, "pool.ntp.org");
    cfg.timezone = readStr(KEY_TIME_TZ_NAME, "Europe/Moscow");
    cfg.utcOffsetHours = readI32(KEY_TIME_TZ_H, 3);
    cfg.syncIntervalSec =
        static_cast<uint32_t>(readI32(KEY_TIME_SYNC_S, 86400));
    cfg.allowOwnSubnet = readI32(KEY_TIME_ALLOW_LAN, 1) != 0;   // default ON
    {
        // Rule 39: the NTP rate limit is a count per second, so its scale is
        // the same 1..100 the percentage uses.
        static constexpr int32_t kTimeRateMin = 1;
        static constexpr int32_t kTimeRateMax = 100;
        int32_t rate = readI32(KEY_TIME_RATE, 5);
        if (rate < kTimeRateMin) rate = kTimeRateMin;
        if (rate > kTimeRateMax) rate = kTimeRateMax;
        cfg.rateLimitPerSec = static_cast<uint32_t>(rate);
    }
    cfg.logTerminal = readI32(KEY_TIME_LOG_TERM, 0) != 0;
    cfg.logRest = readI32(KEY_TIME_LOG_REST, 0) != 0;
    cfg.logUrl = readStr(KEY_TIME_LOG_URL, "");
    cfg.logAuthEnabled = readI32(KEY_TIME_LOG_AUTH, 0) != 0;
    cfg.logAuthUser = readStr(KEY_TIME_LOG_AUTH_U, "");
    cfg.logAuthPassword = readStr(KEY_TIME_LOG_AUTH_P, "");
    return cfg;
}

void Config::setTime(const TimeConfig& cfg)
{
    writeI32(KEY_TIME_ENABLED, cfg.enabled ? 1 : 0);
    writeI32(KEY_TIME_SYNC_EN, cfg.syncEnabled ? 1 : 0);
    writeStr(KEY_TIME_NTP, cfg.externalNtp);
    writeStr(KEY_TIME_TZ_NAME, cfg.timezone);
    writeI32(KEY_TIME_TZ_H, cfg.utcOffsetHours);
    writeI32(KEY_TIME_SYNC_S, static_cast<int32_t>(cfg.syncIntervalSec));
    writeI32(KEY_TIME_ALLOW_LAN, cfg.allowOwnSubnet ? 1 : 0);
    writeI32(KEY_TIME_RATE, static_cast<int32_t>(cfg.rateLimitPerSec));
    writeI32(KEY_TIME_LOG_TERM, cfg.logTerminal ? 1 : 0);
    writeI32(KEY_TIME_LOG_REST, cfg.logRest ? 1 : 0);
    writeStr(KEY_TIME_LOG_URL, cfg.logUrl);
    writeI32(KEY_TIME_LOG_AUTH, cfg.logAuthEnabled ? 1 : 0);
    writeStr(KEY_TIME_LOG_AUTH_U, cfg.logAuthUser);
    writeStr(KEY_TIME_LOG_AUTH_P, cfg.logAuthPassword);
}

// ─── File explorer (FAT volumes) ────────────────────

FileConfig Config::getFiles() const
{
    FileConfig cfg;
    cfg.allowOwnSubnet = readI32(KEY_FILES_ALLOW_LAN, 1) != 0;   // default ON
    return cfg;
}

void Config::setFiles(const FileConfig& cfg)
{
    writeI32(KEY_FILES_ALLOW_LAN, cfg.allowOwnSubnet ? 1 : 0);
}

} // namespace core
} // namespace dhcp
