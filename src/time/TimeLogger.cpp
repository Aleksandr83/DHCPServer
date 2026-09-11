#include "TimeLogger.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* TAG = "TimeLogger";

namespace {
// Capture the HTTP response body (first bytes) for diagnostics.
struct RestRespCapture {
    char buf[256] = {0};
    size_t len = 0;
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
    }
    return ESP_OK;
}
} // namespace

namespace dhcp {
namespace time {

TimeLogger::TimeLogger()
{
}

void TimeLogger::setLogRest(bool enabled)
{
    logRest_ = enabled;
    updateRestSenderState();
}

void TimeLogger::setLogUrl(const std::string& url)
{
    logUrl_ = url;
    updateRestSenderState();
}

void TimeLogger::setLogAuth(bool enabled, const std::string& user,
                            const std::string& pass)
{
    logAuthEnabled_ = enabled;
    logAuthUser_ = user;
    logAuthPassword_ = pass;
}

void TimeLogger::logRequest(const std::string& clientAddr, uint8_t stratum)
{
    if (logTerminal_) {
        ESP_LOGI(TAG, "NTP request from %s -> stratum %u",
                 clientAddr.c_str(), static_cast<unsigned>(stratum));
    }
    if (logRest_ && !logUrl_.empty() && restQueue_) {
        RestLogRecord rec;
        rec.ts = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        rec.stratum = stratum;
        strncpy(rec.client, clientAddr.c_str(), sizeof(rec.client) - 1);
        if (xQueueSendToBack(restQueue_, &rec, 0) != pdTRUE) {
            // Queue full -> drop the oldest record, then retry once.
            RestLogRecord discard;
            if (xQueueReceive(restQueue_, &discard, 0) == pdTRUE) {
                xQueueSendToBack(restQueue_, &rec, 0);
            }
        }
    }
}

// ─── Async REST sender ──────────────────────────────

void TimeLogger::updateRestSenderState()
{
    const bool want = logRest_ && !logUrl_.empty();
    if (want && !restTask_) {
        ensureRestSender();
    } else if (!want && restTask_) {
        stopRestSender();
    }
}

void TimeLogger::ensureRestSender()
{
    if (restQueue_ == nullptr) {
        restQueue_ = xQueueCreate(kRestQueueDepth, sizeof(RestLogRecord));
    }
    if (!restQueue_) {
        ESP_LOGE(TAG, "Failed to create REST log queue");
        return;
    }
    restStopRequested_ = false;
    if (xTaskCreate(&TimeLogger::restSenderTask, "ntp_rest",
                    kRestSenderStack, this, kRestSenderPriority,
                    &restTask_) != pdPASS) {
        restTask_ = nullptr;
        ESP_LOGE(TAG, "Failed to create REST sender task");
    }
}

void TimeLogger::stopRestSender()
{
    if (!restTask_) return;
    restStopRequested_ = true;
    RestLogRecord marker;
    marker.stop = true;
    if (restQueue_) {
        xQueueSendToBack(restQueue_, &marker, pdMS_TO_TICKS(10));
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    while (restTask_ != nullptr && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    restStopRequested_ = false;
}

void TimeLogger::restSenderTask(void* arg)
{
    auto* self = static_cast<TimeLogger*>(arg);
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

void TimeLogger::sendRestLog(const RestLogRecord& rec)
{
    if (logUrl_.empty()) return;

    const std::string& url = logUrl_;
    const std::string payload = buildRestJson(rec);

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kRestSendTimeoutMs;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    // Do NOT follow 3xx redirects automatically (avoids redirect loops).
    cfg.disable_auto_redirect = true;
    if (logAuthEnabled_ && !logAuthUser_.empty()) {
        cfg.username = logAuthUser_.c_str();
        cfg.password = logAuthPassword_.c_str();
        // Send Basic auth preemptively (avoids the 401 retry loop).
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    cfg.max_authorization_retries = -1;

    RestRespCapture respCap;
    cfg.event_handler = &restRespHandler;
    cfg.user_data = &respCap;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "NTP REST send INIT FAILED");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, payload.c_str(),
                                   static_cast<int>(payload.size()));

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NTP REST send FAILED (%s): http=%d client=%s url=%s",
                 esp_err_to_name(err), esp_http_client_get_status_code(client),
                 rec.client, url.c_str());
    } else {
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "NTP REST send HTTP %d: client=%s body=\"%s\"",
                     status, rec.client, respCap.buf);
        } else if (logTerminal_) {
            ESP_LOGI(TAG, "NTP REST send OK: client=%s status=%d",
                     rec.client, status);
        }
    }
    esp_http_client_cleanup(client);
}

std::string TimeLogger::buildRestJson(const RestLogRecord& rec) const
{
    std::string json = "{\"client_ip\":\"";
    json += rec.client;
    json += "\",\"stratum\":";
    json += std::to_string(static_cast<unsigned>(rec.stratum));
    json += ",\"ts_ms\":";
    json += std::to_string(rec.ts);
    json += "}";
    return json;
}

} // namespace time
} // namespace dhcp
