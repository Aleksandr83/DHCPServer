#ifndef DHCP_TIME_TIMEMATH_H
#define DHCP_TIME_TIMEMATH_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace time {

/**
 * @brief A broken-down calendar date/time in UTC (or plain local) fields.
 */
struct DateTime {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

/**
 * @brief Pure date/time arithmetic — no ESP-IDF, no C library time zone data.
 *
 * Kept free of platform dependencies on purpose: the logic is exercised by
 * unit tests (and can be compiled/run on a host), while the rest of the time
 * module stays hardware-bound. Uses the days-from-civil algorithm, which has
 * no leap-year or epoch pitfalls and works for the whole 1970..2100 range.
 */
class TimeMath {
public:
    /** @brief Supported year range (keeps the result inside uint32 seconds). */
    static constexpr int kMinYear = 1970;
    static constexpr int kMaxYear = 2100;

    /** @brief Calendar and clock bounds (a time is 00:00:00..23:59:59). */
    static constexpr int kMonthsPerYear = 12;
    static constexpr int kMaxHour   = 23;
    static constexpr int kMaxMinute = 59;
    static constexpr int kMaxSecond = 59;

    /** @brief The scale a timestamp is counted in. */
    static constexpr int64_t kSecondsPerMinute = 60;
    static constexpr int64_t kSecondsPerHour   = 3600;
    static constexpr int64_t kSecondsPerDay    = 86400;

    /** @brief Gregorian leap-year rule. */
    static bool isLeapYear(int year);

    /** @brief Days in a month (0 for an invalid month). */
    static int daysInMonth(int year, int month);

    /** @brief True when the Y-M-D triple is a real calendar date. */
    static bool isValidDate(int year, int month, int day);

    /** @brief True when the H:M:S triple is inside 00:00:00..23:59:59. */
    static bool isValidTime(int hour, int minute, int second);

    /**
     * @brief Parse "YYYY-MM-DD HH:MM:SS" (also accepts "T" as the separator
     *        and a missing ":SS" part, which the browser date/time inputs and
     *        the ISO-8601 form may produce).
     * @param text Input, must fill the whole string (no surrounding spaces).
     * @param out  Parsed value; untouched on failure.
     * @return true on success (the fields are also range-checked).
     */
    static bool parseDateTime(const std::string& text, DateTime& out);

    /**
     * @brief Days-since-epoch seconds for a calendar date/time treated as UTC.
     *
     * The caller is responsible for converting local time to UTC (subtract the
     * timezone offset in seconds) before storing the result on the device.
     */
    static uint32_t toUnixSec(const DateTime& dt);

    /** @brief Inverse of toUnixSec() — used for display and by the tests. */
    static DateTime fromUnixSec(uint32_t unixSec);

    /** @brief Format as "YYYY-MM-DD HH:MM:SS". */
    static std::string format(const DateTime& dt);

private:
    /** @brief Parse exactly @p count ASCII digits; false on any other char. */
    static bool parseDigits(const char* text, int count, int& out);
};

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_TIMEMATH_H
