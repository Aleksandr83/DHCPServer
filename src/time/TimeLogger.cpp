#include "TimeLogger.h"
#include "core/ErrorLog.h"
#include "core/RestSenderLimits.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

using namespace std;

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

void TimeLogger::setLogUrl(const string& url)
{
    logUrl_ = url;
    updateRestSenderState();
}

void TimeLogger::setLogAuth(bool enabled, const string& user,
                            const string& pass)
{
    logAuthEnabled_ = enabled;
    logAuthUser_ = user;
    logAuthPassword_ = pass;
}

void TimeLogger::logRequest(const string& clientAddr, uint8_t stratum, bool clockVerified)
{
    // The terminal and the REST feed count *served* requests: a reply that said
    // "the clock is not trustworthy" was never one of them, and a client of the
    // REST API would have to filter it out again (the behaviour is unchanged).
    if (clockVerified && logTerminal_) {
        ESP_LOGI(TAG, "NTP request from %s -> stratum %u",
                 clientAddr.c_str(), static_cast<unsigned>(stratum));
    }
    if (clockVerified && logRest_ && !logUrl_.empty() && restQueue_) {
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
    if (logFile_ && fileQueue_) {
        // Only the address, the stratum and the verdict are queued: the stamp is
        // made when the line is written, so a queue that waits says nothing wrong
        // about time.
        FileLogRecord rec;
        rec.stratum = stratum;
        rec.clockVerified = clockVerified;
        strncpy(rec.text, clientAddr.c_str(), sizeof(rec.text) - 1);
        if (xQueueSendToBack(fileQueue_, &rec, 0) != pdTRUE) {
            FileLogRecord discard;
            if (xQueueReceive(fileQueue_, &discard, 0) == pdTRUE) {
                xQueueSendToBack(fileQueue_, &rec, 0);
            }
        }
    }
}

void TimeLogger::logEvent(const string& text)
{
    if (logTerminal_) {
        ESP_LOGI(TAG, "NTP %s", text.c_str());
    }
    if (!logFile_ || !fileQueue_) return;

    // The sentence is clamped, not dropped: a truncated line still says what
    // happened, a missing one says nothing.
    FileLogRecord rec;
    rec.isEvent = true;
    strncpy(rec.text, text.c_str(), sizeof(rec.text) - 1);
    if (xQueueSendToBack(fileQueue_, &rec, 0) != pdTRUE) {
        FileLogRecord discard;
        if (xQueueReceive(fileQueue_, &discard, 0) == pdTRUE) {
            xQueueSendToBack(fileQueue_, &rec, 0);
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
        xQueueSendToBack(restQueue_, &marker, pdMS_TO_TICKS(core::kStopMarkerPollMs));
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(core::kDrainDeadlineMs);
    while (restTask_ != nullptr && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(core::kStopMarkerPollMs));
    }
    restStopRequested_ = false;
}

void TimeLogger::restSenderTask(void* arg)
{
    auto* self = static_cast<TimeLogger*>(arg);
    RestLogRecord rec;
    while (!self->restStopRequested_) {
        if (xQueueReceive(self->restQueue_, &rec, pdMS_TO_TICKS(core::kQueueWaitMs)) == pdTRUE) {
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

    const string& url = logUrl_;
    const string payload = buildRestJson(rec);

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = core::kSendTimeoutMs;
    cfg.buffer_size = core::kHttpClientBufferBytes;
    cfg.buffer_size_tx = core::kHttpClientBufferBytes;
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

void TimeLogger::setLogFile(bool enabled)
{
    logFile_ = enabled;
    updateFileLogState();
}

void TimeLogger::setFileLogVolumes(const string& fatMountPoint,
                                   const string& cardMountPoint,
                                   CardMountedCheck cardMounted)
{
    fatMountPoint_ = fatMountPoint;
    cardMountPoint_ = cardMountPoint;
    cardMounted_ = move(cardMounted);
    fileVolumesReady_ = !fatMountPoint_.empty();
    updateFileLogState();
}

void TimeLogger::updateFileLogState()
{
    const bool want = logFile_ && fileVolumesReady_;
    if (want && !fileTask_) {
        ensureFileWriter();
    } else if (!want && fileTask_) {
        stopFileWriter();
    }
}

void TimeLogger::ensureFileWriter()
{
    // The live file goes on the internal volume and a full one is archived to the
    // card; the failures of that archive are the operator's business, so they are
    // written into Errors.log — which is the file he reads when something is
    // wrong — and not only into the terminal.
    fileWriter_ = make_unique<core::NtpLogWriter>(
        fatMountPoint_ + "/" + core::kNtpLogFolder + "/" + core::kNtpLogFileName,
        cardMountPoint_,
        [this] { return cardMounted_ && cardMounted_(); },
        [](const string& message) {
            core::ErrorLog::instance().errorf("TimeLogger", "%s", message.c_str());
        });
    if (fileQueue_ == nullptr) {
        fileQueue_ = xQueueCreate(kFileQueueDepth, sizeof(FileLogRecord));
    }
    if (!fileQueue_) {
        ESP_LOGE(TAG, "Failed to create file log queue");
        return;
    }
    fileStopRequested_ = false;
    if (xTaskCreate(&TimeLogger::fileLogTask, "ntp_file", kFileTaskStack, this,
                    kFileTaskPriority, &fileTask_) != pdPASS) {
        fileTask_ = nullptr;
        ESP_LOGE(TAG, "Failed to create file log task");
    }
}

void TimeLogger::stopFileWriter()
{
    if (fileTask_) {
        fileStopRequested_ = true;
        FileLogRecord marker;
        marker.stop = true;
        if (fileQueue_) {
            xQueueSendToBack(fileQueue_, &marker, pdMS_TO_TICKS(core::kStopMarkerPollMs));
        }
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(core::kDrainDeadlineMs);
        while (fileTask_ != nullptr && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(core::kStopMarkerPollMs));
        }
        fileStopRequested_ = false;
    }
    fileWriter_.reset();
}

void TimeLogger::fileLogTask(void* arg)
{
    auto* self = static_cast<TimeLogger*>(arg);
    FileLogRecord rec;
    while (!self->fileStopRequested_) {
        if (xQueueReceive(self->fileQueue_, &rec, pdMS_TO_TICKS(core::kQueueWaitMs)) == pdTRUE) {
            if (rec.stop) break;
            self->writeFileLog(rec);
        }
    }
    self->fileTask_ = nullptr;
    vTaskDelete(nullptr);
}

void TimeLogger::writeFileLog(const FileLogRecord& rec)
{
    if (!fileWriter_) return;

    // The line's stamp is the device's LOCAL time — the same clock the pages,
    // Errors.log and a sweep's log file show, because applying the time settings
    // points the C library at the operator's zone (stage 186). It is read here,
    // in the writing task, and not in the NTP task: the line should say when it
    // was written.
    // `std::` is spelled out for `time`/`tm` on purpose: this file lives in the
    // `dhcp::time` namespace, which is exactly the name the C library needs
    // (rule 40 names this collision).
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    const bool clockSet = core::ErrorLogCore::clockIsSet(static_cast<uint64_t>(now)) &&
                          core::ErrorLogCore::localTime(now, local);
    const core::NtpLogStamp stamp = core::ntpLogStampFromLocal(local, clockSet);

    const std::string line = rec.isEvent
        ? core::ntpLogEventLine(stamp, rec.text)
        : core::ntpLogLine(stamp, rec.text, rec.stratum, rec.clockVerified);

    if (fileWriter_->append(line, stamp)) {
        writeFailures_ = 0;
        return;
    }

    // The volume refused the line. It is not repeated here — the next request
    // will try again on its own — but a file log that silently stops is worse
    // than a noisy one, so the failure is named on the terminal at a rate that
    // cannot bury anything else.
    ++writeFailures_;
    if (writeFailures_ == 1 || (writeFailures_ % kWriteFailureWarnEvery) == 0) {
        ESP_LOGW(TAG, "NTP file log write failed (%u in a row): %s",
                 static_cast<unsigned>(writeFailures_), fatMountPoint_.c_str());
    }
}

string TimeLogger::buildRestJson(const RestLogRecord& rec) const
{
    string json = "{\"client_ip\":\"";
    json += rec.client;
    json += "\",\"stratum\":";
    json += to_string(static_cast<unsigned>(rec.stratum));
    json += ",\"ts_ms\":";
    json += to_string(rec.ts);
    json += "}";
    return json;
}

} // namespace time
} // namespace dhcp
