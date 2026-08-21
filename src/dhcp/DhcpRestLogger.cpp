#include "DhcpRestLogger.h"
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_http_client.h"

static const char* TAG = "DhcpRest";

namespace dhcp {
namespace dhcp {

void DhcpRestLogger::setEnabled(bool enabled)
{
    enabled_ = enabled;
    updateSenderState();
}

void DhcpRestLogger::setUrl(const std::string& url)
{
    url_ = url;
    updateSenderState();
}

void DhcpRestLogger::setAuth(bool enabled, const std::string& user,
                             const std::string& pass)
{
    authEnabled_ = enabled;
    authUser_ = user;
    authPassword_ = pass;
}

void DhcpRestLogger::logEvent(const std::string& event,
                              const std::string& mac,
                              const std::string& ip,
                              const std::string& mask,
                              const std::string& gateway,
                              const std::string& dns,
                              int32_t leaseTime)
{
    if (!queue_ || !enabled_ || url_.empty()) return;

    Record rec;
    rec.leaseTime = leaseTime;
    strncpy(rec.event, event.c_str(), sizeof(rec.event) - 1);
    strncpy(rec.mac, mac.c_str(), sizeof(rec.mac) - 1);
    strncpy(rec.ip, ip.c_str(), sizeof(rec.ip) - 1);
    strncpy(rec.mask, mask.c_str(), sizeof(rec.mask) - 1);
    strncpy(rec.gateway, gateway.c_str(), sizeof(rec.gateway) - 1);
    strncpy(rec.dns, dns.c_str(), sizeof(rec.dns) - 1);

    // Ring buffer with overwrite: if full, drop the oldest and retry once.
    if (xQueueSendToBack(queue_, &rec, 0) != pdTRUE) {
        Record discard;
        if (xQueueReceive(queue_, &discard, 0) == pdTRUE) {
            xQueueSendToBack(queue_, &rec, 0);
        }
    }
}

void DhcpRestLogger::stopRestSender()
{
    if (!task_) return;
    stopRequested_ = true;
    Record marker;
    marker.stop = true;
    if (queue_) {
        xQueueSendToBack(queue_, &marker, pdMS_TO_TICKS(10));
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    while (task_ != nullptr && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    stopRequested_ = false;
}

void DhcpRestLogger::updateSenderState()
{
    const bool want = enabled_ && !url_.empty();
    if (want && !task_) {
        ensureSender();
    } else if (!want && task_) {
        stopRestSender();
    }
}

void DhcpRestLogger::ensureSender()
{
    if (queue_ == nullptr) {
        queue_ = xQueueCreate(kQueueDepth, sizeof(Record));
    }
    if (!queue_) {
        ESP_LOGE(TAG, "Failed to create DHCP REST queue");
        return;
    }
    stopRequested_ = false;
    if (xTaskCreate(&DhcpRestLogger::senderTask, "dhcp_rest",
                    kSenderStack, this, kSenderPriority,
                    &task_) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create DHCP REST sender task");
    }
}

void DhcpRestLogger::senderTask(void* arg)
{
    auto* self = static_cast<DhcpRestLogger*>(arg);
    Record rec;
    while (!self->stopRequested_) {
        if (xQueueReceive(self->queue_, &rec, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (rec.stop) break;
            self->sendRecord(rec);
        }
    }
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

void DhcpRestLogger::sendRecord(const Record& rec)
{
    if (url_.empty()) return;

    const std::string payload = buildJson(rec);

    esp_http_client_config_t cfg = {};
    cfg.url = url_.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kSendTimeoutMs;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    // Do NOT follow 3xx redirects automatically (redirect loops used to end
    // in ESP_ERR_HTTP_MAX_REDIRECT); the 3xx status is logged as a WARN below.
    cfg.disable_auto_redirect = true;
    if (authEnabled_ && !authUser_.empty()) {
        cfg.username = authUser_.c_str();
        cfg.password = authPassword_.c_str();
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
    // Do NOT set skip_cert_common_name_check — that disables SNI and breaks
    // Apache name-based vhosts (421). Self-signed certs are accepted via
    // CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY.

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "DHCP REST INIT FAILED");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // Tell Laravel the client expects JSON so an unauthenticated API request
    // returns 401 JSON instead of a 302 redirect to login (redirect loop).
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, payload.c_str(),
                                   static_cast<int>(payload.size()));

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        // Report the HTTP status too (e.g. 401) so an auth problem is visible.
        const int hstatus = esp_http_client_get_status_code(client);
        ESP_LOGW(TAG, "DHCP REST send FAILED (%s): http=%d %s",
                 esp_err_to_name(err), hstatus, url_.c_str());
    } else {
        const int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "DHCP REST log sent: %s %s", rec.event, rec.mac);
        } else {
            ESP_LOGW(TAG, "DHCP REST send HTTP %d: event=%s mac=%s",
                     status, rec.event, rec.mac);
        }
    }
    esp_http_client_cleanup(client);
}

std::string DhcpRestLogger::buildJson(const Record& rec) const
{
    // Matches the server contract in Plan/ServerPrompt.md:
    // {event, mac, ip, mask, gateway, dns, lease_time}
    std::string json = "{\"event\":\"";
    json += rec.event;
    json += "\",\"mac\":\"";
    json += rec.mac;
    json += "\",\"ip\":\"";
    json += rec.ip;
    json += "\",\"mask\":\"";
    json += rec.mask;
    json += "\",\"gateway\":\"";
    json += rec.gateway;
    json += "\",\"dns\":\"";
    json += rec.dns;
    json += "\",\"lease_time\":";
    json += std::to_string(rec.leaseTime);
    json += "}";
    return json;
}

} // namespace dhcp
} // namespace dhcp
