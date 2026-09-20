#include "CacheAutosave.h"

#include "core/JobRegistry.h"
#include "time/TimeMath.h"

#include <ctime>
#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace std;

namespace dhcp {
namespace dns {

namespace {

const char* TAG = "CacheAutosave";

// Rule 39: the tick of the countdown, the identity this operation uses in the
// job registry (the page translates `jobs.cache_autosave`), and the two numbers
// of the "clock is set" test.
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kTaskStackBytes = 4096;   // one write and a log line
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
const char* kJobId = "cache_autosave";
const char* kJobTitleKey = "jobs.cache_autosave";
// 2020-01-01 00:00:00 UTC: a device that has never been set starts at 1970, and
// the CPU counts seconds from boot, so anything before this is "not set".
constexpr int64_t kClockSetSinceEpoch = 1577836800;
// While the clock is unset the task wakes up every tick and does nothing; the
// reason is logged once a minute rather than sixty times.
constexpr uint32_t kClockWaitLogEveryTicks = 60;

// Progress of the write, straight into the registry: the scheduler page shows
// the percentage while the save runs.
void reportProgress(unsigned long done, unsigned long total, void*)
{
    core::JobRegistry::instance().progress(kJobId, static_cast<uint32_t>(done),
                                           static_cast<uint32_t>(total));
}

} // namespace

CacheAutosave::CacheAutosave(InternalDnsCache& cache, string path)
    : cache_(cache), path_(move(path))
{
}

CacheAutosave::~CacheAutosave()
{
    enabled_ = false;
    // Let the task see the flag and leave; it sleeps a tick at most.
    for (int i = 0; i < 5 && task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs / 10));
    }
}

bool CacheAutosave::clockIsSet()
{
    return static_cast<int64_t>(std::time(nullptr)) >= kClockSetSinceEpoch;
}

uint16_t CacheAutosave::currentMonthDays()
{
    if (!clockIsSet()) return 0;             // unknown: the clamp uses 31
    const time_t now = std::time(nullptr);
    tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &now);                    // the host test builds under MinGW
#else
    gmtime_r(&now, &tmv);
#endif
    return static_cast<uint16_t>(
        time::TimeMath::daysInMonth(tmv.tm_year + 1900, tmv.tm_mon + 1));
}

uint32_t CacheAutosave::effectivePeriodSec() const
{
    const uint16_t days = currentMonthDays();
    return core::autosavePeriodSec(
        unit_, core::autosaveClampInterval(unit_, interval_, days));
}

void CacheAutosave::configure(bool enabled, core::AutosavePeriod unit, uint16_t interval)
{
    unit_ = unit;
    interval_ = interval;

    if (!enabled) {
        enabled_ = false;
        ESP_LOGI(TAG, "autosave off");
        return;
    }

    // Turning it on, or changing the period, starts the countdown from a whole
    // period: the operator asked for "every hour", not "an hour from whenever
    // the last one happened to be".
    periodSec_ = effectivePeriodSec();
    remainingSec_ = periodSec_;
    restartRequested_ = true;
    enabled_ = true;

    if (task_ == nullptr) {
        TaskHandle_t handle = nullptr;
        if (xTaskCreate(taskEntry, "cache_save", kTaskStackBytes, this,
                        kTaskPriority, &handle) != pdPASS) {
            enabled_ = false;
            ESP_LOGE(TAG, "cannot start the autosave task");
            return;
        }
        task_ = handle;
    }
    ESP_LOGI(TAG, "autosave every %u s", static_cast<unsigned>(periodSec_));
}

void CacheAutosave::notifyManualSave()
{
    if (!enabled_) return;
    periodSec_ = effectivePeriodSec();
    remainingSec_ = periodSec_;
}

void CacheAutosave::taskEntry(void* arg)
{
    static_cast<CacheAutosave*>(arg)->run();
    vTaskDelete(nullptr);
}

void CacheAutosave::run()
{
    uint32_t clockWaitTicks = 0;
    bool wasSet = true;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));

        if (!enabled_) {
            task_ = nullptr;
            return;              // stop(); configure(true, …) starts a new task
        }

        // Suspended while the clock is not set: no countdown, no write. The
        // period is recomputed once the clock arrives, because the day bound of
        // the interval depends on the month that is running.
        if (!clockIsSet()) {
            wasSet = false;
            if (clockWaitTicks++ % kClockWaitLogEveryTicks == 0) {
                ESP_LOGW(TAG, "autosave waiting for the clock to be set");
            }
            continue;
        }
        if (!wasSet) {
            wasSet = true;
            periodSec_ = effectivePeriodSec();
            remainingSec_ = periodSec_;
            ESP_LOGI(TAG, "clock is set: autosave every %u s",
                     static_cast<unsigned>(periodSec_));
        }

        if (restartRequested_) {
            restartRequested_ = false;
            periodSec_ = effectivePeriodSec();
            remainingSec_ = periodSec_;
            continue;
        }

        if (remainingSec_ > 0) {
            remainingSec_ -= kTickMs / 1000;
            continue;
        }

        saveNow();
        periodSec_ = effectivePeriodSec();   // the month may have changed
        remainingSec_ = periodSec_;
    }
}

void CacheAutosave::saveNow()
{
    // Nothing to save into, or nothing to save: say nothing and wait for the
    // next period rather than hammering a cache that is switched off.
    if (!cache_.available()) return;

    core::JobRegistry& jobs = core::JobRegistry::instance();
    // The total is unknown (0), so the page shows the row without a percentage
    // instead of inventing one.
    if (!jobs.begin(kJobId, kJobTitleKey, path_, 0)) {
        ESP_LOGW(TAG, "job registry is full, skipping this save");
        return;
    }

    size_t entries = 0;
    const bool ok = cache_.saveToFile(path_.c_str(), &entries, reportProgress);

    if (ok) {
        jobs.finish(kJobId, core::JobState::Done,
                    to_string(entries) + " entries");
        ESP_LOGI(TAG, "autosave wrote %u entries",
                 static_cast<unsigned>(entries));
    } else {
        jobs.finish(kJobId, core::JobState::Failed, "write failed");
        ESP_LOGW(TAG, "autosave failed");
    }

    // A stop pressed on the scheduler page turns the autosave off, as decided:
    // the request is only recorded by the registry, the owner acts on it.
    if (jobs.cancelRequested(kJobId)) {
        jobs.finish(kJobId, core::JobState::Cancelled, "stopped by the operator");
        enabled_ = false;
        ESP_LOGI(TAG, "autosave stopped from the scheduler page");
        if (onDisabled_) onDisabled_();
    }
}

} // namespace dns
} // namespace dhcp
