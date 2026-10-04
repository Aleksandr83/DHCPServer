#include "CacheAutoUpdate.h"

#include "core/AutoUpdatePlan.h"
#include "core/JobRegistry.h"

#include <ctime>
#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace std;

namespace dhcp {
namespace dns {

namespace {

const char* TAG = "CacheAutoUpdate";

// Rule 39: the tick of the countdown, the identity this operation uses in the
// job registry (the page translates `jobs.cache_autoupdate`), the most records
// that may wait to be retried, and the two numbers of the "clock is set" test.
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kTaskStackBytes = 8192;   // one upstream query with its buffers
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
const char* kJobId = "cache_autoupdate";
const char* kJobTitleKey = "jobs.cache_autoupdate";
// Bounded on purpose: the list exists to retry a record sooner, not to grow for
// every name that stops resolving.
constexpr size_t kMaxPending = 256;
// 2020-01-01 00:00:00 UTC: a device that has never been set starts at 1970, and
// the CPU counts seconds from boot, so anything before this is "not set".
constexpr int64_t kClockSetSinceEpoch = 1577836800;
// While the clock is unset the task wakes up every tick and does nothing; the
// reason is logged once a minute rather than sixty times.
constexpr uint32_t kClockWaitLogEveryTicks = 60;

} // namespace

CacheAutoUpdate::CacheAutoUpdate(InternalDnsCache& cache)
    : cache_(cache)
{
}

CacheAutoUpdate::~CacheAutoUpdate()
{
    enabled_ = false;
    // Let the task see the flag and leave; it sleeps a tick at most.
    for (int i = 0; i < 5 && task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs / 10));
    }
}

bool CacheAutoUpdate::clockIsSet()
{
    return static_cast<int64_t>(std::time(nullptr)) >= kClockSetSinceEpoch;
}

uint32_t CacheAutoUpdate::effectivePeriodSec() const
{
    return core::autosavePeriodSec(
        unit_, core::autoUpdateClampInterval(unit_, interval_));
}

void CacheAutoUpdate::configure(bool enabled, core::AutosavePeriod unit,
                                uint16_t interval, uint16_t batch,
                                uint16_t pauseSec)
{
    unit_ = unit;
    interval_ = interval;
    batch_ = batch;
    pauseSec_ = pauseSec;

    if (!enabled) {
        enabled_ = false;
        ESP_LOGI(TAG, "auto-update off");
        return;
    }

    // Turning it on, or changing the period, starts the countdown from a whole
    // period: the operator asked for "every hour", not "an hour from whenever
    // the last cycle happened to be".
    periodSec_ = effectivePeriodSec();
    remainingSec_ = periodSec_;
    restartRequested_ = true;
    enabled_ = true;
    // A fresh start does not yet know whether the clock is set — the task says so
    // within a tick. Claiming a countdown before that is what a stale flag from a
    // previous run would do.
    clockSet_ = false;

    if (task_ == nullptr) {
        TaskHandle_t handle = nullptr;
        if (xTaskCreate(taskEntry, "cache_upd", kTaskStackBytes, this,
                        kTaskPriority, &handle) != pdPASS) {
            enabled_ = false;
            ESP_LOGE(TAG, "cannot start the auto-update task");
            return;
        }
        task_ = handle;
    }
    ESP_LOGI(TAG, "auto-update every %u s, batch %u, pause %u s",
             static_cast<unsigned>(periodSec_), static_cast<unsigned>(batch_),
             static_cast<unsigned>(pauseSec_));
}

CacheAutoUpdate::Status CacheAutoUpdate::status() const
{
    Status s;
    s.enabled = enabled_;
    s.counting = enabled_ && clockSet_;
    s.running = running_;
    s.remainingSec = remainingSec_;
    return s;
}

void CacheAutoUpdate::taskEntry(void* arg)
{
    static_cast<CacheAutoUpdate*>(arg)->run();
    vTaskDelete(nullptr);
}

void CacheAutoUpdate::run()
{
    uint32_t clockWaitTicks = 0;
    bool wasSet = true;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));

        if (!enabled_) {
            task_ = nullptr;
            return;              // stop(); configure(true, …) starts a new task
        }

        // Suspended while the clock is not set: an hour or a day cannot be
        // counted against a device that starts at 1970.
        if (!clockIsSet()) {
            wasSet = false;
            clockSet_ = false;   // the countdown is not running: do not show one
            if (clockWaitTicks++ % kClockWaitLogEveryTicks == 0) {
                ESP_LOGW(TAG, "auto-update waiting for the clock to be set");
            }
            continue;
        }
        // The clock is set, so the countdown really runs. This must NOT live in
        // the `!wasSet` branch below: `wasSet` starts true for a device that
        // already knows the time, so that branch never runs and the flag would
        // stay false for ever — which is exactly how the countdown stayed hidden
        // on the first build of stage 174.
        clockSet_ = true;
        if (!wasSet) {
            wasSet = true;
            periodSec_ = effectivePeriodSec();
            remainingSec_ = periodSec_;
            ESP_LOGI(TAG, "clock is set: auto-update every %u s",
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

        runCycle();
        periodSec_ = effectivePeriodSec();
        remainingSec_ = periodSec_;
    }
}

bool CacheAutoUpdate::isPending(const char* name, uint16_t qtype) const
{
    for (const auto& p : pending_) {
        if (p.qtype == qtype && p.name == name) return true;
    }
    return false;
}

void CacheAutoUpdate::rememberFailed(const string& name, uint16_t qtype)
{
    if (isPending(name.c_str(), qtype)) return;
    if (pending_.size() >= kMaxPending) pending_.erase(pending_.begin());
    pending_.push_back({name, qtype});
}

void CacheAutoUpdate::forgetFailed(const string& name, uint16_t qtype)
{
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->qtype == qtype && it->name == name) {
            pending_.erase(it);
            return;
        }
    }
}

void CacheAutoUpdate::runCycle()
{
    // Nothing to do without a way to ask, without the cache, or with a cache
    // whose records expire on their own — the sweep exists for the "eternal"
    // ones, and that is exactly what Ignore TTL means.
    if (!refresh_ || !cache_.available() || !cache_.ignoreTtl()) return;

    // Records that failed last time go first; the ones gone from the cache in the
    // meantime are dropped rather than retried forever.
    vector<InternalDnsCache::RefreshCandidate> plan;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (cache_.contains(it->name, it->qtype)) {
            ++it;
        } else {
            it = pending_.erase(it);
        }
    }
    const size_t want = batch_;
    for (const auto& p : pending_) {
        if (plan.size() >= want) break;
        plan.push_back(p);
    }
    if (plan.size() < want) {
        auto oldest = cache_.oldestEntries(
            want - plan.size(),
            [this](const char* n, uint16_t t) { return isPending(n, t); });
        for (auto& e : oldest) plan.push_back(move(e));
    }
    if (plan.empty()) return;

    core::JobRegistry& jobs = core::JobRegistry::instance();
    if (!jobs.begin(kJobId, kJobTitleKey, "", static_cast<uint32_t>(plan.size()))) {
        ESP_LOGW(TAG, "job registry is full, skipping this cycle");
        return;
    }

    // The countdown is stopped while the cycle runs — the page says "updating"
    // instead of showing a frozen number (stage 174).
    running_ = true;
    uint32_t done = 0;
    for (const auto& item : plan) {
        if (!enabled_) break;

        vector<string> ips;
        uint32_t ttl = 0;
        if (refresh_(item.name, item.qtype, ips, ttl) && !ips.empty()) {
            cache_.store(item.name, item.qtype, ips, ttl);
            forgetFailed(item.name, item.qtype);
        } else {
            // Kept, not deleted: a timeout is not proof the name is gone.
            rememberFailed(item.name, item.qtype);
        }
        ++done;
        jobs.progress(kJobId, done, static_cast<uint32_t>(plan.size()), item.name);

        // A stop pressed on the scheduler page turns the auto-update off, as
        // decided for the autosave: the registry only records the request.
        if (jobs.cancelRequested(kJobId)) {
            jobs.finish(kJobId, core::JobState::Cancelled, "stopped by the operator");
            enabled_ = false;
            running_ = false;
            ESP_LOGI(TAG, "auto-update stopped from the scheduler page");
            if (onDisabled_) onDisabled_();
            return;
        }

        // Pause after the record, one tick at a time, so a stop is seen within a
        // second instead of after the whole pause.
        for (uint32_t s = 0; s < static_cast<uint32_t>(pauseSec_) && enabled_; ++s) {
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
        }
    }

    running_ = false;

    if (!enabled_) {
        jobs.finish(kJobId, core::JobState::Cancelled, "stopped by the operator");
        return;
    }
    jobs.finish(kJobId, core::JobState::Done, to_string(done) + " records");
    ESP_LOGI(TAG, "auto-update refreshed %u records", static_cast<unsigned>(done));
}

} // namespace dns
} // namespace dhcp
