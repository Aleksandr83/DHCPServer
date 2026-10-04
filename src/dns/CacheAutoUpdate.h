#ifndef DHCP_DNS_CACHEAUTOUPDATE_H
#define DHCP_DNS_CACHEAUTOUPDATE_H

#include "InternalDnsCache.h"
#include "core/AutosavePeriod.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dhcp {
namespace dns {

/**
 * @brief Refreshes the built-in DNS cache from upstream on a timer (stage 172).
 *
 * With "Ignore TTL" on, a record never expires, so nothing but this task keeps it
 * honest: a domain that changes its address would sit in the cache forever. The
 * class owns one low-priority task with a one-second tick. It waits out the
 * configured period — a unit and an interval, say "every 2 hours" — then runs one
 * cycle: up to `batch` records are asked of the upstream DNS again (the same
 * name and type), and one that answers is written back with its fresh TTL, which
 * makes the record young again. The `pause` seconds after each record keep the
 * sweep from hammering the upstream.
 *
 * Which records: the ones whose last refresh failed come first, then the oldest
 * records still in the pool — so a sweep of `batch` per cycle walks the whole
 * cache by itself. A record that fails is kept and remembered for the next cycle
 * rather than deleted: a timeout is not proof that the name is gone.
 *
 * The cycle announces itself in the job registry for its duration, so the
 * operator sees it on the scheduler page and may stop it there; a stop switches
 * the auto-update off for good, exactly like the autosave.
 *
 * The task is suspended while the clock is not set: the period counts hours and
 * days, and a day is not a fixed number of seconds on a device that does not
 * know whether it is February.
 */
class CacheAutoUpdate {
public:
    /// @brief Called when the cycle was stopped and the feature must go off.
    using DisabledHandler = std::function<void()>;

    /**
     * @brief Ask the upstream DNS for one record.
     * @param[in]  name   Domain name, as it is stored in the cache.
     * @param[in]  qtype  A (1) or AAAA (28).
     * @param[out] ips    The fresh addresses; empty when nothing came back.
     * @param[out] ttl    The fresh TTL in seconds.
     * @return false when the upstream did not answer or did not answer with an IP.
     */
    using RefreshHandler = std::function<bool(const std::string& name, uint16_t qtype,
                                              std::vector<std::string>& ips, uint32_t& ttl)>;

    /// @param[in] cache  The cache to refresh; must outlive this object.
    explicit CacheAutoUpdate(InternalDnsCache& cache);
    ~CacheAutoUpdate();

    CacheAutoUpdate(const CacheAutoUpdate&) = delete;
    CacheAutoUpdate& operator=(const CacheAutoUpdate&) = delete;

    /// @brief Report a stop that came from the scheduler page (owned by caller).
    void setDisabledHandler(DisabledHandler handler) { onDisabled_ = std::move(handler); }
    /// @brief Report the way a single record is refreshed (owned by caller).
    void setRefreshHandler(RefreshHandler handler) { refresh_ = std::move(handler); }

    /**
     * @brief Apply the settings: start, re-arm or stop the timer.
     *
     * @param[in] enabled  Whether the operator wants the cache refreshed by itself.
     * @param[in] unit     Hours or days.
     * @param[in] interval How many of that unit between two cycles.
     * @param[in] batch    How many records one cycle refreshes at most.
     * @param[in] pauseSec Seconds to wait after each refreshed record.
     */
    void configure(bool enabled, core::AutosavePeriod unit, uint16_t interval,
                   uint16_t batch, uint16_t pauseSec);

    /// @brief True while the task is waiting for the next period.
    bool active() const { return enabled_; }

    /**
     * @brief What the page shows under "Update every" (stage 174).
     *
     * The countdown is the task's own remaining time, not a value derived from
     * the settings: it is re-armed whenever the settings change and it is the
     * truth about when the next cycle really happens.
     */
    struct Status {
        bool enabled = false;      ///< the operator switched it on
        bool counting = false;     ///< the clock is set, so the countdown runs
        bool running = false;      ///< a refresh cycle is in progress right now
        uint32_t remainingSec = 0; ///< seconds until the next cycle
    };
    Status status() const;

private:
    static void taskEntry(void* arg);
    void run();
    /// @brief One sweep: up to `batch` records, a pause after each.
    void runCycle();
    /// @brief True when the record is already on the retry-first list.
    bool isPending(const char* name, uint16_t qtype) const;
    void rememberFailed(const std::string& name, uint16_t qtype);
    void forgetFailed(const std::string& name, uint16_t qtype);

    /// @brief Seconds of the configured period.
    uint32_t effectivePeriodSec() const;
    /// @brief True when the device clock looks set (see kClockSetSinceEpoch).
    static bool clockIsSet();

    InternalDnsCache& cache_;

    DisabledHandler onDisabled_;
    RefreshHandler refresh_;

    void* task_ = nullptr;                 ///< FreeRTOS task handle (opaque here).
    volatile bool enabled_ = false;
    core::AutosavePeriod unit_ = core::AutosavePeriod::Hour;
    uint16_t interval_ = 1;
    uint16_t batch_ = 50;
    uint16_t pauseSec_ = 60;
    volatile uint32_t periodSec_ = 0;
    volatile uint32_t remainingSec_ = 0;   ///< Countdown, written by the task too.
    volatile bool restartRequested_ = false;
    /// The clock is set, so the countdown is really running (the task is asleep
    /// otherwise, and a frozen number would be a lie — stage 174).
    volatile bool clockSet_ = false;
    /// A refresh cycle is in progress: the countdown is stopped while it runs.
    volatile bool running_ = false;
    /// Records whose last refresh failed; tried again first next cycle. Only the
    /// task touches this list, so it needs no lock of its own.
    std::vector<InternalDnsCache::RefreshCandidate> pending_;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_CACHEAUTOUPDATE_H
