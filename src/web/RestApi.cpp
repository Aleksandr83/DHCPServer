#include "RestApi.h"
#include "AuthManager.h"
#include "../core/Version.h"
#include "../core/Config.h"
#include "../core/CpuMonitor.h"
#include "../wifi/IWiFiManager.h"
#include "../dhcp/IDhcpServer.h"
#include "../dhcp/DhcpServer.h"
#include "../dns/DnsServer.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>

#include "esp_log.h"
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
::dhcp::web::AuthManager*    RestApi::s_auth = nullptr;

void RestApi::init(::dhcp::wifi::IWiFiManager* wifi,
                    ::dhcp::dhcp::IDhcpServer* dhcpSrv,
                    ::dhcp::dns::DnsServer* dnsSrv,
                    ::dhcp::web::AuthManager* auth)
{
    s_wifi = wifi;
    s_dhcp = dhcpSrv;
    s_dns  = dnsSrv;
    s_auth = auth;
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

std::string RestApi::readBody(httpd_req* req)
{
    size_t totalLen = req->content_len;
    if (totalLen == 0) return "";
    // Sanity cap: settings bodies are small (<1 KB). Guard against a bogus
    // content_len claiming a huge body (the httpd socket buffer is limited).
    if (totalLen > 4096) totalLen = 4096;

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
    addJsonInt(json, "cpu_load0", ::dhcp::core::CpuMonitor::loadCore0(), true);
    addJsonInt(json, "cpu_load1", ::dhcp::core::CpuMonitor::loadCore1(), true);
    addJsonInt(json, "heap_free",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::freeHeap()), true);
    addJsonInt(json, "heap_total",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::totalHeap()), true);
    addJsonInt(json, "heap_largest",
               static_cast<int64_t>(::dhcp::core::CpuMonitor::largestBlock()), true);
    addJsonInt(json, "static_bindings_used",
               static_cast<int64_t>(::dhcp::core::Config::instance().staticBindingsBytes()), true);
    addJsonInt(json, "static_bindings_max",
               static_cast<int64_t>(::dhcp::core::Config::kMaxBindingsBytes), true);
    addJsonInt(json, "local_hosts_used",
               static_cast<int64_t>(::dhcp::core::Config::instance().localHostsBytes()), true);
    addJsonInt(json, "local_hosts_max",
               static_cast<int64_t>(::dhcp::core::Config::kMaxLocalHostsBytes), true);
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
    addJsonInt(json, "lease_count",
               s_dhcp ? static_cast<int64_t>(s_dhcp->leaseCount()) : 0, true);
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
// POST /api/ota/upload
// ─────────────────────────────────────────────────────

esp_err_t RestApi::handlePostOtaUpload(httpd_req* req)
{
    if (!checkAuth(req)) return ESP_OK;

    ESP_LOGI(TAG, "OTA update starting...");

    esp_ota_handle_t otaHandle = 0;
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_OK;
    }

    esp_err_t err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &otaHandle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    // Read and write firmware chunks
    char buf[1024];
    int remaining = req->content_len;
    bool success = true;

    while (remaining > 0) {
        int recvLen = httpd_req_recv(req, buf, std::min(remaining, (int)sizeof(buf)));
        if (recvLen <= 0) {
            success = false;
            break;
        }

        err = esp_ota_write(otaHandle, buf, recvLen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            success = false;
            break;
        }
        remaining -= recvLen;
    }

    if (success) {
        err = esp_ota_end(otaHandle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
            success = false;
        }
    }

    if (success) {
        err = esp_ota_set_boot_partition(partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
            success = false;
        }
    }

    if (success) {
        ESP_LOGI(TAG, "OTA update successful! Rebooting...");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Update successful. Rebooting...\"}");

        // Give the response time to be sent before reboot
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"OTA update failed\"}");
    }

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

} // namespace web
} // namespace dhcp
