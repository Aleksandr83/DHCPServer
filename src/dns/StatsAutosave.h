#ifndef DHCP_DNS_STATSAUTOSAVE_H
#define DHCP_DNS_STATSAUTOSAVE_H

#include "core/AutosavePeriod.h"

#include <cstdint>
#include <functional>
#include <string>

namespace dhcp {
namespace dns {

/**
 * @brief Writes `Statistica.dat` on a timer (stage 189).
 *
 * The device's second autosave, and deliberately not a second copy of the first
 * one: the cache's timer writes megabytes of records, this one writes a 92-byte
 * record, and the operator asked for the Statistics block to look after its own
 * file. Its rules are the ones the cache's autosave already follows, because
 * they were decided once and there is no reason for two answers:
 *
 *  - the registry record exists **only while a write runs** (no `repeatSec`), so
 *    the scheduler page shows the operation while it is there and no longer;
 *  - stopping it on the scheduler page **switches the autosave off** — the
 *    callback reports that to the settings, which persist it;
 *  - the timer is **suspended while the clock is not set**: a device that does
 *    not know the date cannot honour "every hour", and the day bound of the
 *    interval depends on the month that is running.
 *
 * The write itself is not this class's business. The device already knows how to
 * write the statistics — the same single-flight job the reboot path and the
 * page's button use, with its read-back check and its verdict — so the owner
 * hands that in as a callback (`WriteHandler`) rather than letting a timer learn
 * what a statistics file is.
 */
class StatsAutosave {
public:
    /// @brief Called when the operation was stopped and autosave must go off.
    using DisabledHandler = std::function<void()>;

    /**
     * @brief Write the statistics file now and say what the device decided.
     * @param[out] detail The device's own reason when the answer is false.
     * @return true when the file was written and read back.
     */
    using WriteHandler = std::function<bool(std::string& detail)>;

    /// @brief State of the timer, for `GET /api/dns/autosave`.
    struct Status {
        bool enabled = false;       ///< the switch is on
        bool counting = false;      ///< a countdown is really running (the clock is set)
        uint32_t remainingSec = 0;  ///< seconds left of the period
        uint32_t periodSec = 0;     ///< length of the whole period
    };

    explicit StatsAutosave(WriteHandler write);
    ~StatsAutosave();

    StatsAutosave(const StatsAutosave&) = delete;
    StatsAutosave& operator=(const StatsAutosave&) = delete;

    /// @brief Report a stop that came from the scheduler page (owned by caller).
    void setDisabledHandler(DisabledHandler handler) { onDisabled_ = std::move(handler); }

    /**
     * @brief Apply the settings: start, re-arm or stop the timer.
     *
     * @param[in] enabled  Whether the operator wants the statistics saved by itself.
     * @param[in] unit     Minutes, hours or days.
     * @param[in] interval How many of that unit between two writes.
     */
    void configure(bool enabled, core::AutosavePeriod unit, uint16_t interval);

    /**
     * @brief Shift the countdown, because a manual save just happened.
     *
     * The rule the cache's autosave follows as well: a manual save is a save, so
     * the next automatic one is a full period away rather than around the corner.
     */
    void notifyManualSave();

    /// @brief State for the page's "Next update in" row.
    Status status() const;

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

    WriteHandler write_;
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

#endif // DHCP_DNS_STATSAUTOSAVE_H
