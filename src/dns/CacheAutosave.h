#ifndef DHCP_DNS_CACHEAUTOSAVE_H
#define DHCP_DNS_CACHEAUTOSAVE_H

#include "InternalDnsCache.h"
#include "core/AutosavePeriod.h"

#include <cstdint>
#include <functional>
#include <string>

namespace dhcp {
namespace dns {

/**
 * @brief Saves the built-in DNS cache on a timer (stages 153/154).
 *
 * Owns one low-priority task with a one-second tick. The task waits out the
 * configured period — a unit and an interval, say "every 2 hours" — then writes
 * the cache to the FAT partition, announcing itself in the job registry for the
 * duration of the write so the operator sees it on the scheduler page and may
 * stop it there.
 *
 * Decisions of the operator (19.09.2026), kept here so the code and the plan do
 * not drift apart:
 *  - the registry record exists **only while a save runs** (no `repeatSec`): the
 *    registry answers "what is running now" and has eight slots in all;
 *  - stopping the operation on the scheduler page **switches the autosave off
 *    entirely** — the callback reports that to the settings, which persist it;
 *  - the autosave is **suspended while the clock is not set** (neither by hand
 *    nor by NTP): a device that does not know the date cannot decide whether its
 *    cache file is stale, and the day bound of the interval depends on the month
 *    that is running.
 */
class CacheAutosave {
public:
    /// @brief Called when the operation was stopped and autosave must go off.
    using DisabledHandler = std::function<void()>;

    /**
     * @param[in] cache  The cache to save; must outlive this object.
     * @param[in] path   Where the cache file lives (`/fat/cache.dat`).
     */
    CacheAutosave(InternalDnsCache& cache, std::string path);
    ~CacheAutosave();

    CacheAutosave(const CacheAutosave&) = delete;
    CacheAutosave& operator=(const CacheAutosave&) = delete;

    /// @brief Report a stop that came from the scheduler page (owned by caller).
    void setDisabledHandler(DisabledHandler handler) { onDisabled_ = std::move(handler); }

    /**
     * @brief Apply the settings: start, re-arm or stop the timer.
     *
     * @param[in] enabled  Whether the operator wants the cache saved by itself.
     * @param[in] unit     Minutes, hours or days.
     * @param[in] interval How many of that unit between two saves.
     */
    void configure(bool enabled, core::AutosavePeriod unit, uint16_t interval);

    /**
     * @brief Shift the countdown, because a manual save just happened.
     *
     * The operator asked for this: a manual save is a save, so the next
     * automatic one is a full period away rather than around the corner.
     */
    void notifyManualSave();

    /// @brief True while the task is waiting for the next period.
    bool active() const { return enabled_; }

private:
    static void taskEntry(void* arg);
    void run();
    void saveNow();

    /// @brief Seconds of the configured period, using the month that is running.
    uint32_t effectivePeriodSec() const;
    /// @brief Length of the current month, or 0 when the clock is not set.
    static uint16_t currentMonthDays();
    /// @brief True when the device clock looks set (see kClockSetSinceEpoch).
    static bool clockIsSet();

    InternalDnsCache& cache_;
    std::string path_;

    DisabledHandler onDisabled_;

    void* task_ = nullptr;                 ///< FreeRTOS task handle (opaque here).
    volatile bool enabled_ = false;
    core::AutosavePeriod unit_ = core::AutosavePeriod::Hour;
    uint16_t interval_ = 1;
    volatile uint32_t periodSec_ = 0;
    volatile uint32_t remainingSec_ = 0;   ///< Countdown, written by the task too.
    volatile bool restartRequested_ = false;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_CACHEAUTOSAVE_H
