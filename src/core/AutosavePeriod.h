#ifndef DHCP_CORE_AUTOSAVE_PERIOD_H
#define DHCP_CORE_AUTOSAVE_PERIOD_H

#include <cstdint>

namespace dhcp {
namespace core {

/**
 * @file AutosavePeriod.h
 * @brief When the cache saves itself (stages 153/154).
 *
 * A period is a **unit plus an interval**: "every N minutes", "every N hours",
 * "every N days". The interval counts how many of the unit fit into the next one
 * up, so its maximum follows from the unit, and for days from the length of the
 * month that is running right now (28…31).
 *
 * Kept free of ESP-IDF on purpose, the same way TimeMath is: the settings, the
 * task, the REST API and the host test all need these rules, and none of them
 * should spell out 24 or 31 again.
 */

/// @brief The unit the interval counts in (the stored index is this value).
enum class AutosavePeriod : uint8_t {
    Hour   = 0,  ///< every N hours, N = 1..24
    Day    = 1,  ///< every N days, N = 1..length of the current month
    Minute = 2,  ///< every N minutes, N = 1..60
};

/// @brief How many units the page offers (and the settings accept).
inline constexpr uint8_t kAutosavePeriodCount = 3;

/// @brief The scale a period is counted in.
inline constexpr uint32_t kSecondsPerMinute = 60;
inline constexpr uint32_t kSecondsPerHour = 60 * kSecondsPerMinute;
inline constexpr uint32_t kSecondsPerDay = 24 * kSecondsPerHour;

/// @brief An interval is at least one of its unit…
inline constexpr uint16_t kAutosaveIntervalMin = 1;
/// @brief …and at most the next unit up.
inline constexpr uint16_t kMinutesPerHour = 60;
inline constexpr uint16_t kHoursPerDay = 24;
/// @brief Used when the month is not known: the longest month, so nothing valid
///        is refused for lack of knowledge.
inline constexpr uint16_t kDaysPerMonthFallback = 31;
/// @brief February of a common year — anything shorter is not a month.
inline constexpr uint16_t kDaysPerMonthMin = 28;

/// @brief Longest interval for a unit. @p daysInMonth 0 means "unknown".
constexpr uint16_t autosaveIntervalMax(AutosavePeriod unit, uint16_t daysInMonth)
{
    switch (unit) {
        case AutosavePeriod::Minute: return kMinutesPerHour;
        case AutosavePeriod::Day:
            return (daysInMonth >= kDaysPerMonthMin &&
                    daysInMonth <= kDaysPerMonthFallback)
                       ? daysInMonth
                       : kDaysPerMonthFallback;
        case AutosavePeriod::Hour:   break;
    }
    return kHoursPerDay;
}

/// @brief Bring an interval inside its bounds: 0 becomes 1, too large becomes max.
constexpr uint16_t autosaveClampInterval(AutosavePeriod unit, uint16_t interval,
                                         uint16_t daysInMonth)
{
    const uint16_t max = autosaveIntervalMax(unit, daysInMonth);
    if (interval < kAutosaveIntervalMin) return kAutosaveIntervalMin;
    return (interval > max) ? max : interval;
}

/// @brief Seconds in "every @p interval @p unit".
constexpr uint32_t autosavePeriodSec(AutosavePeriod unit, uint16_t interval)
{
    switch (unit) {
        case AutosavePeriod::Minute: return static_cast<uint32_t>(interval) * kSecondsPerMinute;
        case AutosavePeriod::Day:    return static_cast<uint32_t>(interval) * kSecondsPerDay;
        case AutosavePeriod::Hour:   break;
    }
    return static_cast<uint32_t>(interval) * kSecondsPerHour;
}

/// @brief The unit stored under this index; anything out of range means hours,
///        so a value written by another firmware cannot stop the timer.
constexpr AutosavePeriod autosavePeriodFromIndex(uint8_t index)
{
    return index < kAutosavePeriodCount ? static_cast<AutosavePeriod>(index)
                                        : AutosavePeriod::Hour;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTOSAVE_PERIOD_H
