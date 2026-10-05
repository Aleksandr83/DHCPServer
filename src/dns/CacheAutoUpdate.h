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
 * **sweep**: the whole cache is walked in blocks of `batch` records (the same name
 * and type asked of the upstream again, and one that answers written back with its
 * fresh TTL, which makes the record young again). The period counts **between
 * sweeps**, so a cache of 2000 records is refreshed in one go instead of 40 times
 * over 40 periods.
 *
 * Pacing lives between the blocks, not inside them: `pause` seconds sit after a
 * block and before the next one, so the pause throttles the sweep without stalling
 * every single record (stage 180 — before that the pause was applied per record,
 * which turned a block of 50 into 50 minutes of waiting).
 *
 * Which records: the ones whose last refresh failed come first, and only once per
 * sweep; after them the oldest records still in the pool — a refreshed record
 * moves to the young end, so block after block walks the whole cache without a
 * list of what has been seen. A record that fails is kept and remembered for the
 * next sweep rather than deleted: a timeout is not proof that the name is gone.
 *
 * The sweep announces itself in the job registry for its duration — with the whole
 * cache as its total, not one block — so the operator sees real progress on the
 * scheduler page and may stop it there. A stop ends **this sweep** and nothing
 * else (stage 184): the countdown is re-armed, the cache goes on being refreshed,
 * and the setting is left exactly as the operator saved it on the page. The
 * autosave keeps the opposite rule on purpose — its stop means "no more automatic
 * saves" (stage 153–155) — see `stopIfAsked()`.
 *
 * The task is suspended while the clock is not set: the period counts hours and
 * days, and a day is not a fixed number of seconds on a device that does not
 * know whether it is February.
 */
class CacheAutoUpdate {
public:

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

    /**
     * @brief Ask for one sweep right away (stage 177).
     *
     * The request is served on the task's next tick and waits for neither the
     * countdown nor the clock: the countdown counts periods, while one sweep the
     * operator asked for has no period to wait out. The countdown is re-armed
     * afterwards, exactly as after a scheduled cycle.
     *
     * @return false when the feature is off — there is no task to ask then.
     */
    bool requestNow();

private:
    static void taskEntry(void* arg);
    void run();
    /// @brief One sweep: the whole cache, in blocks of `batch`, a pause between.
    void runSweep();
    /**
     * @brief Finish the sweep when the scheduler page asked for it to stop.
     *
     * The page only records the request, so noticing it is the owner's job
     * (stage 173) — and the owner notices it both between two records and once a
     * second **inside** a pause between two blocks (stage 182), so a stop does
     * not have to wait out "Pause after update".
     *
     * A stop ends the sweep and only the sweep (stage 184). The countdown is
     * re-armed by `run()`, so the next one comes a full period later, and the
     * setting — "Auto Update" on the internal-cache page — is not touched: the
     * operator reached for the scheduler's Stop to end an operation, not to
     * change a saved setting, and the page has the switch for that. The autosave
     * is deliberately different: its write is one call into FatFS that cannot be
     * aborted, so a stop there can only mean "no more automatic saves" and it
     * turns the setting off (stage 153–155).
     *
     * @return true when the sweep was stopped and the caller must return at once.
     */
    bool stopIfAsked();
    /// @brief True when the record is already on the retry-first list.
    bool isPending(const char* name, uint16_t qtype) const;
    void rememberFailed(const std::string& name, uint16_t qtype);
    void forgetFailed(const std::string& name, uint16_t qtype);

    /// @brief Seconds of the configured period.
    uint32_t effectivePeriodSec() const;
    /// @brief True when the device clock looks set (see kClockSetSinceEpoch).
    static bool clockIsSet();

    InternalDnsCache& cache_;

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
    /// A sweep was asked for by the operator and is served on the next tick
    /// (stage 177). Written by the REST task, read by the timer task.
    volatile bool nowRequested_ = false;
    /// Records whose last refresh failed; tried again first next cycle. Only the
    /// task touches this list, so it needs no lock of its own.
    std::vector<InternalDnsCache::RefreshCandidate> pending_;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_CACHEAUTOUPDATE_H
