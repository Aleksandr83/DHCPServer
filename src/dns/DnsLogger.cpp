#include "DnsLogger.h"
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* TAG = "DnsLogger";

// Capture the HTTP response body (first bytes) so a non-2xx reply (e.g.
// Laravel's 400/422 JSON error) can be logged for diagnostics.
namespace {
struct RestRespCapture {
    char buf[256] = {0};
    size_t len = 0;
    char location[256] = {0};
};

esp_err_t restRespHandler(esp_http_client_event_t* evt)
{
    auto* cap = static_cast<RestRespCapture*>(evt->user_data);
    if (!cap) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        const size_t space = sizeof(cap->buf) - 1 - cap->len;
        const size_t n = (static_cast<size_t>(evt->data_len) < space)
                             ? static_cast<size_t>(evt->data_len) : space;
        if (n > 0) {
            memcpy(cap->buf + cap->len, evt->data, n);
            cap->len += n;
            cap->buf[cap->len] = '\0';
        }
    } else if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
               evt->header_value) {
        // Capture the redirect target for diagnostics (Location header).
        if (strncasecmp(evt->header_key, "Location", 8) == 0) {
            strncpy(cap->location, evt->header_value,
                    sizeof(cap->location) - 1);
        }
    }
    return ESP_OK;
}
} // namespace

namespace dhcp {
namespace dns {

DnsLogger::DnsLogger()
{
}

void DnsLogger::setLogTerminal(bool enabled)
{
    logTerminal_ = enabled;
}

void DnsLogger::setLogForwarded(bool enabled)
{
    logForwarded_ = enabled;
}

void DnsLogger::setLogLocal(bool enabled)
{
    logLocal_ = enabled;
}

void DnsLogger::setLogCache(bool enabled)
{
    logCache_ = enabled;
}

void DnsLogger::setLogRestSent(bool enabled)
{
    logRestSent_ = enabled;
}

void DnsLogger::setLogRest(bool enabled)
{
    logRest_ = enabled;
    updateRestSenderState();
}

void DnsLogger::setLogUrl(const std::string& url)
{
    logUrl_ = url;
    updateRestSenderState();
}

void DnsLogger::setLogAuth(bool enabled, const std::string& user,
                           const std::string& pass)
{
    logAuthEnabled_ = enabled;
    logAuthUser_ = user;
    logAuthPassword_ = pass;
}

void DnsLogger::setLocalHosts(
    const std::map<std::string, std::vector<std::string>>* hosts)
{
    localHosts_ = hosts;
}

void DnsLogger::logQuery(const std::string& domain, uint16_t type,
                          const std::string& clientAddr,
                          const std::string& clientMac,
                          DnsLogSource source,
                          bool resolved, const std::string& answer)
{
    if (logTerminal_) {
        bool allowed = (source == DnsLogSource::LOCAL && logLocal_) ||
                       (source == DnsLogSource::CACHE && logCache_) ||
                       (source == DnsLogSource::FORWARDED && logForwarded_);
        // "Sent to REST" filter: only show queries that were also sent to
        // the external REST log service.
        if (logRestSent_) {
            allowed = allowed && logRest_ && !logUrl_.empty();
        }
        if (allowed) {
            logToTerminal(domain, type, clientAddr, clientMac,
                          source, resolved, answer);
        }
    }
    if (logRest_ && !logUrl_.empty()) {
        logToRest(domain, type, clientAddr, clientMac,
                  source, resolved, answer);
    }
}

void DnsLogger::logToTerminal(const std::string& domain, uint16_t type,
                               const std::string& clientAddr,
                               const std::string& clientMac,
                               DnsLogSource source,
                               bool resolved, const std::string& answer)
{
    const char* tag = (source == DnsLogSource::LOCAL) ? "local" :
                      (source == DnsLogSource::CACHE) ? "cache" : "forward";

    const char* typeStr = (type == 1) ? "A" :
                          (type == 28) ? "AAAA" :
                          (type == 15) ? "MX" :
                          (type == 5) ? "CNAME" : "OTHER";

    if (resolved) {
        ESP_LOGI(TAG, "DNS: [%s] %s %s -> %s (from %s mac=%s)",
                 tag, typeStr, domain.c_str(), answer.c_str(),
                 clientAddr.c_str(), clientMac.c_str());
    } else {
        ESP_LOGI(TAG, "DNS: [%s] %s %s -> NXDOMAIN (from %s mac=%s)",
                 tag, typeStr, domain.c_str(), clientAddr.c_str(),
                 clientMac.c_str());
    }
}

void DnsLogger::logToRest(const std::string& domain, uint16_t type,
                           const std::string& clientAddr,
                           const std::string& clientMac,
                           DnsLogSource source,
                           bool resolved, const std::string& answer)
{
    if (!restQueue_) return;

    // Build a fixed-size record (no heap allocation in the DNS task) and push
    // it into the bounded ring buffer. On overflow the oldest record is
    // overwritten (ring buffer semantics) so the DNS task never blocks.
    RestLogRecord rec;
    rec.type = type;
    rec.resolved = resolved;
    rec.source = static_cast<uint8_t>(source);
    rec.ts = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    strncpy(rec.domain, domain.c_str(), sizeof(rec.domain) - 1);
    strncpy(rec.client, clientAddr.c_str(), sizeof(rec.client) - 1);
    strncpy(rec.mac, clientMac.c_str(), sizeof(rec.mac) - 1);
    strncpy(rec.answer, answer.c_str(), sizeof(rec.answer) - 1);

    if (xQueueSendToBack(restQueue_, &rec, 0) != pdTRUE) {
        // Queue full -> drop the oldest record, then retry once.
        RestLogRecord discard;
        if (xQueueReceive(restQueue_, &discard, 0) == pdTRUE) {
            xQueueSendToBack(restQueue_, &rec, 0);
        }
        ++restDropped_;
        // Log at WARN only occasionally so a flooded server doesn't spam.
        if ((restDropped_ % 50) == 1) {
            ESP_LOGW(TAG, "REST log queue full, oldest dropped (%u total)",
                     static_cast<unsigned>(restDropped_));
        }
    }
}

// ─── Async REST sender ──────────────────────────────

void DnsLogger::updateRestSenderState()
{
    const bool want = logRest_ && !logUrl_.empty();
    if (want && !restTask_) {
        ensureRestSender();
    } else if (!want && restTask_) {
        stopRestSender();
    }
}

void DnsLogger::ensureRestSender()
{
    if (restQueue_ == nullptr) {
        restQueue_ = xQueueCreate(kRestQueueDepth, sizeof(RestLogRecord));
    }
    if (!restQueue_) {
        ESP_LOGE(TAG, "Failed to create REST log queue");
        return;
    }
    restStopRequested_ = false;
    if (xTaskCreate(&DnsLogger::restSenderTask, "dns_rest",
                    kRestSenderStack, this, kRestSenderPriority,
                    &restTask_) != pdPASS) {
        restTask_ = nullptr;
        ESP_LOGE(TAG, "Failed to create REST sender task");
    }
}

void DnsLogger::stopRestSender()
{
    if (!restTask_) return;
    restStopRequested_ = true;
    // Wake the task if it is blocked on the queue.
    RestLogRecord marker;
    marker.stop = true;
    if (restQueue_) {
        xQueueSendToBack(restQueue_, &marker, pdMS_TO_TICKS(10));
    }
    // Wait for the sender to exit (an in-flight POST may take up to the
    // timeout). The queue is kept allocated to avoid a use-after-free.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    while (restTask_ != nullptr && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    restStopRequested_ = false;
}

void DnsLogger::restSenderTask(void* arg)
{
    auto* self = static_cast<DnsLogger*>(arg);
    RestLogRecord rec;
    while (!self->restStopRequested_) {
        if (xQueueReceive(self->restQueue_, &rec, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (rec.stop) break;
            self->sendRestLog(rec);
        }
    }
    self->restTask_ = nullptr;
    vTaskDelete(nullptr);
}

// Rewrite the host part of `url` to the IP from the local hosts list, so a
// ".lo" style name (known only by the built-in DNS server for LAN clients)
// can be reached without relying on the system DNS resolver (which would
// fail with EAI_NONAME). Unchanged if the host is not a local host.
std::string DnsLogger::resolveUrlHost(const std::string& url) const
{
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return url;

    const size_t hostStart = schemeEnd + 3;
    const size_t hostEnd = url.find_first_of("/:", hostStart);
    const size_t hostLen =
        (hostEnd == std::string::npos) ? (url.length() - hostStart) : (hostEnd - hostStart);
    const std::string host = url.substr(hostStart, hostLen);
    if (host.empty() || !localHosts_) return url;

    const auto it = localHosts_->find(host);
    if (it == localHosts_->end()) return url;

    // Prefer an IPv4 address if present.
    std::string ip;
    for (const auto& addr : it->second) {
        if (addr.find(':') == std::string::npos) { ip = addr; break; }
    }
    if (ip.empty() && !it->second.empty()) ip = it->second.front();
    if (ip.empty()) return url;

    std::string rewritten = url.substr(0, hostStart) + ip;
    if (hostEnd != std::string::npos) rewritten += url.substr(hostEnd);
    ESP_LOGI(TAG, "REST URL host '%s' resolved to %s", host.c_str(), ip.c_str());
    return rewritten;
}

void DnsLogger::sendRestLog(const RestLogRecord& rec)
{
    if (logUrl_.empty()) return;

    // Keep the URL hostname as-is: the system DNS now points to the built-in
    // DNS server, so ".lo" names resolve and the Host header stays correct
    // for Apache name-based vhosts.
    const std::string& url = logUrl_;
    const std::string payload = buildRestJson(rec);

    // "Sent to REST" checkbox (logRestSent_) gates the detailed per-send
    // terminal logging; it only applies when terminal logging is on.
    if (logTerminal_ && logRestSent_) {
        ESP_LOGI(TAG, "REST send: POST %s domain=%s type=%u auth=%s",
                 url.c_str(), rec.domain, rec.type,
                 (logAuthEnabled_ && !logAuthUser_.empty()) ? "on" : "off");
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kRestSendTimeoutMs;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    // Do NOT follow 3xx redirects automatically (redirect loops used to end
    // in ESP_ERR_HTTP_MAX_REDIRECT after 10 hops); the 3xx status is logged
    // below as a WARN together with the Location header.
    cfg.disable_auto_redirect = true;
    if (logAuthEnabled_ && !logAuthUser_.empty()) {
        cfg.username = logAuthUser_.c_str();
        cfg.password = logAuthPassword_.c_str();
        // Send Basic auth preemptively. Without auth_type the first request
        // carries NO Authorization header, the server answers 401, and
        // esp_http_client_add_auth() retries up to 10 times (each incrementing
        // redirect_counter) until ESP_ERR_HTTP_MAX_REDIRECT fires — the same
        // misleading error as a 3xx redirect loop.
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    // Disable the 401 retry loop entirely: a single 401 is returned as a
    // status code (logged below), instead of 10 auth retries ending in
    // ESP_ERR_HTTP_MAX_REDIRECT.
    cfg.max_authorization_retries = -1;
    // Self-signed server certificates are accepted via
    // CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY (sets MBEDTLS_SSL_VERIFY_NONE),
    // so skip_cert_common_name_check must NOT be set — that flag disables
    // SNI entirely, and Apache then can't route to the correct name-based
    // vhost (it returns 421 Misdirected Request). SNI (the URL hostname) is
    // sent automatically from the URL.

    // Capture the response body for diagnostics (e.g. why the server
    // returned 400/422).
    RestRespCapture respCap;
    cfg.event_handler = &restRespHandler;
    cfg.user_data = &respCap;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "REST send INIT FAILED (cannot allocate client)");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // Tell Laravel the client expects JSON so an unauthenticated API request
    // returns 401 JSON instead of a 302 redirect to login (redirect loop).
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, payload.c_str(),
                                   static_cast<int>(payload.size()));

    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = esp_http_client_perform(client);
    const int64_t elapsedMs = (esp_timer_get_time() - t0) / 1000;

    if (err != ESP_OK) {
        // Failures are always reported (WARN) so problems are never hidden.
        // Report the HTTP status too (e.g. 401) so an auth problem is visible.
        const int hstatus = esp_http_client_get_status_code(client);
        ESP_LOGW(TAG, "REST send FAILED (%s): http=%d domain=%s url=%s (%lld ms)",
                 esp_err_to_name(err), hstatus, rec.domain, url.c_str(),
                 static_cast<long long>(elapsedMs));
    } else {
        const int status = esp_http_client_get_status_code(client);
        const int64_t contentLen =
            esp_http_client_get_content_length(client);
        if (status >= 200 && status < 300) {
            if (logTerminal_ && logRestSent_) {
                ESP_LOGI(TAG, "REST send OK: domain=%s type=%u status=%d resp=%lld bytes body=\"%s\" (%lld ms)",
                         rec.domain, rec.type, status,
                         static_cast<long long>(contentLen), respCap.buf,
                         static_cast<long long>(elapsedMs));
            }
        } else {
            // 3xx (redirect) or non-2xx — surface it loudly with Location.
            ESP_LOGW(TAG, "REST send HTTP %d: domain=%s type=%u location=%s body=\"%s\" (%lld ms)",
                     status, rec.domain, rec.type,
                     respCap.location[0] ? respCap.location : "-",
                     respCap.buf,
                     static_cast<long long>(elapsedMs));
        }
    }
    esp_http_client_cleanup(client);
}

std::string DnsLogger::buildRestJson(const RestLogRecord& rec) const
{
    const char* src = (rec.source == 0) ? "local" :
                      (rec.source == 1) ? "cache" : "forward";
    std::string json = "{\"domain\":\"";
    json += rec.domain;
    json += "\",\"type\":";
    json += std::to_string(rec.type);
    json += ",\"client_ip\":\"";
    json += rec.client;
    json += "\",\"client_mac\":\"";
    json += rec.mac;
    json += "\",\"source\":\"";
    json += src;
    json += "\",\"resolved\":";
    json += rec.resolved ? "true" : "false";
    json += ",\"answer\":\"";
    json += rec.answer;
    json += "\"}";
    return json;
}

} // namespace dns
} // namespace dhcp
