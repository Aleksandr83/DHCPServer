#ifndef DHCP_TIME_POSIXTIMEZONE_H
#define DHCP_TIME_POSIXTIMEZONE_H

#include <string>

namespace dhcp {
namespace time {

/**
 * @file PosixTimezone.h
 * @brief The `TZ` value that makes `localtime()` show the operator's offset
 *        (stage 186).
 *
 * The time settings hold the offset as a whole number of hours from UTC
 * (`utc_offset_hours`, -12…+14) and a zone *name* that is display-only. Nothing
 * used to reach the C library at all — `TZ` was never set anywhere in the
 * project — so `localtime()` answered UTC while the pages showed UTC+offset:
 * every log line was stamped an offset away from the clock the operator reads,
 * and the new per-sweep log file would have carried the same wrong time in its
 * name.
 *
 * POSIX inverts the sign: the value names how many hours **west** of Greenwich
 * the zone lies, so UTC+3 is `UTC-3`. A sign slipped the other way moves every
 * stamp twice as far as the mistake, and it is exactly the kind of thing no
 * compiler and no run on one machine would catch — which is why the rule is a
 * pure function with a host test instead of a line inside a setter.
 *
 * Whole hours only: the setting is an integer number of hours (that is what the
 * page and the import clamp), so there is no `UTC-5:30` form to get wrong. A
 * zone that needs one is set by its offset, and daylight saving is not
 * modelled — the offset the operator saved is the offset that is used.
 *
 * @param offsetHours Offset from UTC in whole hours (-12…+14).
 * @return The `TZ` value, e.g. `UTC-3` for Moscow, `UTC0` for zero, `UTC+5` for
 *         a zone five hours west of Greenwich.
 */
inline std::string posixTimezone(int offsetHours)
{
    if (offsetHours == 0) return "UTC0";
    const int hours = offsetHours < 0 ? -offsetHours : offsetHours;
    return std::string(offsetHours < 0 ? "UTC+" : "UTC-") +
           std::to_string(hours);
}

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_POSIXTIMEZONE_H
