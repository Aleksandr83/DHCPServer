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

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTOUPDATE_PLAN_H
