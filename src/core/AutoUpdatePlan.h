#ifndef DHCP_CORE_AUTOUPDATE_PLAN_H
#define DHCP_CORE_AUTOUPDATE_PLAN_H

#include "AutosavePeriod.h"

#include <cstdint>

namespace dhcp {
namespace core {

/**
 * @file AutoUpdatePlan.h
 * @brief Bounds of the built-in-cache auto-update (stage 172).
 *
 * The auto-update refreshes the built-in cache from the upstream DNS on a timer.
 * One cycle refreshes up to `batch` records, pausing `pause` seconds after each
 * one, and the next cycle starts one `period` later. The period reuses the units
 * the autosave already stores (`AutosavePeriod`), so both share the same guard
 * against a value another firmware wrote.
 *
 * Free of ESP-IDF on purpose, the same way TimeMath and AutosavePeriod are: the
 * settings, the REST API, the task and the host test all need these bounds, and
 * none of them should spell 1000 or 3600 again.
 */

/// @brief A cycle refreshes at least one record…
inline constexpr uint16_t kAutoUpdateBatchMin = 1;
/// @brief …and at most this many, so one cycle cannot run for hours.
inline constexpr uint16_t kAutoUpdateBatchMax = 1000;
/// @brief The pause after a record is at least a second (0 would hammer upstream)…
inline constexpr uint16_t kAutoUpdatePauseMinSec = 1;
/// @brief …and at most an hour.
inline constexpr uint16_t kAutoUpdatePauseMaxSec = 3600;

/// @brief Longest interval of the auto-update: a day of hours or (a fixed, not
///        calendar, bound) thirty days. Unlike the autosave the days bound does
///        not follow the month: the sweep is not tied to a file growing stale.
inline constexpr uint16_t kAutoUpdateDaysMax = 30;

/// @brief Longest interval for a unit, in the auto-update case.
constexpr uint16_t autoUpdateIntervalMax(AutosavePeriod unit)
{
    return (unit == AutosavePeriod::Day) ? kAutoUpdateDaysMax : kHoursPerDay;
}

/// @brief Bring an interval inside its bounds: 0 becomes 1, too large becomes max.
constexpr uint16_t autoUpdateClampInterval(AutosavePeriod unit, uint16_t interval)
{
    if (interval < kAutosaveIntervalMin) return kAutosaveIntervalMin;
    const uint16_t max = autoUpdateIntervalMax(unit);
    return (interval > max) ? max : interval;
}

/// @brief Bring a batch size inside [1, 1000].
constexpr uint16_t autoUpdateClampBatch(int32_t value)
{
    if (value < kAutoUpdateBatchMin) return kAutoUpdateBatchMin;
    if (value > kAutoUpdateBatchMax) return kAutoUpdateBatchMax;
    return static_cast<uint16_t>(value);
}

/// @brief Bring the pause inside [1, 3600] seconds.
constexpr uint16_t autoUpdateClampPause(int32_t value)
{
    if (value < kAutoUpdatePauseMinSec) return kAutoUpdatePauseMinSec;
    if (value > kAutoUpdatePauseMaxSec) return kAutoUpdatePauseMaxSec;
    return static_cast<uint16_t>(value);
}

/// @brief Whether the auto-update countdown must restart from a whole period.
///
/// The timer re-arms on a fresh enable and on a changed period, but not when the
/// same settings are applied again: every DNS settings save reaches the timer
/// through the same call, and throwing the remaining time away on an unrelated
/// save made the page reset "Next update in" for no reason (stage 176).
constexpr bool autoUpdateReArmNeeded(bool wasEnabled, uint32_t oldPeriodSec,
                                     uint32_t newPeriodSec)
{
    return !wasEnabled || oldPeriodSec != newPeriodSec;
}

/// @brief Why a sweep asked for by the operator cannot be served (stage 177).
enum class AutoUpdateRunNowRefusal {
    None = 0,       ///< the sweep may start
    NotEnabled,     ///< Auto Update is switched off: no task to ask
    IgnoreTtlOff,   ///< Ignore TTL is off: the records expire on their own
    Unavailable,    ///< the cache is not there (no PSRAM arena)
    AlreadyRunning, ///< a cycle is in progress right now
};

/// @brief Whether one sweep may be started right away, and if not, why.
///
/// The reasons mirror the guards the cycle itself follows — `runCycle()` returns
/// at once without them — so the button refuses in words instead of promising a
/// sweep that would walk nothing. The order is the order of the switches the
/// operator sees: the feature, then Ignore TTL, then the cache, and a second
/// request while a cycle runs is refused rather than queued, because that second
/// cycle would ask the upstream for the same records all over again.
constexpr AutoUpdateRunNowRefusal autoUpdateRunNowRefusal(bool enabled, bool ignoreTtl,
                                                          bool cacheAvailable, bool running)
{
    if (!enabled) return AutoUpdateRunNowRefusal::NotEnabled;
    if (!ignoreTtl) return AutoUpdateRunNowRefusal::IgnoreTtlOff;
    if (!cacheAvailable) return AutoUpdateRunNowRefusal::Unavailable;
    if (running) return AutoUpdateRunNowRefusal::AlreadyRunning;
    return AutoUpdateRunNowRefusal::None;
}

/// @brief How many blocks one full sweep of @p records needs, at @p batch each.
///
/// A sweep walks the **whole** cache — that is what the operator expects from one
/// "update" — and the configured period counts between sweeps, not between blocks
/// (stage 180). A cache with nothing in it, or a batch of none, needs no block.
constexpr uint32_t autoUpdateBlockCount(uint32_t records, uint16_t batch)
{
    if (records == 0 || batch == 0) return 0;
    // Round up: a last, partly filled block still has records to walk.
    return (records + batch - 1) / batch;
}

/// @brief True when a pause follows this block.
///
/// The pause sits **between** blocks, so the last one of a sweep must not send the
/// task to sleep for the whole pause — the countdown is re-armed right after the
/// sweep, and a sleeping sweep is a countdown that has not started yet.
constexpr bool autoUpdatePauseAfterBlock(uint32_t blockIndex, uint32_t blockCount)
{
    return blockIndex + 1 < blockCount;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTOUPDATE_PLAN_H
