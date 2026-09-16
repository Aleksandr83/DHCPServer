#include "RestApi.h"
#include "AuthManager.h"
#include "FileJson.h"
#include "JsonWriter.h"
#include "MultipartExtractor.h"
#include "../core/Version.h"
#include "../core/Config.h"
#include "../core/CpuMonitor.h"
#include "../core/JobRegistry.h"
#include "../wifi/IWiFiManager.h"
#include "../dhcp/IDhcpServer.h"
#include "../dhcp/DhcpServer.h"
#include "../dns/DnsServer.h"
#include "../files/IFileManager.h"
#include "../storage/PathUtil.h"
#include "../time/TimeServer.h"
#include "../time/TimeMath.h"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char* TAG = "RestApi";

namespace dhcp {
namespace web {

// ─── Static pointer initialization ──────────────────
::dhcp::wifi::IWiFiManager*  RestApi::s_wifi = nullptr;
::dhcp::dhcp::IDhcpServer*   RestApi::s_dhcp = nullptr;
::dhcp::dns::DnsServer*      RestApi::s_dns  = nullptr;
::dhcp::time::TimeServer*    RestApi::s_time = nullptr;
::dhcp::web::AuthManager*    RestApi::s_auth = nullptr;
::dhcp::files::IFileManager* RestApi::s_files = nullptr;

void RestApi::init(::dhcp::wifi::IWiFiManager* wifi,
                    ::dhcp::dhcp::IDhcpServer* dhcpSrv,
                    ::dhcp::dns::DnsServer* dnsSrv,
                    ::dhcp::time::TimeServer* timeSrv,
                    ::dhcp::web::AuthManager* auth,
                    ::dhcp::files::IFileManager* fileMgr)
{
    s_wifi = wifi;
    s_dhcp = dhcpSrv;
    s_dns  = dnsSrv;
    s_time = timeSrv;
    s_auth = auth;
    s_files = fileMgr;
    ESP_LOGI(TAG, "RestApi initialized");
}

// ─────────────────────────────────────────────────────
// Auth helper
// ─────────────────────────────────────────────────────

bool RestApi::checkAuth(httpd_req* req)
{
    if (!s_auth) return false;

    size_t hdrLen = httpd_req_get_hdr_value_len(req, "Authorization");
    std::string authHeader;
    if (hdrLen > 0) {
        authHeader.resize(hdrLen);
        httpd_req_get_hdr_value_str(req, "Authorization", &authHeader[0], hdrLen + 1);
    }

    std::string clientIp = getClientIp(req);

    if (!s_auth->authenticate(authHeader, clientIp)) {
        respondUnauthorized(req);
        return false;
    }
    return true;
}

esp_err_t RestApi::respondUnauthorized(httpd_req* req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       s_auth ? s_auth->wwwAuthenticateHeader().c_str() :
                       "Basic realm=\"DHCPServer\"");
    httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\"}");
    return ESP_OK;
}

uint32_t RestApi::getClientIp4(httpd_req* req)
{
    // The peer of the TCP connection — deliberately NOT the X-Forwarded-For
    // header: the file endpoints decide access on this address, and a client
    // must not be able to claim an address inside the allowed subnet.
    const int sock = httpd_req_to_sockfd(req);
    if (sock < 0) return 0;

    struct sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        ESP_LOGW(TAG, "getpeername failed (%s)", strerror(errno));
        return 0;
    }
    if (addr.sin_family != AF_INET) return 0;

    return ntohl(addr.sin_addr.s_addr);
}

bool RestApi::checkFileAccess(httpd_req* req)
{
    if (!s_files) return true;   // no file service in this build

    const uint32_t clientIp = getClientIp4(req);
    if (s_files->allowClient(clientIp)) return true;

    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"status\":\"error\",\"message\":\"forbidden (client is outside the allowed subnet)\"}");
    return false;
}

std::string RestApi::getClientIp(httpd_req* req)
{
    // Try X-Forwarded-For first
    size_t hdrLen = httpd_req_get_hdr_value_len(req, "X-Forwarded-For");
    if (hdrLen > 0) {
        std::string ip;
        ip.resize(hdrLen);
        httpd_req_get_hdr_value_str(req, "X-Forwarded-For", &ip[0], hdrLen + 1);
        return ip;
    }
    // Fallback: return local network identifier
    // (Client IP extraction from httpd_req varies by ESP-IDF version)
    return "192.168.1.0";
}

// ─────────────────────────────────────────────────────
// JSON helpers
// ─────────────────────────────────────────────────────

void RestApi::addJsonString(std::string& json, const std::string& key,
                             const std::string& val, bool addComma)
{
    if (addComma) json += ",";
    json += "\"" + key + "\":\"" + val + "\"";
}

void RestApi::addJsonBool(std::string& json, const std::string& key,
                           bool val, bool addComma)
{
    if (addComma) json += ",";
    json += "\"" + key + "\":" + (val ? "true" : "false");
}

void RestApi::addJsonInt(std::string& json, const std::string& key,
                          int64_t val, bool addComma)
{
    if (addComma) json += ",";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", (long long)val);
    json += "\"" + key + "\":" + buf;
}

// ─────────────────────────────────────────────────────
// Body reader
// ─────────────────────────────────────────────────────

std::string RestApi::readBody(httpd_req* req, size_t maxLen)
{
    size_t totalLen = req->content_len;
    if (totalLen == 0) return "";
    // Sanity cap: guard against a bogus content_len claiming a huge body
    // (the httpd socket buffer is limited). Most settings bodies are small;
    // the settings-import endpoint passes a larger cap (~16 KB).
    if (totalLen > maxLen) totalLen = maxLen;

    std::string body(totalLen, '\0');
    size_t offset = 0;
    int retries = 0;
    while (offset < totalLen) {
        // httpd_req_recv may return HTTPD_SOCK_ERR_TIMEOUT between TCP
        // segments if the body arrives in several chunks — retry (bounded)
        // instead of treating it as the end of the body (that truncated the
        // LAST JSON fields, e.g. cache_auth_user / cache_auth_password).
        const size_t want = totalLen - offset;
        int ret = httpd_req_recv(req, &body[offset], want);
        if (ret < 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && offset > 0 && retries++ < 20) {
                continue;
            }
            break;
        }
        if (ret == 0) break;
        offset += static_cast<size_t>(ret);
    }
    // Shrink to the bytes actually received so callers can detect truncation
    // (and jsonGetStr/Bool don't see garbage null bytes at the tail).
    body.resize(offset);
    return body;
}

// ─── Simple JSON parser helper ──────────────────────
static std::string jsonGetStr(const std::string& json, const std::string& key)
{
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.length() && json[pos] == ' ') pos++;
    if (pos >= json.length() || json[pos] != '"') return "";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static bool jsonGetBool(const std::string& json, const std::string& key, bool def)
{
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.length() && json[pos] == ' ') pos++;
    if (pos >= json.length()) return def;
    return (json[pos] == 't' || json[pos] == 'T');
}

static int jsonGetInt(const std::string& json, const std::string& key, int def)
{
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.length() && json[pos] == ' ') pos++;
    if (pos >= json.length()) return def;
    return std::atoi(&json[pos]);
}

// ─────────────────────────────────────────────────────
// GET /api/status
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetStatus(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string json = "{";
    addJsonBool(json, "wifi_connected",
                s_wifi ? s_wifi->isConnected() : false, false);
    addJsonString(json, "wifi_ssid",
                  s_wifi ? s_wifi->ssid() : "", true);
    addJsonString(json, "ip4",
                  s_wifi ? s_wifi->ip4() : "", true);
    addJsonString(json, "ip6",
                  s_wifi ? s_wifi->ip6() : "", true);
    addJsonBool(json, "dhcp_running",
                s_dhcp ? s_dhcp->isRunning() : false, true);
    addJsonBool(json, "dns_running",
                s_dns ? s_dns->isRunning() : false, true);
    addJsonBool(json, "ntp_running",
                s_time ? s_time->isRunning() : false, true);
    // Device uptime since boot. The home page shows it next to its own title,
    // and that row has to stay readable while the time service is switched off,
    // so the raw `esp_timer` count is read here instead of asking `s_time` —
    // the same clock `TimeServer::uptimeSec()` uses, which keeps this endpoint
    // and `/api/time/settings` from ever disagreeing.
    addJsonInt(json, "uptime_sec",
               static_cast<int64_t>(esp_timer_get_time() / 1000000ULL), true);
    // File explorer availability: only the ESP32-P4 has FAT volumes (flash
    // data partition + card slot); the web UI hides the menu entry otherwise.
    addJsonBool(json, "files_enabled",
                s_files ? s_files->supported() : false, true);
    // Storage capacity of those volumes — the internal flash data partition and
    // the microSD card. The home page shows them next to RAM, so the status
    // endpoint carries the same array the explorer serves (one shared writer,
    // `FileJson::volumeArray`), and a client outside the allowed subnet still
    // sees the capacities even though `/api/files/*` answers it with 403.
    json += ",\"volumes\":" + FileJson::volumeArray(
        s_files ? s_files->volumes()
                : std::vector<::dhcp::storage::VolumeInfo>{});
    addJsonInt(json, "cpu_load0", ::dhcp::core::CpuMonitor::loadCore0(), true);
    addJsonInt(json, "cpu_load1", ::dhcp::core::CpuMonitor::loadCore1(), true);
    addJsonInt(json, "heap_free",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::freeHeap()), true);
    addJsonInt(json, "heap_total",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::totalHeap()), true);
    addJsonInt(json, "heap_largest",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::largestBlock()), true);
    addJsonInt(json, "ram_total",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::ramTotal()), true);
    addJsonInt(json, "psram_total",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::psramTotal()), true);
    addJsonInt(json, "psram_free",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::psramFree()), true);
    addJsonInt(json, "psram_largest",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::psramLargest()), true);
    addJsonInt(json, "static_bindings_used",
               static_cast<int64_t>(::dhcp::core::Config::instance().staticBindingsBytes()), true);
    addJsonInt(json, "static_bindings_max",
               static_cast<int64_t>(::dhcp::core::Config::kMaxBindingsBytes), true);
    addJsonInt(json, "local_hosts_used",
               static_cast<int64_t>(::dhcp::core::Config::instance().localHostsBytes()), true);
    addJsonInt(json, "local_hosts_max",
               static_cast<int64_t>(::dhcp::core::Config::kMaxLocalHostsBytes), true);
    // Built-in (PSRAM) DNS cache statistics + per-query counters
    {
        ::dhcp::dns::InternalDnsCache::Stats st;
        int64_t icHits = 0, fwd = 0, avgUs = 0;
        if (s_dns) {
            st = s_dns->internalCacheStats();
            icHits = s_dns->internalCacheHits();
            fwd = s_dns->forwardedCount();
            avgUs = s_dns->internalCacheAvgHitUs();
        }
        addJsonBool(json, "internal_cache_available", st.available, true);
        addJsonInt(json, "internal_cache_size_mb",
                   static_cast<int64_t>(st.sizeMb), true);
        addJsonInt(json, "internal_cache_entries",
                   static_cast<int64_t>(st.entries), true);
        addJsonInt(json, "internal_cache_capacity",
                   static_cast<int64_t>(st.capacity), true);
        addJsonInt(json, "internal_cache_used_bytes",
                   static_cast<int64_t>(st.usedBytes), true);
        addJsonInt(json, "internal_cache_free_bytes",
                   static_cast<int64_t>(st.freeBytes), true);
        addJsonInt(json, "internal_cache_hits", icHits, true);
        addJsonInt(json, "internal_cache_avg_hit_us", avgUs, true);
        addJsonInt(json, "internal_forward_count", fwd, true);
    }
    addJsonString(json, "firmware_version",
                  ::dhcp::core::Version::instance().toString(), true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/version
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetVersion(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string json = "{\"firmware_version\":\"";
    json += ::dhcp::core::Version::instance().toString();
    json += "\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/dhcp/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetDhcpSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    auto cfg = ::dhcp::core::Config::instance().getDhcp();
    std::string json = "{";
    addJsonBool(json, "enabled", cfg.enabled, false);
    addJsonString(json, "server_state",
                  s_dhcp ? s_dhcp->stateString() : "unknown", true);
    addJsonString(json, "start_ip", cfg.startIp, true);
    addJsonString(json, "end_ip", cfg.endIp, true);
    addJsonString(json, "subnet", cfg.subnet, true);
    addJsonString(json, "gateway", cfg.gateway, true);
    addJsonString(json, "server_ip", cfg.serverIp, true);
    addJsonBool(json, "log_terminal", cfg.logTerminal, true);
    addJsonBool(json, "log_rest", cfg.logRest, true);
    addJsonString(json, "log_url", cfg.logUrl, true);
    addJsonBool(json, "log_auth", cfg.logAuthEnabled, true);
    addJsonString(json, "log_auth_user", cfg.logAuthUser, true);
    addJsonString(json, "log_auth_password", cfg.logAuthPassword, true);
    addJsonString(json, "dns_mode", cfg.dnsMode, true);
    addJsonString(json, "dns_address", cfg.dnsAddress, true);
    addJsonBool(json, "dns_running",
                s_dns ? s_dns->isRunning() : false, true);
    addJsonInt(json, "lease_time", cfg.leaseTimeSec, true);
    addJsonInt(json, "max_lease_entries",
               static_cast<int64_t>(cfg.maxLeaseEntries), true);
    addJsonInt(json, "lease_count",
               s_dhcp ? static_cast<int64_t>(s_dhcp->leaseCount()) : 0, true);
    addJsonInt(json, "max_lease_entries_effective",
               s_dhcp ? static_cast<int64_t>(s_dhcp->maxLeaseEntriesEffective()) : 0,
               true);
    addJsonInt(json, "lease_limit_rejects",
               s_dhcp ? static_cast<int64_t>(s_dhcp->leaseLimitRejects()) : 0,
               true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/dhcp/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostDhcpSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    ::dhcp::core::DhcpConfig cfg;
    cfg.enabled = jsonGetBool(body, "enabled", false);
    cfg.startIp = jsonGetStr(body, "start_ip");
    if (cfg.startIp.empty()) cfg.startIp = "192.168.1.100";
    cfg.endIp = jsonGetStr(body, "end_ip");
    if (cfg.endIp.empty()) cfg.endIp = "192.168.1.200";
    cfg.subnet = jsonGetStr(body, "subnet");
    if (cfg.subnet.empty()) cfg.subnet = "255.255.255.0";
    cfg.gateway = jsonGetStr(body, "gateway");
    if (cfg.gateway.empty()) cfg.gateway = "192.168.1.1";
    cfg.serverIp = jsonGetStr(body, "server_ip");
    if (cfg.serverIp.empty()) cfg.serverIp = "192.168.1.201";
    cfg.logTerminal = jsonGetBool(body, "log_terminal", false);
    cfg.logRest = jsonGetBool(body, "log_rest", false);
    cfg.logUrl = jsonGetStr(body, "log_url");
    cfg.logAuthEnabled = jsonGetBool(body, "log_auth", false);
    cfg.logAuthUser = jsonGetStr(body, "log_auth_user");
    cfg.logAuthPassword = jsonGetStr(body, "log_auth_password");
    cfg.leaseTimeSec = jsonGetInt(body, "lease_time", 86400);
    {
        int64_t maxEntries = jsonGetInt(body, "max_lease_entries", 0);
        if (maxEntries != 0) {       // 0 = auto (2x pool size)
            if (maxEntries < 8) maxEntries = 8;
            if (maxEntries > 512) maxEntries = 512;
        }
        cfg.maxLeaseEntries = static_cast<uint32_t>(maxEntries);
    }
    cfg.dnsMode = jsonGetStr(body, "dns_mode");
    if (cfg.dnsMode != "manual") cfg.dnsMode = "auto";
    cfg.dnsAddress = jsonGetStr(body, "dns_address");

    // body_read < content_len means the POST body was truncated; the last
    // fields (log_auth_user/password) would be lost.
    ESP_LOGI(TAG, "DHCP POST: content_len=%d body_read=%zu log_auth=%d log_user=%s",
             req->content_len, body.size(),
             cfg.logAuthEnabled ? 1 : 0,
             cfg.logAuthUser.empty() ? "-" : cfg.logAuthUser.c_str());

    ::dhcp::core::Config::instance().setDhcp(cfg);

    // Apply enable/disable to running server
    if (s_dhcp) {
        s_dhcp->setLogTerminal(cfg.logTerminal);
        s_dhcp->setRestLogging(cfg.logRest, cfg.logUrl,
                               cfg.logAuthEnabled, cfg.logAuthUser,
                               cfg.logAuthPassword);
        // Re-apply the lease/offer table cap (config was just stored).
        s_dhcp->applyLeaseLimit();

        if (cfg.enabled && !s_dhcp->isRunning()) {
            // Check WiFi before starting
            bool wifiOk = s_wifi && s_wifi->isConnected();
            if (!wifiOk) {
                ESP_LOGW(TAG, "Cannot start DHCP server: WiFi not connected");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"WiFi not connected\"}");
                return ESP_OK;
            }
            if (s_dhcp->start()) {
                ESP_LOGI(TAG, "DHCP server started via API");
            } else {
                ESP_LOGE(TAG, "DHCP server failed to start via API");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to start DHCP server\"}");
                return ESP_OK;
            }
        } else if (!cfg.enabled && s_dhcp->isRunning()) {
            s_dhcp->stop();
            ESP_LOGI(TAG, "DHCP server stopped via API");
        }
    }

    ESP_LOGI(TAG, "DHCP settings updated (enabled=%d, log_terminal=%d)", cfg.enabled, cfg.logTerminal);

    // The DHCP settings own the subnet used by the DNS/NTP "own subnet only"
    // client filters — re-evaluate them when the subnet (or the IP) changes.
    if (s_dns) s_dns->applySubnetFilter();
    if (s_time) s_time->applyAccessFilter();
    if (s_files) s_files->applyAccessFilter();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/dhcp/static-bindings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetStaticBindings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    auto bindings = ::dhcp::core::Config::instance().getStaticBindings();
    std::string json = "{\"bindings\":[";
    for (size_t i = 0; i < bindings.size(); i++) {
        if (i > 0) json += ",";
        json += "{\"mac\":\"" + bindings[i].mac + "\",";
        json += "\"ip\":\"" + bindings[i].ip + "\",";
        json += "\"name\":\"" + bindings[i].name + "\",";
        json += "\"gateway\":\"" + bindings[i].gateway + "\",";
        json += std::string("\"use_gateway\":") + (bindings[i].useGateway ? "true" : "false") + ",";
        json += std::string("\"enabled\":") + (bindings[i].enabled ? "true" : "false") + ",";
        json += std::string("\"use_dns\":") + (bindings[i].useDns ? "true" : "false") + "}";
    }
    json += "]}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/dhcp/static-bindings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostStaticBindings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    // Parse JSON array of bindings
    std::vector<::dhcp::core::StaticBinding> bindings;
    // Simple parser: find "mac":"...","ip":"...","name":"..." patterns
    size_t pos = 0;
    while ((pos = body.find("\"mac\"", pos)) != std::string::npos) {
        ::dhcp::core::StaticBinding b;
        b.mac = jsonGetStr(body.substr(pos), "mac");
        b.ip = jsonGetStr(body.substr(pos), "ip");
        b.name = jsonGetStr(body.substr(pos), "name");
        b.gateway = jsonGetStr(body.substr(pos), "gateway");
        b.useGateway = jsonGetBool(body.substr(pos), "use_gateway", true);
        b.enabled = jsonGetBool(body.substr(pos), "enabled", true);
        b.useDns = jsonGetBool(body.substr(pos), "use_dns", true);
        if (!b.mac.empty() && !b.ip.empty()) {
            bindings.push_back(b);
        }
        pos++;
    }

    ::dhcp::core::Config::instance().setStaticBindings(bindings);
    ESP_LOGI(TAG, "Static bindings updated (%zu entries)", bindings.size());

    // Apply immediately to the running DHCP server (so gateway / use_gateway
    // changes take effect without a reboot).
    if (s_dhcp) {
        s_dhcp->reloadStaticBindings();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/dhcp/leases
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetLeases(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    // Try to get leases from DhcpServer
    auto* dhcpFull = static_cast<::dhcp::dhcp::DhcpServer*>(s_dhcp);
    std::string json = "{\"leases\":[";
    if (dhcpFull) {
        auto leases = dhcpFull->getLeases();
        for (size_t i = 0; i < leases.size(); i++) {
            if (i > 0) json += ",";
            char macStr[18];
            std::snprintf(macStr, sizeof(macStr),
                          "%02x:%02x:%02x:%02x:%02x:%02x",
                          leases[i].mac[0], leases[i].mac[1],
                          leases[i].mac[2], leases[i].mac[3],
                          leases[i].mac[4], leases[i].mac[5]);
            char ipStr[16];
            inet_ntop(AF_INET, &leases[i].ip, ipStr, sizeof(ipStr));
            json += "{\"mac\":\"" + std::string(macStr) + "\",";
            json += "\"ip\":\"" + std::string(ipStr) + "\",";
            json += "\"expiry\":" + std::to_string(leases[i].expiry) + "}";
        }
    }
    json += "]}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/dns/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetDnsSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    auto cfg = ::dhcp::core::Config::instance().getDns();
    std::string json = "{";
    addJsonBool(json, "enabled", cfg.enabled, false);
    addJsonString(json, "server_state",
                  s_dns ? s_dns->stateString() : "unknown", true);
    addJsonInt(json, "query_count",
               s_dns ? static_cast<int64_t>(s_dns->queryCount()) : 0, true);
    addJsonString(json, "external_dns", cfg.externalDns, true);
    addJsonBool(json, "log_terminal", cfg.logTerminal, true);
    addJsonBool(json, "log_forwarded", cfg.logForwarded, true);
    addJsonBool(json, "log_local", cfg.logLocal, true);
    addJsonBool(json, "log_cache", cfg.logCache, true);
    addJsonBool(json, "log_rest_sent", cfg.logRestSent, true);
    addJsonBool(json, "log_rest", cfg.logRest, true);
    addJsonString(json, "log_url", cfg.logUrl, true);
    addJsonBool(json, "log_auth", cfg.logAuthEnabled, true);
    addJsonString(json, "log_auth_user", cfg.logAuthUser, true);
    addJsonString(json, "log_auth_password", cfg.logAuthPassword, true);
    addJsonString(json, "cache_url", cfg.cacheUrl, true);
    addJsonBool(json, "cache_rest", cfg.cacheRest, true);
    addJsonBool(json, "cache_rest_read", cfg.cacheRestRead, true);
    addJsonBool(json, "cache_rest_write", cfg.cacheRestWrite, true);
    addJsonBool(json, "cache_auth", cfg.cacheAuthEnabled, true);
    addJsonString(json, "cache_auth_user", cfg.cacheAuthUser, true);
    addJsonString(json, "cache_auth_password", cfg.cacheAuthPassword, true);
    addJsonBool(json, "cache_internal", cfg.cacheInternal, true);
    addJsonInt(json, "cache_internal_size_mb", cfg.cacheInternalSizeMb, true);
    addJsonBool(json, "cache_internal_ignore_ttl", cfg.cacheInternalIgnoreTtl, true);
    addJsonBool(json, "block_forward_non_aa", cfg.blockForwardNonAA, true);
    addJsonBool(json, "allow_own_subnet", cfg.allowOwnSubnet, true);
    addJsonBool(json, "cache_internal_available",
                ::dhcp::core::CpuMonitor::psramTotal() > 0, true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/dns/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostDnsSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    ::dhcp::core::DnsConfig cfg;
    cfg.enabled = jsonGetBool(body, "enabled", true);
    cfg.externalDns = jsonGetStr(body, "external_dns");
    if (cfg.externalDns.empty()) cfg.externalDns = "192.168.1.1";
    cfg.logTerminal = jsonGetBool(body, "log_terminal", false);
    cfg.logForwarded = jsonGetBool(body, "log_forwarded", true);
    cfg.logLocal = jsonGetBool(body, "log_local", true);
    cfg.logCache = jsonGetBool(body, "log_cache", true);
    cfg.logRestSent = jsonGetBool(body, "log_rest_sent", false);
    cfg.logRest = jsonGetBool(body, "log_rest", false);
    cfg.logUrl = jsonGetStr(body, "log_url");
    cfg.logAuthEnabled = jsonGetBool(body, "log_auth", false);
    cfg.logAuthUser = jsonGetStr(body, "log_auth_user");
    cfg.logAuthPassword = jsonGetStr(body, "log_auth_password");
    cfg.cacheRest = jsonGetBool(body, "cache_rest", false);
    cfg.cacheRestRead = jsonGetBool(body, "cache_rest_read", true);
    cfg.cacheRestWrite = jsonGetBool(body, "cache_rest_write", true);
    cfg.cacheUrl = jsonGetStr(body, "cache_url");
    cfg.cacheAuthEnabled = jsonGetBool(body, "cache_auth", false);
    cfg.cacheAuthUser = jsonGetStr(body, "cache_auth_user");
    cfg.cacheAuthPassword = jsonGetStr(body, "cache_auth_password");
    cfg.cacheInternal = jsonGetBool(body, "cache_internal", false);
    cfg.cacheInternalSizeMb = static_cast<uint32_t>(
        jsonGetInt(body, "cache_internal_size_mb", 20));
    if (cfg.cacheInternalSizeMb < 1) cfg.cacheInternalSizeMb = 1;
    if (cfg.cacheInternalSizeMb > 20) cfg.cacheInternalSizeMb = 20;
    cfg.cacheInternalIgnoreTtl = jsonGetBool(body, "cache_internal_ignore_ttl", false);
    cfg.blockForwardNonAA = jsonGetBool(body, "block_forward_non_aa", false);
    cfg.allowOwnSubnet = jsonGetBool(body, "allow_own_subnet", true);

    // If body_read < content_len the POST body was truncated — the last
    // fields (cache_auth_user/password) would be lost even though earlier
    // ones (cache_url) survive. This line makes that visible in the terminal.
    ESP_LOGI(TAG, "DNS POST: content_len=%d body_read=%zu cache_url=%s cache_auth=%d cache_user=%s",
             req->content_len, body.size(),
             cfg.cacheUrl.empty() ? "-" : cfg.cacheUrl.c_str(),
             cfg.cacheAuthEnabled ? 1 : 0,
             cfg.cacheAuthUser.empty() ? "-" : cfg.cacheAuthUser.c_str());

    ::dhcp::core::Config::instance().setDns(cfg);

    // Apply enable/disable to the running DNS server and keep DHCP in sync
    if (s_dns) {
        if (cfg.enabled && !s_dns->isRunning()) {
            if (s_dns->start()) {
                ESP_LOGI(TAG, "DNS server started via API");
            } else {
                ESP_LOGE(TAG, "DNS server failed to start via API");
            }
        } else if (!cfg.enabled && s_dns->isRunning()) {
            s_dns->stop();
            ESP_LOGI(TAG, "DNS server stopped via API");
        }
        // Apply logging settings to the running server live
        s_dns->setLogTerminal(cfg.logTerminal);
        s_dns->logger().setLogForwarded(cfg.logForwarded);
        s_dns->logger().setLogLocal(cfg.logLocal);
        s_dns->logger().setLogCache(cfg.logCache);
        s_dns->logger().setLogRestSent(cfg.logRestSent);
        s_dns->logger().setLogRest(cfg.logRest);
        s_dns->logger().setLogUrl(cfg.logUrl);
        s_dns->logger().setLogAuth(cfg.logAuthEnabled,
                                   cfg.logAuthUser, cfg.logAuthPassword);
        s_dns->cache().setEnabled(cfg.cacheRest);
        s_dns->cache().setReadEnabled(cfg.cacheRestRead);
        s_dns->cache().setWriteEnabled(cfg.cacheRestWrite);
        s_dns->cache().setUrl(cfg.cacheUrl);
        s_dns->cache().setAuth(cfg.cacheAuthEnabled,
                               cfg.cacheAuthUser, cfg.cacheAuthPassword);
        // Built-in PSRAM cache — apply enable/size/ignore-ttl live.
        s_dns->applyInternalCache(cfg.cacheInternal,
                                  cfg.cacheInternalSizeMb,
                                  cfg.cacheInternalIgnoreTtl);
        // Block forwarding of non-A/AAAA queries — apply live.
        s_dns->setBlockForwardNonAA(cfg.blockForwardNonAA);
        s_dns->applySubnetFilter();
        if (s_dhcp) s_dhcp->setDnsServerRunning(s_dns->isRunning());
    }
    // Diagnostic: shows what the client actually sent for the external cache
    // (auth user only — the password is never logged). Helps confirm the
    // values arrive at the backend before NVS persistence.
    ESP_LOGI(TAG, "DNS settings updated: enabled=%d cache_rest=%d cache_url=%s cache_auth=%d cache_user=%s log_rest=%d log_url=%s log_auth=%d log_user=%s",
             cfg.enabled ? 1 : 0,
             cfg.cacheRest ? 1 : 0,
             cfg.cacheUrl.empty() ? "-" : cfg.cacheUrl.c_str(),
             cfg.cacheAuthEnabled ? 1 : 0,
             cfg.cacheAuthUser.empty() ? "-" : cfg.cacheAuthUser.c_str(),
             cfg.logRest ? 1 : 0,
             cfg.logUrl.empty() ? "-" : cfg.logUrl.c_str(),
             cfg.logAuthEnabled ? 1 : 0,
             cfg.logAuthUser.empty() ? "-" : cfg.logAuthUser.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/dns/local-hosts
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetLocalHosts(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    auto hosts = ::dhcp::core::Config::instance().getLocalHosts();
    std::string json = "{\"hosts\":[";
    for (size_t i = 0; i < hosts.size(); i++) {
        if (i > 0) json += ",";
        json += "{\"name\":\"" + hosts[i].name + "\",";
        json += "\"ip4\":\"" + hosts[i].ip4 + "\",";
        json += "\"ip6\":\"" + hosts[i].ip6 + "\",";
        json += std::string("\"enabled\":") + (hosts[i].enabled ? "true" : "false") + "}";
    }
    json += "]}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/dns/local-hosts
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostLocalHosts(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    // Parse JSON array of hosts: find "name":"..." patterns
    std::vector<::dhcp::core::LocalHostEntry> hosts;
    size_t pos = 0;
    while ((pos = body.find("\"name\"", pos)) != std::string::npos) {
        ::dhcp::core::LocalHostEntry e;
        e.name = jsonGetStr(body.substr(pos), "name");
        e.ip4 = jsonGetStr(body.substr(pos), "ip4");
        e.ip6 = jsonGetStr(body.substr(pos), "ip6");
        e.enabled = jsonGetBool(body.substr(pos), "enabled", true);
        if (!e.name.empty() && (!e.ip4.empty() || !e.ip6.empty())) {
            hosts.push_back(e);
        }
        pos++;
    }

    ::dhcp::core::Config::instance().setLocalHosts(hosts);

    // Apply to the running DNS server immediately
    if (s_dns) {
        s_dns->clearLocalHosts();
        for (const auto& h : hosts) {
            if (!h.enabled) continue;
            if (!h.ip4.empty()) s_dns->addLocalHost(h.name, h.ip4);
            if (!h.ip6.empty()) s_dns->addLocalHost(h.name, h.ip6);
        }
        // Keep the REST logger's local-hosts view in sync (URL host
        // resolution uses the live list, no reboot required).
        s_dns->syncLoggerLocalHosts();
    }
    ESP_LOGI(TAG, "Local hosts updated (%zu entries)", hosts.size());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/security/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetSecuritySettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    auto cfg = ::dhcp::core::Config::instance().getSecurity();
    std::string json = "{";
    addJsonString(json, "username", cfg.username, false);
    addJsonInt(json, "max_attempts", cfg.maxAttempts, true);
    addJsonInt(json, "lockout_period", cfg.lockoutPeriodSec, true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/security/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostSecuritySettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    ::dhcp::core::SecurityConfig cfg;
    cfg.username = jsonGetStr(body, "username");
    if (cfg.username.empty()) cfg.username = "admin";
    cfg.password = jsonGetStr(body, "password");
    if (cfg.password.empty()) {
        // Keep existing password if not provided
        cfg.password = ::dhcp::core::Config::instance().getSecurity().password;
    }
    cfg.maxAttempts = jsonGetInt(body, "max_attempts", 5);
    cfg.lockoutPeriodSec = jsonGetInt(body, "lockout_period", 300);

    ::dhcp::core::Config::instance().setSecurity(cfg);
    // Reload auth config
    if (s_auth) s_auth->reloadConfig();
    ESP_LOGI(TAG, "Security settings updated");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/settings/export
// Full backup of all NVS settings as JSON, including the firmware version.
// Passwords are intentionally NOT exported (log/cache auth and web password) —
// the corresponding *_auth booleans are kept so an import knows auth is on.
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetSettingsExport(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    auto& cfgMgr = ::dhcp::core::Config::instance();
    auto dhcp = cfgMgr.getDhcp();
    auto dns  = cfgMgr.getDns();
    auto sec  = cfgMgr.getSecurity();
    auto bindings = cfgMgr.getStaticBindings();
    auto hosts    = cfgMgr.getLocalHosts();

    std::string json = "{";
    addJsonString(json, "format", "dhcpserver-settings", false);
    addJsonInt(json, "schema", 1, true);
    addJsonString(json, "firmware_version",
                  ::dhcp::core::Version::instance().toString(), true);

    // ── dhcp section (no log_auth_password) ──
    json += ",\"dhcp\":{";
    addJsonBool(json, "enabled", dhcp.enabled, false);
    addJsonString(json, "server_ip", dhcp.serverIp, true);
    addJsonString(json, "start_ip", dhcp.startIp, true);
    addJsonString(json, "end_ip", dhcp.endIp, true);
    addJsonString(json, "subnet", dhcp.subnet, true);
    addJsonString(json, "gateway", dhcp.gateway, true);
    addJsonInt(json, "lease_time", dhcp.leaseTimeSec, true);
    addJsonInt(json, "max_lease_entries",
               static_cast<int64_t>(dhcp.maxLeaseEntries), true);
    addJsonBool(json, "log_terminal", dhcp.logTerminal, true);
    addJsonBool(json, "log_rest", dhcp.logRest, true);
    addJsonString(json, "log_url", dhcp.logUrl, true);
    addJsonBool(json, "log_auth", dhcp.logAuthEnabled, true);
    addJsonString(json, "log_auth_user", dhcp.logAuthUser, true);
    addJsonString(json, "dns_mode", dhcp.dnsMode, true);
    addJsonString(json, "dns_address", dhcp.dnsAddress, true);
    json += "}";

    // ── static_bindings section ──
    json += ",\"static_bindings\":[";
    for (size_t i = 0; i < bindings.size(); i++) {
        if (i > 0) json += ",";
        json += "{\"mac\":\"" + bindings[i].mac + "\",";
        json += "\"ip\":\"" + bindings[i].ip + "\",";
        json += "\"name\":\"" + bindings[i].name + "\",";
        json += "\"gateway\":\"" + bindings[i].gateway + "\",";
        json += std::string("\"use_gateway\":") +
                (bindings[i].useGateway ? "true" : "false") + ",";
        json += std::string("\"enabled\":") +
                (bindings[i].enabled ? "true" : "false") + ",";
        json += std::string("\"use_dns\":") +
                (bindings[i].useDns ? "true" : "false") + "}";
    }
    json += "]";

    // ── dns section (no log_auth_password / cache_auth_password) ──
    json += ",\"dns\":{";
    addJsonBool(json, "enabled", dns.enabled, false);
    addJsonString(json, "external_dns", dns.externalDns, true);
    addJsonBool(json, "log_terminal", dns.logTerminal, true);
    addJsonBool(json, "log_forwarded", dns.logForwarded, true);
    addJsonBool(json, "log_local", dns.logLocal, true);
    addJsonBool(json, "log_cache", dns.logCache, true);
    addJsonBool(json, "log_rest_sent", dns.logRestSent, true);
    addJsonBool(json, "log_rest", dns.logRest, true);
    addJsonString(json, "log_url", dns.logUrl, true);
    addJsonBool(json, "log_auth", dns.logAuthEnabled, true);
    addJsonString(json, "log_auth_user", dns.logAuthUser, true);
    addJsonBool(json, "cache_rest", dns.cacheRest, true);
    addJsonBool(json, "cache_rest_read", dns.cacheRestRead, true);
    addJsonBool(json, "cache_rest_write", dns.cacheRestWrite, true);
    addJsonString(json, "cache_url", dns.cacheUrl, true);
    addJsonBool(json, "cache_auth", dns.cacheAuthEnabled, true);
    addJsonString(json, "cache_auth_user", dns.cacheAuthUser, true);
    addJsonBool(json, "cache_internal", dns.cacheInternal, true);
    addJsonInt(json, "cache_internal_size_mb", dns.cacheInternalSizeMb, true);
    addJsonBool(json, "cache_internal_ignore_ttl", dns.cacheInternalIgnoreTtl, true);
    addJsonBool(json, "block_forward_non_aa", dns.blockForwardNonAA, true);
    addJsonBool(json, "allow_own_subnet", dns.allowOwnSubnet, true);
    json += "}";

    // ── time section (no log_auth_password) ──
    {
        auto tcfg = cfgMgr.getTime();
        json += ",\"time\":{";
        addJsonBool(json, "enabled", tcfg.enabled, false);
        addJsonBool(json, "sync_enabled", tcfg.syncEnabled, true);
        addJsonString(json, "external_ntp", tcfg.externalNtp, true);
        addJsonString(json, "timezone", tcfg.timezone, true);
        addJsonInt(json, "utc_offset_hours", tcfg.utcOffsetHours, true);
        addJsonInt(json, "sync_interval_sec",
                   static_cast<int64_t>(tcfg.syncIntervalSec), true);
        addJsonBool(json, "allow_own_subnet", tcfg.allowOwnSubnet, true);
        addJsonInt(json, "rate_limit_per_sec",
                   static_cast<int64_t>(tcfg.rateLimitPerSec), true);
        addJsonBool(json, "log_terminal", tcfg.logTerminal, true);
        addJsonBool(json, "log_rest", tcfg.logRest, true);
        addJsonString(json, "log_url", tcfg.logUrl, true);
        addJsonBool(json, "log_auth", tcfg.logAuthEnabled, true);
        addJsonString(json, "log_auth_user", tcfg.logAuthUser, true);
        json += "}";
    }

    // ── local_hosts section ──
    json += ",\"local_hosts\":[";
    for (size_t i = 0; i < hosts.size(); i++) {
        if (i > 0) json += ",";
        json += "{\"name\":\"" + hosts[i].name + "\",";
        json += "\"ip4\":\"" + hosts[i].ip4 + "\",";
        json += "\"ip6\":\"" + hosts[i].ip6 + "\",";
        json += std::string("\"enabled\":") +
                (hosts[i].enabled ? "true" : "false") + "}";
    }
    json += "]";

    // ── security section (no password) ──
    json += ",\"security\":{";
    addJsonString(json, "username", sec.username, false);
    addJsonInt(json, "max_attempts", sec.maxAttempts, true);
    addJsonInt(json, "lockout_period", sec.lockoutPeriodSec, true);
    json += "}";

    // ── files section (file explorer access policy) ──
    {
        auto fcfg = cfgMgr.getFiles();
        json += ",\"files\":{";
        addJsonBool(json, "allow_own_subnet", fcfg.allowOwnSubnet, false);
        json += "}";
    }

    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    ESP_LOGI(TAG, "Settings exported (%zu bytes)", json.size());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/settings/import
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostSettingsImport(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req, 16384);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    // ─── 1. Validate format / version marker ───
    const std::string fmt = jsonGetStr(body, "format");
    if (fmt != "dhcpserver-settings") {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"Invalid format marker\"}");
        return ESP_OK;
    }

    // ─── 2. Compare firmware version by release (xxx) ───
    const std::string fileVerStr = jsonGetStr(body, "firmware_version");
    const auto& curVer = ::dhcp::core::Version::instance();
    bool versionMismatch = false;
    bool fileNewer = false;
    if (!fileVerStr.empty()) {
        int g = -1, d = -1, rel = -1;
        if (std::sscanf(fileVerStr.c_str(), "%d.%d.%d", &g, &d, &rel) >= 3) {
            versionMismatch = (rel != curVer.release());
            fileNewer = (rel > curVer.release());
        }
    }

    // ─── 3. Collect unknown top-level keys (cannot be imported) ───
    // We only know these section keys; anything else is reported as skipped.
    std::string skipped;
    const char* known[] = { "format", "schema", "firmware_version",
                            "dhcp", "static_bindings", "dns", "time",
                            "local_hosts", "security", "files" };
    size_t pos = 0;
    while ((pos = body.find('"', pos)) != std::string::npos) {
        size_t keyStart = pos + 1;
        size_t keyEnd = body.find('"', keyStart);
        if (keyEnd == std::string::npos) break;
        std::string key = body.substr(keyStart, keyEnd - keyStart);
        // A key is a candidate object key if followed by ':' (not part of a
        // nested value). Keys at top level are followed by ':' and are not
        // within the known sections (we accept the whole file as flat scan).
        size_t colon = keyEnd + 1;
        while (colon < body.size() && body[colon] == ' ') colon++;
        if (colon < body.size() && body[colon] == ':') {
            bool isKnown = false;
            for (const char* k : known) {
                if (key == k) { isKnown = true; break; }
            }
            if (!isKnown && key != "enabled" && key != "server_ip" &&
                key != "start_ip" && key != "end_ip" && key != "subnet" &&
                key != "gateway" && key != "lease_time" &&
                key != "max_lease_entries" &&
                key != "log_terminal" && key != "log_rest" &&
                key != "log_url" && key != "log_auth" &&
                key != "log_auth_user" && key != "dns_mode" &&
                key != "dns_address" && key != "mac" && key != "ip" &&
                key != "name" && key != "use_gateway" && key != "use_dns" &&
                key != "external_dns" && key != "log_forwarded" &&
                key != "log_local" && key != "log_cache" &&
                key != "log_rest_sent" && key != "cache_rest" &&
                key != "cache_rest_read" && key != "cache_rest_write" &&
                key != "cache_url" && key != "cache_auth" &&
                key != "cache_auth_user" && key != "cache_internal" &&
                key != "cache_internal_size_mb" &&
                key != "cache_internal_ignore_ttl" &&
                key != "block_forward_non_aa" &&
                key != "allow_own_subnet" &&
                key != "external_ntp" && key != "timezone" &&
                key != "sync_enabled" &&
                key != "utc_offset_hours" &&
                key != "sync_interval_sec" &&
                key != "rate_limit_per_sec" &&
                key != "ip4" && key != "ip6" &&
                key != "username" && key != "max_attempts" &&
                key != "lockout_period") {
                if (!skipped.empty()) skipped += ",";
                skipped += "\"" + key + "\"";
            }
        }
        pos = keyEnd + 1;
    }

    // ─── 4. Import sections (recognized fields only; passwords never) ───
    bool importedDhcp = false, importedBind = false, importedDns = false;
    bool importedHosts = false, importedSec = false, importedTime = false;

    auto& cfgMgr = ::dhcp::core::Config::instance();
    const auto oldDhcp = cfgMgr.getDhcp();  // to detect network-level changes

    // DHCP section (object nested under "dhcp")
    {
        size_t s = body.find("\"dhcp\"");
        if (s != std::string::npos) {
            size_t open = body.find('{', s);
            if (open != std::string::npos) {
                std::string seg = body.substr(open);
                auto cur = cfgMgr.getDhcp();
                cur.enabled = jsonGetBool(seg, "enabled", cur.enabled);
                std::string v = jsonGetStr(seg, "server_ip"); if (!v.empty()) cur.serverIp = v;
                v = jsonGetStr(seg, "start_ip"); if (!v.empty()) cur.startIp = v;
                v = jsonGetStr(seg, "end_ip"); if (!v.empty()) cur.endIp = v;
                v = jsonGetStr(seg, "subnet"); if (!v.empty()) cur.subnet = v;
                v = jsonGetStr(seg, "gateway"); if (!v.empty()) cur.gateway = v;
                v = jsonGetStr(seg, "log_url"); cur.logUrl = v;
                v = jsonGetStr(seg, "log_auth_user"); cur.logAuthUser = v;
                v = jsonGetStr(seg, "dns_address"); cur.dnsAddress = v;
                std::string m = jsonGetStr(seg, "dns_mode"); if (m == "manual" || m == "auto") cur.dnsMode = m;
                cur.logTerminal = jsonGetBool(seg, "log_terminal", cur.logTerminal);
                cur.logRest = jsonGetBool(seg, "log_rest", cur.logRest);
                cur.logAuthEnabled = jsonGetBool(seg, "log_auth", cur.logAuthEnabled);
                cur.leaseTimeSec = jsonGetInt(seg, "lease_time", cur.leaseTimeSec);
                {
                    int64_t maxEntries = jsonGetInt(
                        seg, "max_lease_entries",
                        static_cast<int64_t>(cur.maxLeaseEntries));
                    if (maxEntries != 0) {   // 0 = auto (2x pool size)
                        if (maxEntries < 8) maxEntries = 8;
                        if (maxEntries > 512) maxEntries = 512;
                    }
                    cur.maxLeaseEntries = static_cast<uint32_t>(maxEntries);
                }
                cfgMgr.setDhcp(cur);
                importedDhcp = true;
            }
        }
    }

    // Static bindings (array; we rebuild from recognized entries)
    {
        size_t s = body.find("\"static_bindings\"");
        if (s != std::string::npos) {
            size_t open = body.find('[', s);
            if (open != std::string::npos) {
                std::vector<::dhcp::core::StaticBinding> out;
                size_t p = open;
                while ((p = body.find("\"mac\"", p)) != std::string::npos &&
                       p < body.size()) {
                    ::dhcp::core::StaticBinding b;
                    std::string seg = body.substr(p);
                    b.mac = jsonGetStr(seg, "mac");
                    b.ip = jsonGetStr(seg, "ip");
                    b.name = jsonGetStr(seg, "name");
                    b.gateway = jsonGetStr(seg, "gateway");
                    b.useGateway = jsonGetBool(seg, "use_gateway", true);
                    b.enabled = jsonGetBool(seg, "enabled", true);
                    b.useDns = jsonGetBool(seg, "use_dns", true);
                    if (!b.mac.empty() && !b.ip.empty()) out.push_back(b);
                    p++;
                }
                cfgMgr.setStaticBindings(out);
                if (s_dhcp) s_dhcp->reloadStaticBindings();
                importedBind = true;
            }
        }
    }

    // DNS section
    {
        size_t s = body.find("\"dns\"");
        if (s != std::string::npos) {
            size_t open = body.find('{', s);
            if (open != std::string::npos) {
                std::string seg = body.substr(open);
                auto cur = cfgMgr.getDns();
                cur.enabled = jsonGetBool(seg, "enabled", cur.enabled);
                std::string v = jsonGetStr(seg, "external_dns"); if (!v.empty()) cur.externalDns = v;
                v = jsonGetStr(seg, "log_url"); cur.logUrl = v;
                v = jsonGetStr(seg, "log_auth_user"); cur.logAuthUser = v;
                v = jsonGetStr(seg, "cache_url"); cur.cacheUrl = v;
                v = jsonGetStr(seg, "cache_auth_user"); cur.cacheAuthUser = v;
                cur.logTerminal = jsonGetBool(seg, "log_terminal", cur.logTerminal);
                cur.logForwarded = jsonGetBool(seg, "log_forwarded", cur.logForwarded);
                cur.logLocal = jsonGetBool(seg, "log_local", cur.logLocal);
                cur.logCache = jsonGetBool(seg, "log_cache", cur.logCache);
                cur.logRestSent = jsonGetBool(seg, "log_rest_sent", cur.logRestSent);
                cur.logRest = jsonGetBool(seg, "log_rest", cur.logRest);
                cur.logAuthEnabled = jsonGetBool(seg, "log_auth", cur.logAuthEnabled);
                cur.cacheRest = jsonGetBool(seg, "cache_rest", cur.cacheRest);
                cur.cacheRestRead = jsonGetBool(seg, "cache_rest_read", cur.cacheRestRead);
                cur.cacheRestWrite = jsonGetBool(seg, "cache_rest_write", cur.cacheRestWrite);
                cur.cacheAuthEnabled = jsonGetBool(seg, "cache_auth", cur.cacheAuthEnabled);
                cur.cacheInternal = jsonGetBool(seg, "cache_internal", cur.cacheInternal);
                int icSize = jsonGetInt(seg, "cache_internal_size_mb",
                                        static_cast<int>(cur.cacheInternalSizeMb));
                if (icSize < 1) icSize = 1;
                if (icSize > 20) icSize = 20;
                cur.cacheInternalSizeMb = static_cast<uint32_t>(icSize);
                cur.cacheInternalIgnoreTtl =
                    jsonGetBool(seg, "cache_internal_ignore_ttl",
                                cur.cacheInternalIgnoreTtl);
                cur.blockForwardNonAA =
                    jsonGetBool(seg, "block_forward_non_aa",
                                cur.blockForwardNonAA);
                cur.allowOwnSubnet =
                    jsonGetBool(seg, "allow_own_subnet", cur.allowOwnSubnet);
                cfgMgr.setDns(cur);
                importedDns = true;
            }
        }
    }

    // Time (NTP) server section
    {
        size_t s = body.find("\"time\"");
        if (s != std::string::npos) {
            size_t open = body.find('{', s);
            if (open != std::string::npos) {
                std::string seg = body.substr(open);
                auto cur = cfgMgr.getTime();
                cur.enabled = jsonGetBool(seg, "enabled", cur.enabled);
                cur.syncEnabled = jsonGetBool(seg, "sync_enabled", cur.syncEnabled);
                std::string v = jsonGetStr(seg, "external_ntp");
                if (!v.empty()) cur.externalNtp = v;
                v = jsonGetStr(seg, "timezone");
                if (v.size() > 40) v.resize(40);
                cur.timezone = v;
                cur.utcOffsetHours =
                    jsonGetInt(seg, "utc_offset_hours", cur.utcOffsetHours);
                if (cur.utcOffsetHours < -12) cur.utcOffsetHours = -12;
                if (cur.utcOffsetHours > 14) cur.utcOffsetHours = 14;
                int64_t syncSec = jsonGetInt(seg, "sync_interval_sec",
                                             static_cast<int64_t>(cur.syncIntervalSec));
                if (syncSec < 15) syncSec = 15;
                if (syncSec > 7 * 86400) syncSec = 7 * 86400;
                cur.syncIntervalSec = static_cast<uint32_t>(syncSec);
                cur.allowOwnSubnet =
                    jsonGetBool(seg, "allow_own_subnet", cur.allowOwnSubnet);
                {
                    int64_t rate = jsonGetInt(seg, "rate_limit_per_sec",
                                              static_cast<int64_t>(cur.rateLimitPerSec));
                    if (rate < 1) rate = 1;
                    if (rate > 100) rate = 100;
                    cur.rateLimitPerSec = static_cast<uint32_t>(rate);
                }
                cur.logTerminal = jsonGetBool(seg, "log_terminal", cur.logTerminal);
                cur.logRest = jsonGetBool(seg, "log_rest", cur.logRest);
                v = jsonGetStr(seg, "log_url"); cur.logUrl = v;
                v = jsonGetStr(seg, "log_auth_user"); cur.logAuthUser = v;
                cur.logAuthEnabled = jsonGetBool(seg, "log_auth", cur.logAuthEnabled);
                cfgMgr.setTime(cur);
                importedTime = true;
            }
        }
    }

    // Local hosts
    {
        size_t s = body.find("\"local_hosts\"");
        if (s != std::string::npos) {
            size_t open = body.find('[', s);
            if (open != std::string::npos) {
                std::vector<::dhcp::core::LocalHostEntry> out;
                size_t p = open;
                while ((p = body.find("\"name\"", p)) != std::string::npos &&
                       p < body.size()) {
                    ::dhcp::core::LocalHostEntry e;
                    std::string seg = body.substr(p);
                    e.name = jsonGetStr(seg, "name");
                    e.ip4 = jsonGetStr(seg, "ip4");
                    e.ip6 = jsonGetStr(seg, "ip6");
                    e.enabled = jsonGetBool(seg, "enabled", true);
                    if (!e.name.empty() && (!e.ip4.empty() || !e.ip6.empty())) out.push_back(e);
                    p++;
                }
                cfgMgr.setLocalHosts(out);
                if (s_dns) {
                    s_dns->clearLocalHosts();
                    for (const auto& h : out) {
                        if (!h.enabled) continue;
                        if (!h.ip4.empty()) s_dns->addLocalHost(h.name, h.ip4);
                        if (!h.ip6.empty()) s_dns->addLocalHost(h.name, h.ip6);
                    }
                    s_dns->syncLoggerLocalHosts();
                }
                importedHosts = true;
            }
        }
    }

    // Security (username + limits only; password never imported)
    {
        size_t s = body.find("\"security\"");
        if (s != std::string::npos) {
            size_t open = body.find('{', s);
            if (open != std::string::npos) {
                std::string seg = body.substr(open);
                auto cur = cfgMgr.getSecurity();
                std::string v = jsonGetStr(seg, "username"); if (!v.empty()) cur.username = v;
                cur.maxAttempts = jsonGetInt(seg, "max_attempts", cur.maxAttempts);
                cur.lockoutPeriodSec = jsonGetInt(seg, "lockout_period", cur.lockoutPeriodSec);
                cfgMgr.setSecurity(cur);
                if (s_auth) s_auth->reloadConfig();
                importedSec = true;
            }
        }
    }

    // Files (file explorer access policy)
    if (body.find("\"files\"") != std::string::npos) {
        size_t s = body.find("\"files\"");
        size_t open = body.find('{', s);
        if (open != std::string::npos) {
            std::string seg = body.substr(open);
            auto cur = cfgMgr.getFiles();
            cur.allowOwnSubnet =
                jsonGetBool(seg, "allow_own_subnet", cur.allowOwnSubnet);
            cfgMgr.setFiles(cur);
            if (s_files) s_files->applyAccessFilter();
        }
    }

    // ─── 5. Re-apply to running servers (DHCP / DNS restart reads NVS) ───
    // A change to the network parameters requires a reboot — the static IP is
    // applied once at Ethernet init and cannot be re-applied on the fly.
    bool rebootRequired = false;
    {
        auto cur = cfgMgr.getDhcp();
        rebootRequired = (cur.serverIp != oldDhcp.serverIp ||
                          cur.subnet    != oldDhcp.subnet ||
                          cur.gateway   != oldDhcp.gateway);
    }

    // Restart DHCP if it should run; stop if disabled (best effort)
    if (s_dhcp) {
        auto c = cfgMgr.getDhcp();
        if (c.enabled) {
            if (!s_dhcp->isRunning()) s_dhcp->start();
            s_dhcp->setLogTerminal(c.logTerminal);
            s_dhcp->setRestLogging(c.logRest, c.logUrl,
                                   c.logAuthEnabled, c.logAuthUser, c.logAuthPassword);
            s_dhcp->reloadStaticBindings();
        } else if (s_dhcp->isRunning()) {
            s_dhcp->stop();
        }
    }
    if (s_dns) {
        auto c = cfgMgr.getDns();
        if (c.enabled) {
            if (!s_dns->isRunning()) s_dns->start();
        } else if (s_dns->isRunning()) {
            s_dns->stop();
        }
        if (s_dhcp) s_dhcp->setDnsServerRunning(s_dns->isRunning());
    }
    if (s_time) {
        auto c = cfgMgr.getTime();
        s_time->setServerName(c.externalNtp);
        s_time->setSyncIntervalSec(c.syncIntervalSec);
        s_time->setUtcOffsetHours(c.utcOffsetHours);
        s_time->setTimezoneName(c.timezone);
        s_time->logger().setLogTerminal(c.logTerminal);
        s_time->logger().setLogRest(c.logRest);
        s_time->logger().setLogUrl(c.logUrl);
        s_time->logger().setLogAuth(c.logAuthEnabled,
                                    c.logAuthUser, c.logAuthPassword);
        if (c.syncEnabled) {
            if (!s_time->isSyncRunning()) s_time->startSync();
            else s_time->restartSync();
        } else if (s_time->isSyncRunning()) {
            s_time->stopSync();
        }
        if (c.enabled) {
            if (!s_time->isRunning()) s_time->start();
        } else if (s_time->isRunning()) {
            s_time->stop();
        }
    }

    // ─── 6. Build response ───
    std::string json = "{";
    addJsonString(json, "status", "ok", false);
    addJsonString(json, "firmware_version", curVer.toString(), true);
    if (!fileVerStr.empty()) addJsonString(json, "file_version", fileVerStr, true);
    addJsonBool(json, "version_mismatch", versionMismatch, true);
    addJsonBool(json, "file_newer", fileNewer, true);
    addJsonBool(json, "reboot_required", rebootRequired, true);
    json += ",\"imported\":{";
    addJsonBool(json, "dhcp", importedDhcp, false);
    addJsonBool(json, "static_bindings", importedBind, true);
    addJsonBool(json, "dns", importedDns, true);
    addJsonBool(json, "time", importedTime, true);
    addJsonBool(json, "local_hosts", importedHosts, true);
    addJsonBool(json, "security", importedSec, true);
    json += "}";
    if (!skipped.empty()) json += ",\"skipped_fields\":[" + skipped + "]";
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    ESP_LOGI(TAG, "Settings import: ver=%s file=%s mismatch=%d new=%d reboot=%d imported=%d%d%d%d%d%d",
             curVer.toString().c_str(), fileVerStr.c_str(),
             versionMismatch ? 1 : 0, fileNewer ? 1 : 0, rebootRequired ? 1 : 0,
             importedDhcp ? 1 : 0, importedBind ? 1 : 0, importedDns ? 1 : 0,
             importedHosts ? 1 : 0, importedSec ? 1 : 0, importedTime ? 1 : 0);
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/settings/reset — factory reset + reboot
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostSettingsReset(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    if (!::dhcp::core::Config::instance().resetAll()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"NVS erase failed\"}");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Factory reset requested from web UI — rebooting...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"status\":\"ok\",\"message\":\"Settings reset to factory defaults. Rebooting...\",\"reboot\":true}");

    // Give the response time to be sent before reboot
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/device/reboot — reboot without touching settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostDeviceReboot(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    ESP_LOGW(TAG, "Reboot requested from web UI");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"status\":\"ok\",\"message\":\"Device is rebooting...\",\"reboot\":true}");

    // Give the response time to be sent before reboot
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/ota/upload
// ─────────────────────────────────────────────────────
// Accepts two body layouts:
//
//   * `multipart/form-data` with a `firmware` part — what the web UI sends
//     (`new FormData()`); only the part payload is the image, everything else
//     is MIME envelope: `--boundary\r\n` + part headers + `\r\n\r\n`, and the
//     trailing `\r\n--boundary--`.
//   * `application/octet-stream` — the whole body is the image (curl
//     `--data-binary`, scripts).
//
// IMPORTANT: the previous version wrote the *raw* body into the OTA partition
// regardless of the layout, so a multipart upload started with
// `--<boundary>\r\nContent-Disposition: …` instead of the 0xE9 image magic.
// `esp_ota_end()` then rejected the image — and the page still reported
// success, so the update looked like it worked while nothing was installed.
// The failure text (`esp_err_to_name`) is now returned in `detail` as well.

namespace {

/**
 * @brief Buffered writer for `esp_ota_write`.
 *
 * The API accepts arbitrary sizes, so this only batches the HTTP chunks into
 * 4 KB writes (fewer flash calls, and the buffer lives on the handler's stack
 * frame instead of the heap).
 */
struct OtaWriter {
    esp_ota_handle_t handle = 0;
    uint8_t buf[4096];
    size_t len = 0;
    uint64_t written = 0;
    esp_err_t err = ESP_OK;

    /** @brief Write the buffered bytes to the OTA partition. */
    bool flush()
    {
        if (len == 0) return err == ESP_OK;

        err = esp_ota_write(handle, buf, len);
        if (err != ESP_OK) return false;

        written += len;
        len = 0;
        return true;
    }

    /** @brief Feed payload bytes. */
    bool feed(const uint8_t* data, size_t n)
    {
        while (n > 0) {
            const size_t space = sizeof(buf) - len;
            const size_t take = (n < space) ? n : space;
            memcpy(buf + len, data, take);
            len += take;
            data += take;
            n -= take;
            if (len == sizeof(buf) && !flush()) return false;
        }
        return err == ESP_OK;
    }
};

} // namespace

esp_err_t RestApi::handlePostOtaUpload(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    ESP_LOGI(TAG, "OTA update starting...");

    // ── Body layout ────────────────────────────────────
    size_t ctLen = httpd_req_get_hdr_value_len(req, "Content-Type");
    std::string contentType;
    if (ctLen > 0) {
        contentType.resize(ctLen);
        httpd_req_get_hdr_value_str(req, "Content-Type", &contentType[0], ctLen + 1);
    }

    // The payload extraction itself lives in MultipartExtractor (unit-tested on
    // the host): writing the raw multipart body into the OTA partition produced
    // an image without the 0xE9 magic, which esp_ota_end() then rejected.
    const bool multipart = contentType.rfind("multipart/form-data", 0) == 0;
    std::string boundary;
    if (multipart) {
        const size_t b = contentType.find("boundary=");
        if (b == std::string::npos) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"status\":\"error\",\"message\":\"multipart body without boundary\"}");
            return ESP_OK;
        }
        boundary = contentType.substr(b + 9);
        boundary.erase(0, boundary.find_first_not_of(" \t\""));
        const size_t lastOk = boundary.find_last_not_of(" \t\"");
        boundary.erase(lastOk == std::string::npos ? 0 : lastOk + 1);
        const size_t semi = boundary.find(';');
        if (semi != std::string::npos) boundary.erase(semi);
        if (boundary.empty()) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"status\":\"error\",\"message\":\"multipart body without boundary\"}");
            return ESP_OK;
        }
    }

    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"no OTA partition available\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA target partition: %s at 0x%lx (%lu bytes)",
             partition->label, (unsigned long)partition->address,
             (unsigned long)partition->size);

    esp_ota_handle_t otaHandle = 0;
    esp_err_t err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &otaHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        std::string body = "{\"status\":\"error\",\"message\":\"OTA begin failed\",\"detail\":\"";
        body += esp_err_to_name(err);
        body += "\"}";
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body.c_str());
        return ESP_OK;
    }

    OtaWriter writer;
    writer.handle = otaHandle;

    MultipartExtractor extractor(boundary, [&writer](const uint8_t* d, size_t n) {
        return writer.feed(d, n);
    });

    // ── Stream the body ────────────────────────────────
    char buf[1024];
    uint32_t remaining = static_cast<uint32_t>(req->content_len);
    int retries = 0;
    bool badBody = false;

    while (remaining > 0) {
        const uint32_t want = (remaining < sizeof(buf)) ? remaining
                                                        : (uint32_t)sizeof(buf);
        const int got = httpd_req_recv(req, buf, want);
        if (got < 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT && retries++ < 20) continue;
            badBody = true;
            break;
        }
        if (got == 0) {
            badBody = true;
            break;
        }
        remaining -= static_cast<uint32_t>(got);

        const uint8_t* data = reinterpret_cast<const uint8_t*>(buf);
        const size_t n = static_cast<size_t>(got);
        const bool ok = multipart ? extractor.feed(data, n) : writer.feed(data, n);
        if (!ok) {
            badBody = true;
            break;
        }
    }

    // Anything left in the socket (multipart epilogue) is read and dropped so
    // the connection stays clean — the image itself is complete by now.
    while (remaining > 0) {
        const uint32_t want = (remaining < sizeof(buf)) ? remaining
                                                        : (uint32_t)sizeof(buf);
        const int got = httpd_req_recv(req, buf, want);
        if (got <= 0) break;
        remaining -= static_cast<uint32_t>(got);
    }

    // ── Finish ─────────────────────────────────────────
    esp_err_t failErr = ESP_OK;
    bool otaEnded = false;

    if (badBody) {
        failErr = ESP_FAIL;
    } else if (multipart && !extractor.finish()) {
        ESP_LOGE(TAG, "multipart body incomplete (no closing boundary)");
        failErr = ESP_ERR_INVALID_ARG;
    } else if (writer.err != ESP_OK) {
        failErr = writer.err;
    } else if (!writer.flush()) {
        failErr = writer.err;
    } else if ((err = esp_ota_end(otaHandle)) != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        failErr = err;
    } else {
        // From here on the image is written and validated: the handle is gone,
        // so the only remaining step is pointing the bootloader at it.
        otaEnded = true;
        if ((err = esp_ota_set_boot_partition(partition)) != ESP_OK) {
            ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
            failErr = err;
        }
    }

    if (failErr != ESP_OK) {
        // Release the partition only while the session is still open
        // (esp_ota_end() already invalidated the handle).
        if (!otaEnded) esp_ota_abort(otaHandle);

        std::string body = "{\"status\":\"error\",\"message\":\"OTA update failed\",\"detail\":\"";
        body += esp_err_to_name(failErr);
        body += "\",\"received\":";
        body += std::to_string((unsigned long long)writer.written);
        body += "}";
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(failErr));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body.c_str());
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA update successful (%llu bytes) — rebooting",
             (unsigned long long)writer.written);
    std::string body = "{\"status\":\"ok\",\"message\":\"Update successful. Rebooting...\",\"bytes\":";
    body += std::to_string((unsigned long long)writer.written);
    body += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body.c_str());

    // Give the response (and the log) time to get out before the reboot.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/web/file?path=<relative path on SPIFFS>
// ─────────────────────────────────────────────────────
// Uploads ONE web-interface file into SPIFFS. The browser sends the raw file
// bytes as the request body (application/octet-stream) and the destination
// path RELATIVE to the SPIFFS root (/spiffs) as a percent-encoded query
// parameter, e.g.:
//   POST /api/web/file?path=pages%2Fversion.html
// with the file contents as the body. The file is written to /spiffs/<path>.
//
// SPIFFS holds ONLY the web files (device settings live in NVS), so replacing
// an existing file is safe. A brand-new path is also written, but a new page
// only becomes reachable once firmware registers a route for it — the server
// registers explicit routes only (no wildcard matching), so the UI tells the
// user that new files/pages require a firmware update.

esp_err_t RestApi::handlePostWebFile(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    constexpr size_t kMaxFileBytes = 256 * 1024;

    auto sendJsonError = [req](const char* status, const char* msg) {
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json");
        std::string j = "{\"status\":\"error\",\"message\":\"";
        j += msg;
        j += "\"}";
        httpd_resp_sendstr(req, j.c_str());
    };

    // Extract and percent-decode the "path" query parameter. httpd_query_key_value
    // copies the raw (still percent-encoded) value, so decode it here.
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        sendJsonError("400 Bad Request", "Missing query string");
        return ESP_OK;
    }
    char rawPath[80];
    if (httpd_query_key_value(query, "path", rawPath, sizeof(rawPath)) != ESP_OK) {
        sendJsonError("400 Bad Request", "Missing path parameter");
        return ESP_OK;
    }
    std::string relPath;
    {
        const char* p = rawPath;
        while (*p) {
            if (*p == '%') {
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                if (!p[1] || !p[2]) {
                    sendJsonError("400 Bad Request", "Bad path encoding");
                    return ESP_OK;
                }
                int hi = hexVal(p[1]);
                int lo = hexVal(p[2]);
                if (hi < 0 || lo < 0) {
                    sendJsonError("400 Bad Request", "Bad path encoding");
                    return ESP_OK;
                }
                relPath.push_back(static_cast<char>((hi << 4) | lo));
                p += 3;
            } else {
                relPath.push_back(*p);
                ++p;
            }
        }
    }

    // Validate: must be a relative path with no traversal and no exotic chars.
    // Only letters/digits/._- in each segment. Longest real path is ~27 chars
    // (pages/settings_export.html); 64 is a generous but safe cap (SPIFFS object
    // name limit is 32, and the "/spiffs" prefix is stripped by the VFS layer).
    auto validRelChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    };
    bool valid = !relPath.empty() && relPath.size() <= 64 && relPath[0] != '/';
    if (valid) {
        size_t segStart = 0;
        while (segStart <= relPath.size()) {
            size_t slash = relPath.find('/', segStart);
            std::string seg = relPath.substr(
                segStart, slash == std::string::npos ? std::string::npos : slash - segStart);
            if (seg.empty() || seg == "." || seg == "..") {
                valid = false;
                break;
            }
            for (char c : seg) {
                if (!validRelChar(c)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) break;
            if (slash == std::string::npos) break;
            segStart = slash + 1;
        }
    }
    if (!valid) {
        sendJsonError("400 Bad Request", "Invalid path");
        return ESP_OK;
    }

    std::string fullPath = "/spiffs/" + relPath;

    if (req->content_len > kMaxFileBytes) {
        sendJsonError("413 Payload Too Large", "File too large");
        return ESP_OK;
    }

    FILE* f = fopen(fullPath.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "Web file upload: cannot open %s", fullPath.c_str());
        sendJsonError("500 Internal Server Error", "Cannot open file");
        return ESP_OK;
    }

    // Stream the raw body to the file. Reuse the bounded timeout-retry pattern
    // from readBody(): httpd_req_recv can return HTTPD_SOCK_ERR_TIMEOUT between
    // TCP segments of a multi-chunk body.
    char buf[1024];
    size_t remaining = req->content_len;
    size_t written = 0;
    int retries = 0;
    bool ok = true;
    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int got = httpd_req_recv(req, buf, want);
        if (got < 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT && written > 0 && retries++ < 20) {
                continue;
            }
            ok = false;
            break;
        }
        if (got == 0) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, static_cast<size_t>(got), f) != static_cast<size_t>(got)) {
            ok = false;
            break;
        }
        written += static_cast<size_t>(got);
        remaining -= static_cast<size_t>(got);
    }
    fflush(f);
    int closeRes = fclose(f);

    if (!ok || written != req->content_len || closeRes != 0) {
        ESP_LOGE(TAG, "Web file upload failed: %s written=%u of %u", fullPath.c_str(),
                 (unsigned)written, (unsigned)req->content_len);
        sendJsonError("500 Internal Server Error", "Upload failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Web file uploaded: %s (%u bytes)", fullPath.c_str(), (unsigned)written);

    httpd_resp_set_type(req, "application/json");
    std::string j = "{\"status\":\"ok\",\"path\":\"";
    j += relPath;
    j += "\",\"bytes\":";
    j += std::to_string(written);
    j += "}";
    httpd_resp_sendstr(req, j.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/test-connection
// ─────────────────────────────────────────────────────
// Tries to reach an arbitrary URL with the same HTTP client settings used by
// the REST log/cache senders (preemptive Basic auth, no auto-redirect, no
// auth retry loop) so the web UI can validate a configured endpoint before
// relying on it. Body: {"url":"...","auth":true,"user":"...","pass":"..."}
// Response: {"ok":true/false,"http":<code or 0>,"elapsed_ms":<ms>,"error":"..."}
// The request is a GET (harmless — no record is written by a GET).
//
// IMPORTANT: the actual HTTP round-trip runs in a DEDICATED task with a large
// stack, NOT in the httpd task. esp_http_client + TLS needs >4 KB of stack,
// while the httpd task only has CONFIG_HTTPD_STACK_SIZE (default 4096) —
// doing the request inline overflowed httpd's stack and rebooted the device.
// The handler starts the task, waits on a semaphore, and replies with the
// result once the task signals completion.

namespace {
struct TestConnCtx {
    std::string url;
    bool useAuth;
    std::string user;
    std::string pass;
    std::string method;   // "GET" (default) or "POST" — must match what the
                          // real sender does so the request actually hits the
                          // auth-protected route (GET on a POST-only base URL
                          // returns 405/404 BEFORE auth is checked).
    std::string body;     // optional POST body
    int http = 0;
    int64_t elapsedMs = 0;
    esp_err_t err = ESP_FAIL;
    SemaphoreHandle_t done = nullptr;
};

void testConnectionTask(void* arg)
{
    auto* ctx = static_cast<TestConnCtx*>(arg);

    // A few seconds at most, but it is exactly the kind of operation the
    // scheduler page exists for: something is happening, and it can be seen.
    auto& jobs = ::dhcp::core::JobRegistry::instance();
    jobs.begin("test_connection", "jobs.test_connection", ctx->url);

    // The request itself is one call and cannot be interrupted in the middle,
    // but a stop that arrived before it started is honoured here.
    if (jobs.cancelRequested("test_connection")) {
        jobs.finish("test_connection", ::dhcp::core::JobState::Cancelled, ctx->url);
        xSemaphoreGive(ctx->done);
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = ctx->url.c_str();
    cfg.method = (ctx->method == "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = 5000;
    cfg.buffer_size = 512;
    cfg.buffer_size_tx = 512;
    cfg.disable_auto_redirect = true;
    cfg.max_authorization_retries = -1;
    if (ctx->useAuth && !ctx->user.empty()) {
        cfg.username = ctx->user.c_str();
        cfg.password = ctx->pass.c_str();
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ctx->err = ESP_ERR_HTTP_CONNECT;
        jobs.finish("test_connection", ::dhcp::core::JobState::Failed, "client init");
        xSemaphoreGive(ctx->done);
        vTaskDelete(nullptr);
        return;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    if (cfg.method == HTTP_METHOD_POST) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        const std::string& p = ctx->body.empty() ? std::string("{}") : ctx->body;
        esp_http_client_set_post_field(client, p.c_str(),
                                       static_cast<int>(p.size()));
    }

    const int64_t t0 = esp_timer_get_time();
    ctx->err = esp_http_client_perform(client);
    ctx->elapsedMs = (esp_timer_get_time() - t0) / 1000;

    if (ctx->err == ESP_OK) {
        ctx->http = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);

    jobs.finish("test_connection",
                ctx->err == ESP_OK ? ::dhcp::core::JobState::Done
                                   : ::dhcp::core::JobState::Failed,
                ctx->url);

    xSemaphoreGive(ctx->done);
    vTaskDelete(nullptr);
}
} // namespace

esp_err_t RestApi::handlePostTestConnection(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    // The MCU reads the ACTUAL persisted (NVS) settings itself — the browser
    // must NOT pass url/user/pass (a stale snapshot in the page caused a false
    // "OK"). target selects which NVS config to test:
    //   "dns_log"   -> DnsConfig.logUrl + logAuth*   (POST {} on a protected route)
    //   "dns_cache" -> DnsConfig.cacheUrl + cacheAuth* (GET {url}/probe)
    //   "dhcp_log"  -> DhcpConfig.logUrl + logAuth*  (POST {} on a protected route)
    const std::string target = jsonGetStr(body, "target");
    if (target != "dns_log" && target != "dns_cache" && target != "dhcp_log") {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"http\":0,\"elapsed_ms\":0,\"error\":\"bad target\"}");
        return ESP_OK;
    }

    TestConnCtx ctx;
    if (target == "dns_log" || target == "dns_cache") {
        const auto dns = ::dhcp::core::Config::instance().getDns();
        if (target == "dns_log") {
            ctx.url = dns.logUrl;
            ctx.useAuth = dns.logAuthEnabled;
            ctx.user = dns.logAuthUser;
            ctx.pass = dns.logAuthPassword;
            ctx.method = "POST";
            ctx.body = "{}";
        } else {
            ctx.url = dns.cacheUrl;
            ctx.useAuth = dns.cacheAuthEnabled;
            ctx.user = dns.cacheAuthUser;
            ctx.pass = dns.cacheAuthPassword;
            ctx.method = "GET";
            // Probe a made-up domain so the request reaches the protected
            // resource route (401 if auth wrong, 404 if ok & not cached).
            while (ctx.url.size() > 1 && ctx.url.back() == '/') ctx.url.pop_back();
            ctx.url += "/probe";
        }
    } else { // dhcp_log
        const auto dhcp = ::dhcp::core::Config::instance().getDhcp();
        ctx.url = dhcp.logUrl;
        ctx.useAuth = dhcp.logAuthEnabled;
        ctx.user = dhcp.logAuthUser;
        ctx.pass = dhcp.logAuthPassword;
        ctx.method = "POST";
        ctx.body = "{}";
    }

    if (ctx.url.empty()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"http\":0,\"elapsed_ms\":0,\"error\":\"empty url\"}");
        return ESP_OK;
    }

    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"http\":0,\"elapsed_ms\":0,\"error\":\"no semaphore\"}");
        return ESP_OK;
    }

    // Dedicated task with a stack large enough for TLS (8192, like the REST
    // senders). 7 s wait covers the 5 s client timeout plus scheduling slack.
    BaseType_t created = xTaskCreate(&testConnectionTask, "tst_conn", 8192,
                                     &ctx, 5, nullptr);
    if (created != pdPASS) {
        vSemaphoreDelete(ctx.done);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"http\":0,\"elapsed_ms\":0,\"error\":\"task create failed\"}");
        return ESP_OK;
    }
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(7000));
    vSemaphoreDelete(ctx.done);

    // ok only when the round-trip succeeded AND the server did not reject
    // the credentials (401/403 = auth problem, not a healthy endpoint).
    const bool ok = (ctx.err == ESP_OK) && ctx.http != 401 && ctx.http != 403;

    std::string json = "{\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"http\":" + std::to_string(ctx.http);
    json += ",\"elapsed_ms\":" + std::to_string(static_cast<long long>(ctx.elapsedMs));
    json += ",\"error\":\"";
    if (ctx.err != ESP_OK) {
        json += esp_err_to_name(ctx.err);
    } else if (ctx.http == 401 || ctx.http == 403) {
        json += (ctx.http == 401) ? "Unauthorized" : "Forbidden";
    }
    json += "\"}";

    ESP_LOGI(TAG, "Test connection: target=%s method=%s url=%s http=%d err=%s (%lld ms)",
             target.c_str(),
             ctx.method.empty() ? "GET" : ctx.method.c_str(),
             ctx.url.c_str(), ctx.http, esp_err_to_name(ctx.err),
             static_cast<long long>(ctx.elapsedMs));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/dns/internal-cache/file
// ─────────────────────────────────────────────────────
// Info about the built-in (PSRAM) DNS cache persistence file (cache.dat on
// FAT). Response: {exists, path, size_bytes, entries, version}. The file only
// exists after a "save" — until then exists=false.

esp_err_t RestApi::handleGetInternalCacheFile(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    ::dhcp::dns::InternalDnsCache::FileInfo info;
    if (s_dns) info = s_dns->internalCacheFileInfo();

    std::string json = "{";
    addJsonBool(json, "exists", info.exists, false);
    addJsonString(json, "path",
                  s_dns ? s_dns->kCacheDatPath : "/fat/cache.dat", true);
    addJsonInt(json, "size_bytes", static_cast<int64_t>(info.size), true);
    addJsonInt(json, "entries", static_cast<int64_t>(info.entries), true);
    addJsonInt(json, "version", static_cast<int64_t>(info.version), true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/dns/internal-cache/progress
// ─────────────────────────────────────────────────────
// Progress of the background save/load job. Response: {busy, save, done,
// total, percent}. busy=false means no job is running (the last finished
// job's done/total are retained so the UI can report "finished at N").

esp_err_t RestApi::handleGetInternalCacheProgress(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    ::dhcp::dns::DnsServer::PersistProgress p;
    if (s_dns) p = s_dns->persistProgress();

    const uint32_t percent = (p.total > 0)
                                 ? static_cast<uint32_t>(
                                       (static_cast<uint64_t>(p.done) * 100ULL) /
                                       p.total)
                                 : 0;

    std::string json = "{";
    addJsonBool(json, "busy", p.busy, false);
    addJsonBool(json, "save", p.isSave, true);
    addJsonInt(json, "done", static_cast<int64_t>(p.done), true);
    addJsonInt(json, "total", static_cast<int64_t>(p.total), true);
    addJsonInt(json, "percent", static_cast<int64_t>(percent), true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/dns/internal-cache/save
// ─────────────────────────────────────────────────────
// Starts a BACKGROUND save of the built-in (PSRAM) DNS cache to
// /fat/cache.dat. The handler returns immediately {status:started}; the UI
// polls GET .../progress until busy=false. Fails with 409 when another job is
// running or the cache is disabled, and with 500 when the job could not start.

esp_err_t RestApi::handlePostInternalCacheSave(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    if (!s_dns) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"DNS server unavailable\"}");
        return ESP_OK;
    }
    if (!s_dns->internalCache().available()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Internal cache is disabled\"}");
        return ESP_OK;
    }
    if (s_dns->persistProgress().busy) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Operation already running\"}");
        return ESP_OK;
    }

    if (!s_dns->startPersistJob(true)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Could not start save\"}");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"status\":\"started\",\"op\":\"save\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/dns/internal-cache/load
// ─────────────────────────────────────────────────────
// Starts a BACKGROUND load of the built-in (PSRAM) DNS cache from
// /fat/cache.dat. The handler returns immediately {status:started}; the UI
// polls GET .../progress until busy=false. Fails with 409 when another job is
// running or the cache is disabled, and with 404 when there is no file.

esp_err_t RestApi::handlePostInternalCacheLoad(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    if (!s_dns) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"DNS server unavailable\"}");
        return ESP_OK;
    }
    if (!s_dns->internalCache().available()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Internal cache is disabled\"}");
        return ESP_OK;
    }
    if (!s_dns->internalCacheFileInfo().exists) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"No cache file\"}");
        return ESP_OK;
    }
    if (s_dns->persistProgress().busy) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Operation already running\"}");
        return ESP_OK;
    }

    if (!s_dns->startPersistJob(false)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Could not start load\"}");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"status\":\"started\",\"op\":\"load\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/time/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetTimeSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    auto cfg = ::dhcp::core::Config::instance().getTime();
    std::string json = "{";
    addJsonBool(json, "enabled", cfg.enabled, false);
    addJsonBool(json, "sync_enabled", cfg.syncEnabled, true);
    addJsonString(json, "server_state",
                  s_time ? s_time->stateString() : "unknown", true);
    addJsonBool(json, "synced", s_time ? s_time->isSynced() : false, true);
    addJsonString(json, "now_utc", s_time ? s_time->nowUtcString() : "", true);
    addJsonString(json, "now_local", s_time ? s_time->nowLocalString() : "", true);
    addJsonInt(json, "uptime_sec",
               s_time ? static_cast<int64_t>(s_time->uptimeSec()) : 0, true);
    addJsonString(json, "external_ntp", cfg.externalNtp, true);
    addJsonString(json, "timezone", cfg.timezone, true);
    addJsonInt(json, "utc_offset_hours", cfg.utcOffsetHours, true);
    addJsonInt(json, "sync_interval_sec",
               static_cast<int64_t>(cfg.syncIntervalSec), true);
    addJsonBool(json, "allow_own_subnet", cfg.allowOwnSubnet, true);
    addJsonInt(json, "rate_limit_per_sec",
               static_cast<int64_t>(cfg.rateLimitPerSec), true);
    // Build constant: the lowest date/time the web page accepts for a manual
    // clock setting (kept at the build date by the version scripts).
    addJsonString(json, "min_datetime",
                  ::dhcp::core::Version::instance().minDateTime(), true);
    addJsonBool(json, "log_terminal", cfg.logTerminal, true);
    addJsonBool(json, "log_rest", cfg.logRest, true);
    addJsonString(json, "log_url", cfg.logUrl, true);
    addJsonBool(json, "log_auth", cfg.logAuthEnabled, true);
    addJsonString(json, "log_auth_user", cfg.logAuthUser, true);
    addJsonString(json, "log_auth_password", cfg.logAuthPassword, true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/time/settings
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostTimeSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    ::dhcp::core::TimeConfig cfg;
    cfg.enabled = jsonGetBool(body, "enabled", false);
    cfg.syncEnabled = jsonGetBool(body, "sync_enabled", true);
    cfg.externalNtp = jsonGetStr(body, "external_ntp");
    if (cfg.externalNtp.empty()) cfg.externalNtp = "pool.ntp.org";
    // Timezone id (display only). Keep a safe charset and a bounded length;
    // an empty value means "custom offset".
    cfg.timezone = jsonGetStr(body, "timezone");
    if (cfg.timezone.size() > 40) cfg.timezone.resize(40);
    {
        std::string clean;
        for (char ch : cfg.timezone) {
            const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                            (ch >= '0' && ch <= '9') ||
                            ch == '/' || ch == '_' || ch == '+' || ch == '-' ||
                            ch == ' ';
            if (ok) clean += ch;
        }
        cfg.timezone = clean;
    }
    cfg.utcOffsetHours = jsonGetInt(body, "utc_offset_hours", 3);
    if (cfg.utcOffsetHours < -12) cfg.utcOffsetHours = -12;
    if (cfg.utcOffsetHours > 14) cfg.utcOffsetHours = 14;
    int64_t syncSec = jsonGetInt(body, "sync_interval_sec", 86400);
    if (syncSec < 15) syncSec = 15;              // RFC 4330 minimum
    if (syncSec > 7 * 86400) syncSec = 7 * 86400;
    cfg.syncIntervalSec = static_cast<uint32_t>(syncSec);
    cfg.allowOwnSubnet = jsonGetBool(body, "allow_own_subnet", true);
    {
        int64_t rate = jsonGetInt(body, "rate_limit_per_sec", 5);
        if (rate < 1) rate = 1;
        if (rate > 100) rate = 100;
        cfg.rateLimitPerSec = static_cast<uint32_t>(rate);
    }
    cfg.logTerminal = jsonGetBool(body, "log_terminal", false);
    cfg.logRest = jsonGetBool(body, "log_rest", false);
    cfg.logUrl = jsonGetStr(body, "log_url");
    cfg.logAuthEnabled = jsonGetBool(body, "log_auth", false);
    cfg.logAuthUser = jsonGetStr(body, "log_auth_user");
    cfg.logAuthPassword = jsonGetStr(body, "log_auth_password");

    ::dhcp::core::Config::instance().setTime(cfg);

    // Apply to the running server (start/stop on enable change, update the
    // SNTP server/interval and the logger settings live).
    if (s_time) {
        s_time->setServerName(cfg.externalNtp);
        s_time->setSyncIntervalSec(cfg.syncIntervalSec);
        s_time->setUtcOffsetHours(cfg.utcOffsetHours);
        s_time->setTimezoneName(cfg.timezone);
        // Own-subnet filter + per-client rate limit (subnet comes from DHCP).
        s_time->applyAccessFilter();
        s_time->logger().setLogTerminal(cfg.logTerminal);
        s_time->logger().setLogRest(cfg.logRest);
        s_time->logger().setLogUrl(cfg.logUrl);
        s_time->logger().setLogAuth(cfg.logAuthEnabled,
                                    cfg.logAuthUser, cfg.logAuthPassword);

        // Clock sync (SNTP client) — independent of serving time.
        if (cfg.syncEnabled) {
            if (!s_time->isSyncRunning()) {
                s_time->startSync();
            } else {
                // Re-apply external server / interval changes.
                s_time->restartSync();
            }
        } else if (s_time->isSyncRunning()) {
            s_time->stopSync();
        }

        // NTP server (serving LAN clients).
        if (cfg.enabled && !s_time->isRunning()) {
            if (s_time->start()) {
                ESP_LOGI(TAG, "NTP server started via API");
            } else {
                ESP_LOGE(TAG, "NTP server failed to start via API");
            }
        } else if (!cfg.enabled && s_time->isRunning()) {
            s_time->stop();
            ESP_LOGI(TAG, "NTP server stopped via API");
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/time/now
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetTimeNow(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string json = "{";
    addJsonString(json, "now_utc",
                  s_time ? s_time->nowUtcString() : "", false);
    addJsonString(json, "now_local",
                  s_time ? s_time->nowLocalString() : "", true);
    addJsonInt(json, "unix_sec",
               s_time ? static_cast<int64_t>(s_time->nowUtcSec()) : 0, true);
    addJsonBool(json, "synced", s_time ? s_time->isSynced() : false, true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/time/set
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostTimeSet(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    auto sendError = [&req](const char* status, const char* message) {
        std::string json = "{\"status\":\"error\",\"message\":\"";
        json += message;
        json += "\"}";
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json.c_str());
        return ESP_OK;
    };

    if (!s_time) return sendError("500 Internal Server Error", "time service unavailable");

    // The operator types LOCAL time; the offset is taken from the request (the
    // page sends it, since the zone may not have been saved yet) and falls back
    // to the configured offset.
    const std::string datetime = jsonGetStr(body, "datetime");
    ::dhcp::time::DateTime dt;
    if (!::dhcp::time::TimeMath::parseDateTime(datetime, dt)) {
        return sendError("400 Bad Request",
                         "invalid datetime (expected YYYY-MM-DD HH:MM:SS)");
    }

    int offsetHours = static_cast<int>(
        jsonGetInt(body, "utc_offset_hours", s_time->utcOffsetHours()));
    if (offsetHours < -12) offsetHours = -12;
    if (offsetHours > 14) offsetHours = 14;

    // Local → UTC (the offset is positive east of Greenwich).
    const int64_t localSec = static_cast<int64_t>(::dhcp::time::TimeMath::toUnixSec(dt));
    const int64_t utcSec = localSec - static_cast<int64_t>(offsetHours) * 3600;
    if (utcSec <= 0) {
        return sendError("400 Bad Request",
                         "datetime out of range (before 1970-01-01 UTC)");
    }

    if (!s_time->setUtcTime(static_cast<uint32_t>(utcSec))) {
        return sendError("500 Internal Server Error", "failed to set the clock");
    }

    std::string json = "{\"status\":\"ok\"";
    addJsonInt(json, "unix_sec", static_cast<int64_t>(s_time->nowUtcSec()), true);
    addJsonString(json, "now_utc", s_time->nowUtcString(), true);
    addJsonString(json, "now_local", s_time->nowLocalString(), true);
    addJsonBool(json, "synced", s_time->isSynced(), true);
    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/files/volumes
// ─────────────────────────────────────────────────────
// State of every file-explorer volume: id, mount point, mounted/present flags,
// capacity and the last mount error. The response is also what the web UI
// polls to discover a card that was inserted after boot (FileManager retries
// the mount, throttled, on every call).
//
//   {"enabled":true,"volumes":[
//     {"id":"fat","mount_point":"/fat","mounted":true,"present":true,
//      "total_bytes":22282240,"free_bytes":22118400,"error":""}, … ]}

esp_err_t RestApi::handleGetFileVolumes(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;

    // The body itself is built by `FileJson` (host-tested): the previous
    // hand-written version started with a stray comma (`{,"enabled"…`) and
    // the page failed in JSON.parse() although the status was 200.
    const std::string json = FileJson::volumes(
        s_files ? s_files->supported() : false,
        s_files ? s_files->volumes() : std::vector<::dhcp::storage::VolumeInfo>{});

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// File explorer helpers
// ─────────────────────────────────────────────────────
namespace {

/** @brief Complete HTTP status line for a file-explorer status. */
const char* fileStatusLine(::dhcp::files::FileStatus st)
{
    switch (::dhcp::files::httpStatusFor(st)) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 415: return "415 Unsupported Media Type";
        case 507: return "507 Insufficient Storage";
        default:  return "500 Internal Server Error";
    }
}

/**
 * @brief Send the result of a file-explorer operation.
 *
 * Success carries @p okJson (a ready-made JSON object), failures carry
 * `{"status":"error","message":…[,"detail":…]}` with the status code mapped
 * from the enum — handlers never invent status codes or messages.
 */
esp_err_t sendFileResult(httpd_req* req, ::dhcp::files::FileStatus st,
                         const std::string& okJson,
                         const std::string* detail = nullptr)
{
    if (st == ::dhcp::files::FileStatus::Ok) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, okJson.c_str());
        return ESP_OK;
    }

    JsonWriter fail;
    fail.str("status", "error");
    fail.str("message", ::dhcp::files::messageFor(st));
    if (detail != nullptr && !detail->empty()) {
        fail.str("detail", *detail);
    }

    httpd_resp_set_status(req, fileStatusLine(st));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, fail.toString().c_str());
    return ESP_OK;
}

/** @brief Reply for a missing/unknown file-explorer backend. */
esp_err_t sendNoFileManager(httpd_req* req)
{
    return sendFileResult(req, ::dhcp::files::FileStatus::NotMounted,
                          "{}", nullptr);
}

/**
 * @brief Read and percent-decode one query parameter.
 *
 * `httpd_query_key_value()` copies the raw (still percent-encoded) value; paths
 * with non-ASCII names would reach the validator percent-encoded otherwise.
 * The 1 KB buffer covers a 255-byte path with every byte escaped.
 *
 * @return false when the parameter is missing or the escape sequence is broken.
 */
bool queryParam(httpd_req* req, const char* key, std::string& out)
{
    char query[1024];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    char raw[768];
    if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    out.clear();
    for (const char* p = raw; *p; ++p) {
        if (*p != '%') {
            out += (*p == '+') ? ' ' : *p;
            continue;
        }
        if (!p[1] || !p[2]) return false;
        const int hi = hexVal(p[1]);
        const int lo = hexVal(p[2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>((hi << 4) | lo);
        p += 2;
    }
    return true;
}

} // namespace (file explorer helpers)

// ─────────────────────────────────────────────────────
// Long transfers outside the server task
// ─────────────────────────────────────────────────────
// ESP-IDF's httpd runs a handler **in the server task**: while a handler works,
// the server accepts nothing else. A transfer that lasts — a big file in, a big
// file out — therefore stops every other client for its whole duration: in a
// second browser the pages load slowly and the /api/status polling freezes (the
// home page bars stop moving). It is not a bandwidth problem but a scheduling
// one, and it is the same reason the format moved out of the server task.
//
// `httpd_req_async_handler_begin()` hands the client's socket to the caller, and
// the work continues in a task of its own: the server stays free for everyone
// else and the answer still goes out on the same connection when the work is
// done. The machinery is defined further down, next to the transfers that use it
// (and next to the format, which shares it); declared here because the file
// handlers come first in this file.

namespace {

/**
 * @brief A request that is answered from a task of its own.
 *
 * Derive from this and keep it **first** in the work item, so a task can treat
 * its argument as both. @ref finish is the only way to close such a request —
 * after it, neither the async handle nor the original one may be used.
 */
struct AsyncRequest {
    httpd_req_t* req = nullptr;   ///< Async copy — owns the client's socket

    virtual ~AsyncRequest() = default;

    /** @brief Give the socket back to the server (the answer is already sent). */
    void finish() { httpd_req_async_handler_complete(req); }
};

/**
 * @brief Answer a request with an error, on whichever handle is still valid.
 *
 * @param[in] req      Request to answer (the async one once it exists).
 * @param[in] message  Ready-made JSON body.
 */
void answerError(httpd_req* req, const char* message);

/**
 * @brief Take @p req off the server task and run @p body with @p work.
 *
 * On any failure the request is answered with @p failMessage (the connection is
 * still intact as long as the async copy has not been handed over) and @p work
 * is deleted.
 *
 * @return true when the task was started (the answer is then its job).
 */
bool runDetached(httpd_req* req, AsyncRequest* work, TaskFunction_t body,
                 const char* taskName, const char* failMessage);

} // namespace

// ─────────────────────────────────────────────────────
// GET /api/files/list?volume=<id>&path=<rel>
// ─────────────────────────────────────────────────────
// Directory listing: directories first, then files, case-insensitive by name.
// The response is bounded (max. IFileManager::kMaxListEntries entries) so a
// huge directory cannot exhaust the httpd stack while building the body;
// `truncated` says the list may have been cut.
//
//   {"volume":"fat","path":"/logs","mounted":true,"total_bytes":…,
//    "free_bytes":…,"truncated":false,
//    "entries":[{"name":"2026","is_dir":true,"size":0,"mtime":1757971200}, …]}

esp_err_t RestApi::handleGetFileList(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    std::string volume, path;
    if (!queryParam(req, "volume", volume)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotFound, "{}",
                              nullptr);
    }
    if (!queryParam(req, "path", path)) path = "/";   // root by default

    std::vector<::dhcp::files::FileEntry> entries;
    std::string detail;
    ::dhcp::files::FileStatus st =
        s_files->list(volume, path, entries, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        return sendFileResult(req, st, "{}", &detail);
    }

    // Normalized path + capacity for the breadcrumb/free-space display.
    std::string norm = path;
    if (!::dhcp::storage::PathUtil::normalize(path, norm)) norm = "/";
    ::dhcp::storage::IFileSystem* vol = s_files->find(volume);
    uint64_t total = 0, free = 0;
    if (vol != nullptr) {
        const auto info = vol->info();
        total = info.totalBytes;
        free = info.freeBytes;
    }

    FileJson::ListPayload payload;
    payload.volume = volume;
    payload.path = norm;
    payload.mounted = true;
    payload.totalBytes = total;
    payload.freeBytes = free;
    payload.truncated =
        entries.size() >= ::dhcp::files::IFileManager::kMaxListEntries;
    payload.entries = entries;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, FileJson::list(payload).c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/files/mkdir  |  /rename  |  /delete  |  /format
// ─────────────────────────────────────────────────────
// Bodies (all volume-relative paths, leading slash optional):
//   mkdir : {"volume":"fat","path":"/logs/2026"}
//   rename: {"volume":"fat","path":"/a.txt","to":"/b.txt"}
//   delete: {"volume":"fat","path":"/logs","recursive":true}
//   format: {"volume":"sd","confirm":true}
//
// `recursive` is required to delete a non-empty directory (otherwise 409), and
// `confirm` must be exactly true to format — formatting erases the whole card
// and is only offered for the external volume.

esp_err_t RestApi::handlePostFileMkdir(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    const std::string body = readBody(req);
    const std::string volume = jsonGetStr(body, "volume");
    const std::string path = jsonGetStr(body, "path");

    std::string detail;
    const auto st = s_files->mkdir(volume, path, &detail);
    return sendFileResult(req, st, "{\"status\":\"ok\"}", &detail);
}

esp_err_t RestApi::handlePostFileRename(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    const std::string body = readBody(req);
    const std::string volume = jsonGetStr(body, "volume");
    const std::string from = jsonGetStr(body, "path");
    const std::string to = jsonGetStr(body, "to");

    std::string detail;
    const auto st = s_files->rename(volume, from, to, &detail);
    return sendFileResult(req, st, "{\"status\":\"ok\"}", &detail);
}

esp_err_t RestApi::handlePostFileDelete(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    const std::string body = readBody(req);
    const std::string volume = jsonGetStr(body, "volume");
    const std::string path = jsonGetStr(body, "path");
    const bool recursive = jsonGetBool(body, "recursive", false);

    std::string detail;
    const auto st = s_files->remove(volume, path, recursive, &detail);
    return sendFileResult(req, st, "{\"status\":\"ok\"}", &detail);
}

// ─────────────────────────────────────────────────────
// POST /api/files/format  {"volume":"sd","confirm":true}
// ─────────────────────────────────────────────────────
// The format runs in a task of its own rather than in the server's — the same
// machinery the transfers use (see "Long transfers outside the server task").
// Erasing and re-creating the filesystem of a failing card can take a long
// time, and the httpd serves one request at a time: a synchronous format would
// freeze the whole web interface *and* put its own stop button out of reach,
// because POST /api/jobs/cancel could not be answered before it returned.

namespace {

/** @brief Work item of an asynchronous format. */
struct FormatWork : AsyncRequest {
    ::dhcp::files::IFileManager* files = nullptr;
    std::string volume;
};

/**
 * @brief True while the running format has been stopped from the scheduler.
 *
 * The erase is one call into IDF and FatFS, and there is no way to cut it short
 * from another task — it ends when the driver stops answering (a failing card is
 * exactly the case that takes long). What *can* end at once is the **operation**:
 * the scheduler row leaves the list and the record is finished as cancelled the
 * moment the request arrives, instead of the operator waiting for the card. The
 * format task then knows, when its call finally returns, that it must not report
 * a success — the volume it has just re-created is not what anybody is waiting
 * for any more. It still answers the client that asked for the format, which is
 * the only thing that has to wait for the driver.
 *
 * A flag is enough because only one format can be running (the registry refuses
 * a second one, and `FileManager::formatting_` keeps every file operation off
 * the volume meanwhile).
 */
std::atomic<bool> g_formatAbandoned{false};

/**
 * @brief True once the destructive call of the running format has returned.
 *
 * The escape hatch below waits a moment before it cuts the card's supply, and
 * this is what it asks to find out whether there is anything left to break: a
 * format on a healthy card is over in a moment, and cutting the power of a card
 * nobody is writing to would only leave the volume unmounted for five seconds.
 */
std::atomic<bool> g_formatCallDone{false};

/** @brief Grace before the supply is cut (see @ref powerCutTask). */
constexpr uint32_t kPowerCutGraceMs = 1000;

/** @brief How long the card stays unpowered to break a stuck call. */
constexpr uint32_t kPowerCutMs = 5000;

/** @brief Work item of the power cut that breaks a stopped format. */
struct PowerCutWork {
    ::dhcp::files::IFileManager* files = nullptr;
    std::string volume;
};

/**
 * @brief Body of the escape hatch: wait a moment, then take the card's supply.
 *
 * The erase is one call into IDF and FatFS, so nothing in the firmware can
 * interrupt it — the hardware can: with the supply gone the transfer in flight
 * fails, FatFS aborts its write and the call returns with an error. It runs in
 * a task of its own because the cancel request has to be answered at once (the
 * scheduler row leaves the list immediately), while the cut and the five-second
 * wait go on in the background; the volume comes back unmounted, and the next
 * mount attempt (every five seconds) finds the card again — a failing one with
 * a working retry path instead of a format nobody can stop.
 */
void powerCutTask(void* arg)
{
    auto* work = static_cast<PowerCutWork*>(arg);

    vTaskDelay(pdMS_TO_TICKS(kPowerCutGraceMs));
    if (g_formatCallDone.load()) {
        ESP_LOGI(TAG, "the stopped format of '%s' had already returned — the "
                      "card supply is left alone", work->volume.c_str());
    } else {
        std::string detail;
        const auto st = work->files->powerCycle(work->volume, kPowerCutMs, &detail);
        if (st == ::dhcp::files::FileStatus::Ok) {
            ESP_LOGW(TAG, "card supply cut for %u ms to break the format of '%s'",
                     (unsigned)kPowerCutMs, work->volume.c_str());
        } else {
            ESP_LOGW(TAG, "cannot cut the supply of '%s' (%s): the format runs "
                          "until the driver gives up", work->volume.c_str(),
                     detail.c_str());
        }
    }

    delete work;
    vTaskDelete(nullptr);
}

/** @brief Start the escape hatch for a format that was stopped. */
void startPowerCut(::dhcp::files::IFileManager* files, const std::string& volume)
{
    auto* work = new PowerCutWork{files, volume};
    if (xTaskCreate(powerCutTask, "sd_power_cut", 3072, work,
                    tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        delete work;
        ESP_LOGW(TAG, "cannot start the card power cut task");
    }
}

/** @brief Answer the format request on its async handle and release the socket. */
void formatAnswer(httpd_req_t* req, bool cancelled, ::dhcp::files::FileStatus st,
                  const std::string& detail)
{
    if (cancelled) {
        answerError(req,
            "{\"status\":\"error\",\"message\":\"format cancelled\","
            "\"detail\":\"stopped from the task scheduler\"}");
    } else {
        sendFileResult(req, st, "{\"status\":\"ok\"}", &detail);
    }
}

/** @brief Body of an asynchronous format: the work, the record, the answer. */
void formatTask(void* arg)
{
    auto* work = static_cast<FormatWork*>(arg);
    auto& jobs = ::dhcp::core::JobRegistry::instance();

    std::string detail;
    auto st = ::dhcp::files::FileStatus::Ok;
    // A stop that arrived before the erase started prevents it altogether — the
    // only case in which the card is left untouched.
    bool cancelled = jobs.cancelRequested("format");

    if (!cancelled) {
        st = work->files->format(work->volume, &detail);
        // The escape hatch waits for this flag before cutting the card's supply:
        // whatever happens next, the destructive call is behind us and there is
        // nothing left to break.
        g_formatCallDone.store(true);
        // The destructive call cannot be cut short while it runs, so a stop that
        // arrived meanwhile is honoured here, at the end of that step: the
        // volume stays in the state the call left it — usually unmounted, which
        // is exactly what an operator with a failing card wants — and the
        // operation ends as cancelled instead of pretending it succeeded.
        cancelled = jobs.cancelRequested("format");
    }

    // Stopped from the scheduler while the call ran: the record is already gone
    // (the cancel route finished it, so the row left the list at once) and the
    // only thing left to do is to tell the client that asked for the format.
    const bool abandoned = g_formatAbandoned.exchange(false);
    if (abandoned) {
        ESP_LOGW(TAG, "format of volume '%s' stopped from the scheduler: the "
                      "erase returned afterwards", work->volume.c_str());
        formatAnswer(work->req, /*cancelled*/ true, st, detail);
        work->finish();
        delete work;
        vTaskDelete(nullptr);
        return;
    }

    if (cancelled) {
        ESP_LOGW(TAG, "format of volume '%s' cancelled from the scheduler",
                 work->volume.c_str());
        jobs.finish("format", ::dhcp::core::JobState::Cancelled, work->volume);
    } else if (st == ::dhcp::files::FileStatus::Ok) {
        ESP_LOGW(TAG, "volume '%s' formatted from the web UI", work->volume.c_str());
        jobs.finish("format", ::dhcp::core::JobState::Done, work->volume);
    } else {
        ESP_LOGE(TAG, "format of volume '%s' failed: %s", work->volume.c_str(),
                 detail.c_str());
        jobs.finish("format", ::dhcp::core::JobState::Failed, detail);
    }

    formatAnswer(work->req, cancelled, st, detail);
    work->finish();
    delete work;
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t RestApi::handlePostFileFormat(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    const std::string body = readBody(req);
    const std::string volume = jsonGetStr(body, "volume");
    if (!jsonGetBool(body, "confirm", false)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}",
                              nullptr);
    }

    auto& jobs = ::dhcp::core::JobRegistry::instance();
    // One format at a time. The record is single-flight, so a second request
    // would silently take the running one's place — the operator would lose
    // sight of the erase that is still going on, and two of them on one card are
    // never wanted.
    if (jobs.contains("format")) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"a format is already running\"}");
        return ESP_OK;
    }

    auto* work = new FormatWork{};
    work->files = s_files;
    work->volume = volume;

    if (!jobs.begin("format", "jobs.format", volume)) {
        delete work;
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"too many operations are running\"}");
        return ESP_OK;
    }

    // A fresh start, with nothing left over from a format that was stopped.
    g_formatAbandoned.store(false);
    g_formatCallDone.store(false);

    // The record went up first, so a task that cannot even be created does not
    // leave a stale entry behind (runDetached answers the request itself).
    if (!runDetached(req, work, formatTask, "file_format",
                     "{\"status\":\"error\",\"message\":\"cannot start the format\"}")) {
        jobs.finish("format", ::dhcp::core::JobState::Failed, "no task");
    }
    return ESP_OK;   // the answer is sent by the task
}

namespace {

/** @brief Stack of a detached transfer task (the httpd default is 8192). */
constexpr uint32_t kTransferStack = 8192;

/**
 * @brief Transfers allowed to run at once.
 *
 * Each one costs a task stack plus its own transfer window (512 KB in PSRAM), so
 * they are bounded: the file explorer sends one file at a time, and two browsers
 * mean two. Anything above this is answered with 503 instead of eating the heap.
 */
constexpr int kMaxTransfers = 4;

/** @brief Transfers running right now (see @ref kMaxTransfers). */
std::atomic<int> g_transfers{0};

/**
 * @brief RAII slot in the transfer budget.
 *
 * Constructed where the request arrives, released when the work is done — the
 * slot has to outlive the handler, so the work item owns it.
 */
class TransferSlot {
public:
    TransferSlot() : count_(g_transfers.fetch_add(1) + 1) {}
    ~TransferSlot() { g_transfers.fetch_sub(1); }

    TransferSlot(const TransferSlot&) = delete;
    TransferSlot& operator=(const TransferSlot&) = delete;

    /** @brief False when the budget is already spent. */
    bool ok() const { return count_ <= kMaxTransfers; }

private:
    int count_;
};

/**
 * @brief Byte window of one transfer.
 *
 * The window is what makes a transfer independent of the file size: **512 KB in
 * PSRAM** (the agreed size), with a smaller fallback in the internal heap, and
 * finally `nullptr` — the caller then uses its own 1 KB stack buffer.
 *
 * Each transfer allocates its own. It used to be a single static buffer, which
 * was safe for exactly the reason this stage removed: "the httpd task serves one
 * request at a time". Two transfers now run side by side, and a shared window
 * would let them overwrite each other's data mid-flight.
 */
class TransferWindow {
public:
    TransferWindow()
    {
        constexpr size_t kPreferred = 512 * 1024;
        constexpr size_t kFallback = 64 * 1024;

        buf_ = static_cast<uint8_t*>(heap_caps_malloc(kPreferred, MALLOC_CAP_SPIRAM));
        if (buf_ != nullptr) {
            size_ = kPreferred;
            return;
        }
        buf_ = static_cast<uint8_t*>(heap_caps_malloc(kFallback, MALLOC_CAP_DEFAULT));
        if (buf_ != nullptr) {
            size_ = kFallback;
            return;
        }
        ESP_LOGW(TAG, "no transfer window available, using a 1 KB stack buffer");
    }

    ~TransferWindow()
    {
        if (buf_ != nullptr) heap_caps_free(buf_);
    }

    TransferWindow(const TransferWindow&) = delete;
    TransferWindow& operator=(const TransferWindow&) = delete;

    uint8_t* data() const { return buf_; }
    size_t size() const { return size_; }

private:
    uint8_t* buf_ = nullptr;
    size_t size_ = 0;
};

/**
 * @brief Answer a request with an error, on whichever handle is still valid.
 *
 * @param[in] req      Request to answer (the async one once it exists).
 * @param[in] message  Ready-made JSON body.
 */
void answerError(httpd_req* req, const char* message)
{
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, message);
}

bool runDetached(httpd_req* req, AsyncRequest* work, TaskFunction_t body,
                 const char* taskName, const char* failMessage)
{
    if (httpd_req_async_handler_begin(req, &work->req) != ESP_OK) {
        ESP_LOGE(TAG, "cannot detach '%s' from the server task", taskName);
        answerError(req, failMessage);
        delete work;
        return false;
    }

    if (xTaskCreate(body, taskName, kTransferStack, work,
                    tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the task '%s'", taskName);
        answerError(work->req, failMessage);
        work->finish();
        delete work;
        return false;
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────
// GET /api/files/download?volume=<id>&path=<rel>
// ─────────────────────────────────────────────────────
// Streams the file to the client in chunks (no Content-Length — the response
// is chunked, exactly like the static-file handler), so a 21 MB file costs
// only the transfer window, not a buffer of its size.
//
// Content-Disposition carries both a plain quoted name (for old clients) and
// the RFC 5987 UTF-8 form, so a Cyrillic file name survives the round-trip.
//
// The streaming happens in a task of its own (see above): a slow client must not
// hold the server, and a download of a large file is exactly that.

namespace {

/**
 * @brief Percent-encode for `filename*=UTF-8''…` (RFC 5987).
 *
 * Everything outside the `attr-char` set is escaped; the result is ASCII, so
 * it is safe in an HTTP header even for a UTF-8 name.
 */
std::string rfc5987Encode(const std::string& in)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        const bool attrChar =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '!' || c == '#' || c == '$' || c == '&' || c == '+' ||
            c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
            c == '|' || c == '~';
        if (attrChar) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

/** @brief ASCII-safe replacement for the quoted `filename=` parameter. */
std::string asciiName(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) continue;       // never in a header value
        out += (u < 0x80) ? c : '_';
    }
    if (out.empty()) out = "download";
    return out;
}

} // namespace

namespace {

/** @brief Work item of one download (see @ref AsyncRequest). */
struct DownloadWork : AsyncRequest {
    ::dhcp::files::IFileManager* files = nullptr;
    std::string volume;
    std::string path;
    TransferSlot slot;            ///< Holds a place in the transfer budget
};

/** @brief Body of a download: open the file, stream it, close the request. */
void downloadTask(void* arg)
{
    auto* work = static_cast<DownloadWork*>(arg);
    auto* req = work->req;

    std::unique_ptr<::dhcp::files::IFileSource> src;
    std::string detail;
    const auto st = work->files->openRead(work->volume, work->path, src, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        sendFileResult(req, st, "{}", &detail);
        work->finish();
        delete work;
        vTaskDelete(nullptr);
        return;
    }

    std::string norm;
    if (!::dhcp::storage::PathUtil::normalize(work->path, norm)) norm = work->path;
    const std::string name = ::dhcp::storage::PathUtil::basename(norm);

    httpd_resp_set_type(req, "application/octet-stream");
    const std::string disposition = "attachment; filename=\"" + asciiName(name) +
                                    "\"; filename*=UTF-8''" + rfc5987Encode(name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());

    TransferWindow window;
    uint8_t stackBuf[1024];
    uint8_t* buf = window.data();
    size_t bufSize = window.size();
    if (buf == nullptr) {
        buf = stackBuf;
        bufSize = sizeof(stackBuf);
    }

    ESP_LOGI(TAG, "download %s (%llu bytes, volume %s)", norm.c_str(),
             (unsigned long long)src->size(), work->volume.c_str());

    bool aborted = false;
    while (true) {
        const size_t n = src->read(buf, bufSize);
        if (n == 0) break;   // EOF (or an error — the client sees a short body)
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buf), n) != ESP_OK) {
            ESP_LOGW(TAG, "download %s aborted by the client", norm.c_str());
            aborted = true;
            break;
        }
    }

    if (!aborted) {
        httpd_resp_send_chunk(req, nullptr, 0);   // terminate the chunked body
        if (src->error()) {
            ESP_LOGE(TAG, "download %s ended with a read error", norm.c_str());
        }
    }

    work->finish();
    delete work;
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t RestApi::handleGetFileDownload(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    std::string volume, path;
    if (!queryParam(req, "volume", volume)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotFound, "{}");
    }
    if (!queryParam(req, "path", path)) path = "/";

    auto* work = new DownloadWork{};
    work->files = s_files;
    work->volume = volume;
    work->path = path;

    if (!work->slot.ok()) {
        delete work;
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"too many transfers are running\"}");
        return ESP_OK;
    }

    runDetached(req, work, downloadTask, "file_download",
                "{\"status\":\"error\",\"message\":\"cannot start the download\"}");
    return ESP_OK;   // the answer is sent by the task
}


// ─────────────────────────────────────────────────────
// POST /api/files/upload?volume=<id>&path=<rel>
// ─────────────────────────────────────────────────────
// Raw body (`application/octet-stream`), streamed to the volume in windows: the
// file is written to `<name>.part` and only renamed into place once the whole
// body arrived, so an interrupted upload never leaves a truncated file under the
// real name.
//
//   ?volume=fat&path=/logs/2026-09-15.txt
//
// Answers 200 {"status":"ok","bytes":N,"path":"…"} on success; 411 when the
// body length is missing (a chunked upload cannot be space-checked), 507 when
// the volume cannot hold it, 500 when the write or the final rename fails.
//
// The body is read in a task of its own (see "Long transfers outside the server
// task"): a folder upload is a long series of long requests, and leaving them in
// the server task froze every other client for the whole transfer.

/**
 * @brief Read an unsigned decimal query parameter.
 *
 * `offset`/`total` of a resumable upload have to be plain numbers: a client that
 * sends something else is told so instead of getting a silent zero.
 *
 * @return false when the parameter is absent or not a plain number.
 */
static bool queryParamU64(httpd_req* req, const char* key, uint64_t& out)
{
    std::string raw;
    if (!queryParam(req, key, raw)) return false;
    if (raw.empty() || raw.size() > 20) return false;

    uint64_t value = 0;
    for (char c : raw) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    out = value;
    return true;
}

namespace {

/**
 * @brief Registry id of an upload.
 *
 * One row per file rather than one per kind: transfers run side by side now, so
 * two uploads must not share a single record (a pause in one would have shown as
 * a pause in the other). The path is what makes the id unique — and what makes a
 * *resumed* upload find its own row again.
 */
std::string uploadJobId(const std::string& path)
{
    return "upload:" + path;
}

/**
 * @brief The upload that is waiting to be continued, if any.
 *
 * A paused transfer exists only as a `<name>.part` on the volume — the device
 * keeps no session for it — so the scheduler page needs someone who remembers
 * *which* file it was in order to drop it. One record is enough (a pause is
 * something the client asked for), and it is the only mutable state the
 * transfer handling keeps, which is why it lives here next to the transfer and
 * not in the class: the work now happens in tasks, so it is guarded.
 */
struct PausedUpload {
    std::mutex mutex;
    std::string id;
    std::string volume;
    std::string path;

    /** @brief Remember the transfer that just paused. */
    void remember(const std::string& jobId, const std::string& vol, const std::string& p)
    {
        std::lock_guard<std::mutex> lock(mutex);
        id = jobId;
        volume = vol;
        path = p;
    }

    /** @brief Forget @p jobId — it started again or was published. */
    void forget(const std::string& jobId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (id != jobId) return;   // another transfer is the remembered one
        id.clear();
        volume.clear();
        path.clear();
    }

    /**
     * @brief Take the record of @p jobId out, if that is the paused one.
     * @return false when nothing is paused or it is a different transfer.
     */
    bool take(const std::string& jobId, std::string& vol, std::string& p)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (id.empty() || id != jobId) return false;
        vol = volume;
        p = path;
        id.clear();
        volume.clear();
        path.clear();
        return true;
    }
};

PausedUpload g_pausedUpload;

/** @brief Remember the transfer that just paused (see @ref PausedUpload). */
void rememberPausedUpload(const std::string& jobId, const std::string& volume,
                          const std::string& path)
{
    g_pausedUpload.remember(jobId, volume, path);
}

/** @brief Forget @p jobId — it started again or was published. */
void forgetPausedUpload(const std::string& jobId)
{
    g_pausedUpload.forget(jobId);
}

/**
 * @brief Take the record of the paused upload @p jobId, if that is the one.
 * @return false when nothing is paused or it is a different transfer.
 */
bool takePausedUpload(const std::string& jobId, std::string& volume, std::string& path)
{
    return g_pausedUpload.take(jobId, volume, path);
}

/** @brief Work item of one upload (see @ref AsyncRequest). */
struct UploadWork : AsyncRequest {
    ::dhcp::files::IFileManager* files = nullptr;
    std::string volume;
    std::string path;
    uint64_t offset = 0;
    uint64_t total = 0;
    uint64_t chunk = 0;           ///< Bytes of this request (Content-Length)
    TransferSlot slot;            ///< Holds a place in the transfer budget
};

/** @brief Body of an upload: read the request body, write it, answer, release. */
void uploadTask(void* arg)
{
    auto* work = static_cast<UploadWork*>(arg);
    auto* req = work->req;
    const std::string& volume = work->volume;
    const std::string& path = work->path;
    const uint64_t chunk = work->chunk;

    ::dhcp::files::UploadRange range;
    std::unique_ptr<::dhcp::files::IFileSink> sink;
    std::string detail;
    auto st = work->files->openWrite(volume, path, work->offset, work->total,
                                     chunk, sink, range, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        sendFileResult(req, st, "{}", &detail);
        work->finish();
        delete work;
        vTaskDelete(nullptr);
        return;
    }

    // The operator can watch and stop this transfer from the scheduler page: it
    // is the longest operation the device knows, and the registry is where that
    // page looks. Cancelling it there aborts the request and drops the `.part`
    // (see the read loop), exactly like the Files page's own button does.
    auto& jobs = ::dhcp::core::JobRegistry::instance();
    const std::string jobId = uploadJobId(path);
    jobs.begin(jobId, "jobs.upload", path, static_cast<uint32_t>(range.total));
    // A new request for this file replaces whatever was paused before (the
    // registry is single-flight per id), so the remembered pause goes with it.
    forgetPausedUpload(jobId);
    uint64_t reported = 0;

    TransferWindow window;
    uint8_t stackBuf[1024];
    uint8_t* buf = window.data();
    size_t bufSize = window.size();
    if (buf == nullptr) {
        buf = stackBuf;
        bufSize = sizeof(stackBuf);
    }

    // httpd_req_recv can return HTTPD_SOCK_ERR_TIMEOUT between TCP segments of
    // a multi-chunk body — the same bounded retry as the other upload handlers.
    uint64_t remaining = chunk;
    int retries = 0;
    bool ok = true;
    bool stopped = false;
    while (remaining > 0) {
        const size_t want = static_cast<size_t>(
            (remaining < bufSize) ? remaining : bufSize);
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(buf), want);
        if (got < 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT && retries++ < 20) continue;
            ok = false;
            break;
        }
        if (got == 0) {
            ok = false;
            break;
        }
        if (!sink->write(buf, static_cast<size_t>(got))) {
            ok = false;
            break;
        }
        remaining -= static_cast<uint64_t>(got);

        if (jobs.cancelRequested(jobId)) { stopped = true; break; }
        // Progress every 64 KB: often enough for the page that watches, cheap
        // enough not to take the registry mutex once per window.
        if (sink->written() - reported >= 64 * 1024) {
            reported = sink->written();
            jobs.progress(jobId, static_cast<uint32_t>(reported), 0, path);
        }
    }

    if (stopped) {
        const uint64_t written = sink->written();
        sink->abort();   // the operator cancelled: no `.part` is left behind
        jobs.finish(jobId, ::dhcp::core::JobState::Cancelled, path);
        ESP_LOGW(TAG, "upload of '%s' cancelled after %llu bytes (scheduler)",
                 path.c_str(), (unsigned long long)written);
        answerError(req,
            "{\"status\":\"error\",\"message\":\"upload cancelled\","
            "\"detail\":\"stopped from the task scheduler\"}");
        work->finish();
        delete work;
        vTaskDelete(nullptr);
        return;
    }

    // A paused or interrupted upload answers with the offset it reached: that is
    // the number the client continues from, and it comes from the device rather
    // than from what the client thought it had sent.
    auto sendPartial = [req, &range](uint64_t written) {
        JsonWriter w;
        w.str("status", "partial");
        w.num("offset", static_cast<int64_t>(written));
        w.num("total", static_cast<int64_t>(range.total));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, w.toString().c_str());
    };

    if (!ok || !range.completes()) {
        const uint64_t written = sink->written();

        // A body that stopped early can only be continued in resumable mode —
        // and only if the client is still there to hear about it. Without
        // `total` there is nothing to continue, so the partial file goes.
        if (!range.resumable) {
            sink->abort();
            jobs.finish(jobId, ::dhcp::core::JobState::Failed, path);
            ESP_LOGE(TAG, "upload into '%s' failed after %llu of %llu bytes",
                     path.c_str(), (unsigned long long)written,
                     (unsigned long long)range.total);
            answerError(req,
                "{\"status\":\"error\",\"message\":\"upload failed\","
                "\"detail\":\"incomplete or write error\"}");
            work->finish();
            delete work;
            vTaskDelete(nullptr);
            return;
        }

        sink->keep();
        // Not finished: the `.part` waits for the client to continue, so the
        // scheduler page shows the transfer as paused rather than gone — and
        // remembers where it is, because that page is also the only place from
        // which a paused transfer can be dropped (nothing else knows its name).
        jobs.pause(jobId, path);
        rememberPausedUpload(jobId, volume, path);
        ESP_LOGI(TAG, "upload of '%s' kept at %llu of %llu bytes",
                 path.c_str(), (unsigned long long)written,
                 (unsigned long long)range.total);
        sendPartial(written);
        work->finish();
        delete work;
        vTaskDelete(nullptr);
        return;
    }

    if (!sink->commit()) {
        sink->abort();
        jobs.finish(jobId, ::dhcp::core::JobState::Failed, path);
        answerError(req,
            "{\"status\":\"error\",\"message\":\"upload failed\","
            "\"detail\":\"cannot publish the file\"}");
        work->finish();
        delete work;
        vTaskDelete(nullptr);
        return;
    }

    std::string norm;
    if (!::dhcp::storage::PathUtil::normalize(path, norm)) norm = path;
    jobs.finish(jobId, ::dhcp::core::JobState::Done, norm);
    forgetPausedUpload(jobId);

    JsonWriter w;
    w.str("status", "ok");
    w.num("bytes", static_cast<int64_t>(sink->written()));
    w.str("path", norm);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, w.toString().c_str());

    work->finish();
    delete work;
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t RestApi::handlePostFileUpload(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    std::string volume, path;
    if (!queryParam(req, "volume", volume)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotFound, "{}");
    }
    if (!queryParam(req, "path", path)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }

    // `total` turns the request into one piece of a resumable upload: the client
    // says how big the finished file will be and where this piece starts, so a
    // transfer can be paused and continued (see UploadRange). Without it the
    // body is the whole file, which is what every older client sends.
    uint64_t offset = 0, total = 0;
    const bool hasOffset = queryParamU64(req, "offset", offset);
    const bool hasTotal = queryParamU64(req, "total", total);
    if (hasOffset && !hasTotal) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"offset requires total\"}");
        return ESP_OK;
    }

    // The free-space check needs to know the size up front; browsers always
    // send Content-Length for a file body.
    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "411 Length Required");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Content-Length required\"}");
        return ESP_OK;
    }

    auto* work = new UploadWork{};
    work->files = s_files;
    work->volume = volume;
    work->path = path;
    work->offset = hasOffset ? offset : 0;
    work->total = hasTotal ? total : 0;
    work->chunk = static_cast<uint64_t>(req->content_len);

    if (!work->slot.ok()) {
        delete work;
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"too many transfers are running\"}");
        return ESP_OK;
    }

    runDetached(req, work, uploadTask, "file_upload",
                "{\"status\":\"error\",\"message\":\"cannot start the upload\"}");
    return ESP_OK;   // the answer is sent by the task
}


// ─────────────────────────────────────────────────────
// GET /api/files/upload/offset
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handleGetFileUploadOffset(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    std::string volume, path;
    if (!queryParam(req, "volume", volume)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotFound, "{}");
    }
    if (!queryParam(req, "path", path)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }

    uint64_t offset = 0;
    std::string detail;
    auto st = s_files->uploadOffset(volume, path, offset, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        return sendFileResult(req, st, "{}", &detail);
    }

    std::string json = "{";
    addJsonInt(json, "offset", static_cast<int64_t>(offset), false);
    json += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/files/upload/cancel
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostFileUploadCancel(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    std::string volume, path;
    if (!queryParam(req, "volume", volume)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotFound, "{}");
    }
    if (!queryParam(req, "path", path)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }

    std::string detail;
    auto st = s_files->discardUpload(volume, path, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        return sendFileResult(req, st, "{}", &detail);
    }

    // A paused upload of this file is over: the registry must not keep showing it
    // as waiting (nothing will continue it). Nothing happens when there is no such
    // record, which is the normal case for a completed upload.
    ::dhcp::core::JobRegistry::instance().finish(uploadJobId(path),
                                                ::dhcp::core::JobState::Cancelled, path);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET /api/jobs — the long-running operations of the device
// ─────────────────────────────────────────────────────
// One list for everything that takes minutes: the volume check, the cache
// persist job, uploads, formatting. The operations announce themselves in
// JobRegistry (see its comment), the page draws what this endpoint returns and
// cancels through POST /api/jobs/cancel — so a new long operation needs no
// endpoint of its own.

esp_err_t RestApi::handleGetJobs(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    // No LAN-only filter: the list holds no file data, and every client that may
    // see it is authenticated anyway.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        ::dhcp::web::FileJson::jobs(::dhcp::core::JobRegistry::instance().snapshot()).c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// POST /api/jobs/cancel  {"id": "file_check"}
// ─────────────────────────────────────────────────────
// Asks an unfinished operation to stop. There is no list of "stoppable" kinds:
// anything the scheduler shows is still running or waiting, so it can be asked.
// The answer is immediate even when the operation needs a moment to give up
// (the page shows "stopping…" until the record disappears).

esp_err_t RestApi::handlePostJobCancel(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    const std::string body = readBody(req);
    const std::string id = jsonGetStr(body, "id");
    if (id.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"id is required\"}");
        return ESP_OK;
    }

    auto& jobs = ::dhcp::core::JobRegistry::instance();
    if (!jobs.contains(id)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"no such operation\"}");
        return ESP_OK;
    }

    if (!jobs.requestCancel(id)) {
        // Only an operation that has already ended can say no: everything in the
        // list is unfinished (a finished one-off is removed, a scheduled one is
        // done and waiting), so there is no "not stoppable" case left — that was
        // the old `cancellable` flag, which kept the button off operations the
        // operator could not stop.
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"message\":\"the operation has already finished\"}");
        return ESP_OK;
    }

    // The registry only records the request: the subsystem that owns the
    // operation acts on it. Most owners poll the flag themselves (the walk, the
    // upload's read loop, the format task); the cases that cannot act on it are
    // settled here, in the layer that knows them.
    if (id == "file_check" && s_files) {
        s_files->checkCancel();
    }

    // A format ends **here and now**, and that is the whole point of this branch.
    // The erase is one call into IDF and FatFS: while it runs, nothing in the
    // firmware can cut it short (it returns when the driver stops answering — on
    // a failing card that is what takes so long). Waiting for it would mean the
    // row sits at «остановка…» for minutes and the operator cannot retry, which
    // is exactly what he asked to be rid of. So the operation is finished as
    // cancelled at once — the row leaves the list, a retry is possible as soon as
    // the volume is free again — and the card is taken care of by the hardware:
    // one second later its supply is cut for five, which makes the stuck call
    // fail and lets the volume come back through the normal mount path.
    if (id == "format") {
        // The record carries the volume id, and it is about to go.
        std::string volume;
        for (const auto& job : jobs.snapshot()) {
            if (job.id == id) volume = job.arg;
        }
        g_formatAbandoned.store(true);
        jobs.finish(id, ::dhcp::core::JobState::Cancelled, "stopped");
        ESP_LOGW(TAG, "format stopped from the scheduler; the erase is broken by "
                      "cutting the card's supply");
        if (s_files && !volume.empty()) startPowerCut(s_files, volume);
    }

    // A *paused* upload has no request in flight to notice the flag. The `.part`
    // it left behind is dropped here — the same thing the Files page's own cancel
    // button does — and the record ends with it, because nothing will continue
    // that transfer. Only one transfer can be paused at a time, and this is where
    // it was remembered when its request ended.
    {
        std::string volume, path;
        if (takePausedUpload(id, volume, path)) {
            std::string detail;
            const auto st = s_files ? s_files->discardUpload(volume, path, &detail)
                                    : ::dhcp::files::FileStatus::NotMounted;
            ESP_LOGW(TAG, "paused upload of '%s' dropped on request: %s",
                     path.c_str(),
                     st == ::dhcp::files::FileStatus::Ok ? "ok" : detail.c_str());
            jobs.finish(id, ::dhcp::core::JobState::Cancelled, path);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET/POST /api/files/text?volume=<id>&path=<rel>
// ─────────────────────────────────────────────────────
// Text read/write for the built-in editor. The whole file travels in one JSON
// document, so there is a hard limit (`IFileManager::kMaxTextBytes`, 512 KB)
// and the content must actually be text: a binary file is refused with `415`
// (downloading it still works), and a file that grew past the limit is refused
// with `413`.
//
//   GET  → {"volume":"fat","path":"/notes.txt","size":42,"mtime":1757971200,
//           "truncated":false,"text":"…"}
//   POST ← {"volume":"fat","path":"/notes.txt","text":"…","mtime":1757971200}
//
// `mtime` is optional but recommended on save: when it is present and differs
// from the file on the volume, the write is refused with `409` instead of
// silently overwriting somebody else's change.

namespace {

/** @brief Share of "control-ish" bytes above which a file counts as binary. */
constexpr size_t kBinaryRatioPercent = 10;

/**
 * @brief Decode one JSON string value, escapes included.
 *
 * The lightweight `jsonGetStr()` used for the settings bodies stops at the
 * first quote, which is fine for short single-line values but would corrupt a
 * text file: newlines arrive escaped (`\n`) and a `\"` inside the content would
 * truncate the value. The editor endpoint therefore decodes properly —
 * `\" \\ \/ \b \f \n \r \t` and `\uXXXX` (with surrogate pairs) — and converts
 * to UTF-8.
 *
 * @return false when the key is missing or the value is not a valid string.
 */
bool jsonDecodeStr(const std::string& json, const std::string& key, std::string& out)
{
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;

    /** @brief Append one code point as UTF-8. */
    auto appendUtf8 = [&out](uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };

    out.clear();
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') return true;                // end of the value

        if (c != '\\') {
            out += c;
            continue;
        }
        if (pos >= json.size()) return false;     // dangling escape

        const char esc = json[pos++];
        switch (esc) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                if (pos + 4 > json.size()) return false;
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = json[pos + i];
                    int v = (h >= '0' && h <= '9') ? h - '0'
                          : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                          : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (v < 0) return false;
                    cp = (cp << 4) | static_cast<uint32_t>(v);
                }
                pos += 4;

                // Surrogate pair → one code point above the BMP.
                if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= json.size() &&
                    json[pos] == '\\' && json[pos + 1] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; ++i) {
                        const char h = json[pos + 2 + i];
                        int v = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (v < 0) { ok = false; break; }
                        lo = (lo << 4) | static_cast<uint32_t>(v);
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        pos += 6;
                    }
                }
                appendUtf8(cp);
                break;
            }
            default:
                return false;   // unknown escape — refuse rather than guess
        }
    }
    return false;   // unterminated string
}

/**
 * @brief Cheap binary heuristic for the editor.
 *
 * A NUL byte is decisive (text files never contain one), otherwise the share
 * of bytes outside tab/CR/LF/printable/UTF-8 is measured — a UTF-8 text file
 * with a few odd bytes in a comment still passes, a JPEG or a `.dat` does not.
 */
bool looksLikeText(const std::string& data)
{
    if (data.find('\0') != std::string::npos) return false;

    size_t suspicious = 0;
    for (unsigned char c : data) {
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c >= 0x20 && c != 0x7F) continue;   // printable ASCII or UTF-8 byte
        ++suspicious;
    }
    return suspicious * 100 <= data.size() * kBinaryRatioPercent;
}

} // namespace

esp_err_t RestApi::handleGetFileText(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    std::string volume, path;
    if (!queryParam(req, "volume", volume)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotFound, "{}");
    }
    if (!queryParam(req, "path", path)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }

    ::dhcp::files::FileEntry info;
    std::string detail;
    auto st = s_files->stat(volume, path, info, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        return sendFileResult(req, st, "{}", &detail);
    }
    if (info.isDir) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }
    if (info.size > ::dhcp::files::IFileManager::kMaxTextBytes) {
        return sendFileResult(req, ::dhcp::files::FileStatus::TooLarge, "{}");
    }

    std::unique_ptr<::dhcp::files::IFileSource> src;
    st = s_files->openRead(volume, path, src, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        return sendFileResult(req, st, "{}", &detail);
    }

    std::string text;
    text.reserve(static_cast<size_t>(info.size));
    {
        uint8_t buf[2048];
        while (true) {
            const size_t n = src->read(buf, sizeof(buf));
            if (n == 0) break;
            text.append(reinterpret_cast<const char*>(buf), n);
            if (text.size() > ::dhcp::files::IFileManager::kMaxTextBytes) {
                return sendFileResult(req, ::dhcp::files::FileStatus::TooLarge, "{}");
            }
        }
    }
    if (src->error()) {
        return sendFileResult(req, ::dhcp::files::FileStatus::IoError, "{}", &detail);
    }
    if (!looksLikeText(text)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotText, "{}");
    }

    std::string norm;
    if (!::dhcp::storage::PathUtil::normalize(path, norm)) norm = path;

    FileJson::TextPayload payload;
    payload.volume = volume;
    payload.path = norm;
    payload.text = text;
    payload.size = text.size();
    payload.mtime = info.mtime;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, FileJson::text(payload).c_str());
    return ESP_OK;
}

esp_err_t RestApi::handlePostFileText(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    // The body carries the whole text, and JSON escaping can double its size
    // (a file made of newlines/quotes), so the read cap is twice the editor
    // limit plus the small envelope; anything longer is refused after parsing.
    const std::string body =
        readBody(req, ::dhcp::files::IFileManager::kMaxTextBytes * 2 + 4096);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    // `text` must be the LAST member: the envelope fields are read from the
    // part of the body before it, so a file whose content happens to contain
    // `"mtime":0` (documentation, JSON samples, …) cannot hijack the parsing.
    const size_t textKey = body.find("\"text\"");
    if (textKey == std::string::npos) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }
    const std::string envelope = body.substr(0, textKey);

    const std::string volume = jsonGetStr(envelope, "volume");
    const std::string path = jsonGetStr(envelope, "path");
    const int64_t clientMtime = jsonGetInt(envelope, "mtime", -1);

    std::string text;
    if (!jsonDecodeStr(body, "text", text)) {
        // A missing or malformed `text` member: report it as a bad request the
        // same way every other file endpoint does.
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }

    if (text.size() > ::dhcp::files::IFileManager::kMaxTextBytes) {
        return sendFileResult(req, ::dhcp::files::FileStatus::TooLarge, "{}");
    }
    if (!looksLikeText(text)) {
        return sendFileResult(req, ::dhcp::files::FileStatus::NotText, "{}");
    }

    // Optimistic-locking: only when the client sends the mtime it read.
    ::dhcp::files::FileEntry info;
    std::string detail;
    const auto statSt = s_files->stat(volume, path, info, &detail);
    const bool existed = (statSt == ::dhcp::files::FileStatus::Ok);
    if (existed && info.isDir) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }
    if (existed && clientMtime >= 0 &&
        static_cast<uint64_t>(clientMtime) != info.mtime) {
        ESP_LOGW(TAG, "text save refused: %s changed on the volume", path.c_str());
        return sendFileResult(req, ::dhcp::files::FileStatus::Conflict, "{}");
    }

    std::unique_ptr<::dhcp::files::IFileSink> sink;
    // The editor saves a whole file in one go, so there is nothing to resume:
    // no `total`, and an empty text is a valid empty file (the same meaning the
    // upload endpoint always had for one request).
    ::dhcp::files::UploadRange range;
    auto st = s_files->openWrite(volume, path, 0, 0, text.size(), sink, range, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        return sendFileResult(req, st, "{}", &detail);
    }

    if (!text.empty() &&
        !sink->write(reinterpret_cast<const uint8_t*>(text.data()), text.size())) {
        sink->abort();
        return sendFileResult(req, ::dhcp::files::FileStatus::IoError, "{}", &detail);
    }
    if (!sink->commit()) {
        return sendFileResult(req, ::dhcp::files::FileStatus::IoError, "{}", &detail);
    }

    ::dhcp::files::FileEntry after;
    const uint64_t mtime =
        (s_files->stat(volume, path, after) == ::dhcp::files::FileStatus::Ok)
            ? after.mtime : 0;

    std::string json = "{\"status\":\"ok\"";
    addJsonInt(json, "size", static_cast<int64_t>(text.size()), true);
    addJsonInt(json, "mtime", static_cast<int64_t>(mtime), true);
    json += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// GET/POST /api/files/settings
// ─────────────────────────────────────────────────────
// Access policy of the file explorer. `allow_own_subnet` (default ON) keeps
// browsing/uploading inside the device's own subnet; the subnet itself is the
// device address + netmask from the DHCP settings (same definition as the DNS
// and NTP filters), so this page has no address field of its own.

esp_err_t RestApi::handleGetFileSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    const auto cfg = ::dhcp::core::Config::instance().getFiles();
    const auto net = ::dhcp::core::Config::instance().getDhcp();

    FileJson::SettingsPayload payload;
    payload.enabled = s_files ? s_files->supported() : false;
    payload.allowOwnSubnet = cfg.allowOwnSubnet;
    payload.filterActive = s_files ? s_files->filterActive() : false;
    payload.subnetAddress = net.serverIp;
    payload.subnetMask = net.subnet;
    payload.blockedCount =
        s_files ? static_cast<uint64_t>(s_files->foreignBlocked()) : 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, FileJson::settings(payload).c_str());
    return ESP_OK;
}

esp_err_t RestApi::handlePostFileSettings(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;

    const std::string body = readBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }

    auto cfg = ::dhcp::core::Config::instance().getFiles();
    cfg.allowOwnSubnet = jsonGetBool(body, "allow_own_subnet", cfg.allowOwnSubnet);
    ::dhcp::core::Config::instance().setFiles(cfg);

    if (s_files) s_files->applyAccessFilter();
    ESP_LOGI(TAG, "file settings updated (allow_own_subnet=%d)",
             (int)cfg.allowOwnSubnet);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/*
 * Read-only volume check ("Check for errors").
 *
 * POST starts a walk that reads every file of the volume: on a card that is
 * many gigabytes that takes minutes, so the handler answers immediately and the
 * page polls GET — the same shape the built-in cache persistence uses. GET is
 * also the only way the UI learns the outcome: the report is a snapshot, so a
 * poll in mid-walk is not an error.
 */
esp_err_t RestApi::handlePostFileCheck(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    const std::string body = readBody(req, 512);
    const std::string volume = jsonGetStr(body, "volume");
    if (volume.empty()) {
        return sendFileResult(req, ::dhcp::files::FileStatus::InvalidPath, "{}");
    }

    std::string detail;
    const auto st = s_files->checkStart(volume, &detail);
    if (st != ::dhcp::files::FileStatus::Ok) {
        return sendFileResult(req, st, "{}", &detail);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"started\"}");
    return ESP_OK;
}

esp_err_t RestApi::handlePostFileCheckCancel(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    // Only sets a flag: the walk stops at its next step, so this answers at once.
    s_files->checkCancel();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t RestApi::handleGetFileCheck(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;
    if (!checkFileAccess(req)) return ESP_OK;
    if (!s_files) return sendNoFileManager(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        ::dhcp::web::FileJson::check(s_files->checkReport()).c_str());
    return ESP_OK;
}

} // namespace web
} // namespace dhcp
