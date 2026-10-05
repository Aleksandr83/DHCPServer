#include "CacheAutoUpdate.h"

#include "AutoUpdateLogFormat.h"
#include "core/AutoUpdatePlan.h"
#include "core/JobLog.h"
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
    const bool wasEnabled = enabled_;
    const uint32_t oldPeriodSec = periodSec_;

    unit_ = unit;
    interval_ = interval;
    batch_ = batch;
    pauseSec_ = pauseSec;

    if (!enabled) {
        enabled_ = false;
        ESP_LOGI(TAG, "auto-update off");
        return;
    }

    // Re-arm the countdown only when it actually matters: turning the sweep on,
    // or changing the period, starts it from a whole period ("every hour", not
    // "an hour from whenever the last save happened to be"). Saving the same
    // settings again — or unrelated DNS settings, which arrive through the same
    // POST — must not throw away the remaining time (stage 176).
    const uint32_t newPeriodSec = effectivePeriodSec();
    if (core::autoUpdateReArmNeeded(wasEnabled, oldPeriodSec, newPeriodSec)) {
        periodSec_ = newPeriodSec;
        remainingSec_ = newPeriodSec;
        restartRequested_ = true;
        // A fresh start does not yet know whether the clock is set — the task
        // says so within a tick. Claiming a countdown before that is what a
        // stale flag from a previous run would do.
        clockSet_ = false;
    } else {
        periodSec_ = newPeriodSec;   // the same seconds: nothing to re-arm
    }
    enabled_ = true;

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

bool CacheAutoUpdate::requestNow()
{
    if (!enabled_) return false;
    nowRequested_ = true;
    return true;
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

        // A sweep the operator asked for waits for nothing: not for the
        // countdown and not for the clock. The countdown counts periods, and one
        // asked-for sweep has no period to wait out (stage 177); it is re-armed
        // afterwards exactly like a scheduled cycle.
        if (nowRequested_) {
            nowRequested_ = false;
            runSweep();
            periodSec_ = effectivePeriodSec();
            remainingSec_ = periodSec_;
            continue;
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

        runSweep();
        // The period counts between sweeps: a whole cache was just walked (stage
        // 180), so the next one starts a full period from here.
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

void CacheAutoUpdate::runSweep()
{
    // Nothing to do without a way to ask, without the cache, or with a cache
    // whose records expire on their own — the sweep exists for the "eternal"
    // ones, and that is exactly what Ignore TTL means.
    if (!refresh_ || !cache_.available() || !cache_.ignoreTtl()) return;

    // How much there is to walk, counted once: a record that arrives while the
    // sweep runs waits for the next one instead of moving the finish line.
    const uint32_t records = static_cast<uint32_t>(cache_.stats().entries);
    const uint32_t blocks = core::autoUpdateBlockCount(records, batch_);
    if (blocks == 0) return;

    core::JobRegistry& jobs = core::JobRegistry::instance();
    if (!jobs.begin(kJobId, kJobTitleKey, "", records, core::JobUnit::Records)) {
        ESP_LOGW(TAG, "job registry is full, skipping this sweep");
        return;
    }

    // The countdown is stopped while the sweep runs — the page says "updating"
    // instead of showing a frozen number (stage 174).
    running_ = true;

    // Records that failed the **previous** sweep are tried here, and only once per
    // sweep: this sweep's own failures collect in `pending_` again, which is also
    // the filter that keeps a just-failed record out of the later blocks.
    vector<InternalDnsCache::RefreshCandidate> retry;
    retry.swap(pending_);
    size_t retryTaken = 0;

    uint32_t done = 0;
    for (uint32_t block = 0; block < blocks && enabled_; ++block) {
        // One block: the retries first, then the oldest records still in the
        // cache. `oldestEntries` orders by the time a record was stored and a
        // refreshed record moves to the young end, so the next block continues
        // where this one stopped — that is how a sweep walks the whole cache
        // without keeping a list of what it has seen.
        vector<InternalDnsCache::RefreshCandidate> plan;
        while (plan.size() < batch_ && retryTaken < retry.size()) {
            const auto& item = retry[retryTaken++];
            if (cache_.contains(item.name, item.qtype)) plan.push_back(item);
        }
        if (plan.size() < batch_) {
            auto oldest = cache_.oldestEntries(
                batch_ - plan.size(),
                [this](const char* n, uint16_t t) { return isPending(n, t); });
            for (auto& e : oldest) plan.push_back(move(e));
        }
        if (plan.empty()) break;

        for (const auto& item : plan) {
            if (!enabled_) break;

            vector<string> ips;
            uint32_t ttl = 0;
            const bool refreshed =
                refresh_(item.name, item.qtype, ips, ttl) && !ips.empty();
            if (refreshed) {
                cache_.store(item.name, item.qtype, ips, ttl);
                forgetFailed(item.name, item.qtype);
            } else {
                // Kept, not deleted: a timeout is not proof the name is gone, and
                // the retry belongs to the next sweep rather than to the next
                // block of this one.
                rememberFailed(item.name, item.qtype);
            }
            // The journal holds only the two ends of the sweep; the operator asked
            // for the records as well, and they go to the auto-update's own file
            // (stage 179) — the same task writes both.
            core::JobLog::instance().autoUpdate(
                kJobId, autoUpdateRecordText(item.name, item.qtype, refreshed, ttl));
            ++done;
            jobs.progress(kJobId, done, records, item.name);

            // A stop pressed on the scheduler page ends the sweep at the next
            // record (stage 184); the setting is the page's business, not this
            // operation's, so the request is only read here and in the pause.
            if (stopIfAsked()) return;
        }

        // The pause sits between blocks (stage 180) and never after the last one:
        // one tick at a time, so the auto-update being switched off is seen within
        // a second, and the countdown is re-armed the moment the sweep ends.
        if (!core::autoUpdatePauseAfterBlock(block, blocks)) continue;
        // The scheduler shows that pause instead of a frozen row (stage 181): the
        // row turns into "paused, 60 s" and counts down, because the seconds left
        // are announced once a second rather than the pause staying invisible.
        const uint32_t pauseSec = static_cast<uint32_t>(pauseSec_);
        for (uint32_t s = 0; s < pauseSec && enabled_; ++s) {
            // A stop is noticed here as well, so pressing it does not have to wait
            // out the pause (stage 182) — the question costs one registry read a
            // second, the same one the loop between two records asks.
            if (stopIfAsked()) return;
            jobs.pause(kJobId, "", pauseSec - s);
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
        }
        // Back to work: the first record of the next block can wait up to three
        // seconds for upstream, and the row must not keep saying "paused" meanwhile.
        jobs.progress(kJobId, done, records);
    }

    running_ = false;

    if (!enabled_) {
        jobs.finish(kJobId, core::JobState::Cancelled, "stopped by the operator");
        return;
    }
    jobs.finish(kJobId, core::JobState::Done, to_string(done) + " records");
    ESP_LOGI(TAG, "auto-update refreshed %u of %u records", static_cast<unsigned>(done),
             static_cast<unsigned>(records));
}

bool CacheAutoUpdate::stopIfAsked()
{
    core::JobRegistry& jobs = core::JobRegistry::instance();
    if (!jobs.cancelRequested(kJobId)) return false;

    // The stop belongs to this sweep and to nothing else (stage 184). `run()`
    // re-arms the countdown the moment the sweep returns, so the cache goes on
    // being refreshed a full period later, and the operator's setting — the
    // "Auto Update" switch on the internal-cache page, which the page reads
    // straight from NVS — is left as it was saved. Waking up a stopped sweep
    // means walking into the page and using that switch.
    //
    // The autosave is deliberately not like this: its write is one call into
    // FatFS that cannot be aborted, so a stop there has no operation left to
    // end and can only mean "no more automatic saves" (stage 153–155).
    jobs.finish(kJobId, core::JobState::Cancelled, "stopped by the operator");
    running_ = false;
    ESP_LOGI(TAG, "auto-update stopped from the scheduler page; the setting stays as it is");
    return true;
}

} // namespace dns
} // namespace dhcp
