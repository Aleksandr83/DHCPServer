#include "TimeMath.h"

#include <cstdio>

namespace dhcp {
namespace time {

namespace {

/** Days from 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);          // [0, 399]
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/** Inverse of daysFromCivil(): civil date for days since 1970-01-01. */
void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);       // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);          // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                              // [0, 11]
    day = doy - (153 * mp + 2) / 5 + 1;                                   // [1, 31]
    month = mp + (mp < 10 ? 3 : -9);                                      // [1, 12]
    year = static_cast<int>(y + (month <= 2 ? 1 : 0));
}

} // namespace

// ─────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────

bool TimeMath::isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int TimeMath::daysInMonth(int year, int month)
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && isLeapYear(year)) return 29;
    return kDays[month - 1];
}

bool TimeMath::isValidDate(int year, int month, int day)
{
    if (year < kMinYear || year > kMaxYear) return false;
    const int dim = daysInMonth(year, month);
    return dim != 0 && day >= 1 && day <= dim;
}

bool TimeMath::isValidTime(int hour, int minute, int second)
{
    return hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 59;
}

// ─────────────────────────────────────────────────────
// Parsing / formatting
// ─────────────────────────────────────────────────────

bool TimeMath::parseDigits(const char* text, int count, int& out)
{
    int value = 0;
    for (int i = 0; i < count; ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + (ch - '0');
    }
    out = value;
    return true;
}

bool TimeMath::parseDateTime(const std::string& text, DateTime& out)
{
    // Accepted forms: "YYYY-MM-DD HH:MM" (16) and "YYYY-MM-DD HH:MM:SS" (19),
    // the date/time separator being a space or 'T'.
    const size_t len = text.size();
    if (len != 16 && len != 19) return false;
    if (text[4] != '-' || text[7] != '-') return false;
    if (text[10] != ' ' && text[10] != 'T') return false;
    if (text[13] != ':') return false;
    if (len == 19 && text[16] != ':') return false;

    DateTime dt;
    if (!parseDigits(text.c_str() + 0, 4, dt.year)) return false;
    if (!parseDigits(text.c_str() + 5, 2, dt.month)) return false;
    if (!parseDigits(text.c_str() + 8, 2, dt.day)) return false;
    if (!parseDigits(text.c_str() + 11, 2, dt.hour)) return false;
    if (!parseDigits(text.c_str() + 14, 2, dt.minute)) return false;
    if (len == 19 && !parseDigits(text.c_str() + 17, 2, dt.second)) return false;

    if (!isValidDate(dt.year, dt.month, dt.day)) return false;
    if (!isValidTime(dt.hour, dt.minute, dt.second)) return false;

    out = dt;
    return true;
}

// ─────────────────────────────────────────────────────
// Conversion
// ─────────────────────────────────────────────────────

uint32_t TimeMath::toUnixSec(const DateTime& dt)
{
    const int64_t days = daysFromCivil(dt.year, static_cast<unsigned>(dt.month),
                                       static_cast<unsigned>(dt.day));
    const int64_t secs = days * 86400 + dt.hour * 3600 + dt.minute * 60 + dt.second;
    return static_cast<uint32_t>(secs);
}

DateTime TimeMath::fromUnixSec(uint32_t unixSec)
{
    int64_t secs = static_cast<int64_t>(unixSec);
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    DateTime dt;
    unsigned month = 1;
    unsigned day = 1;
    civilFromDays(days, dt.year, month, day);
    dt.month = static_cast<int>(month);
    dt.day = static_cast<int>(day);
    dt.hour = static_cast<int>(rem / 3600);
    dt.minute = static_cast<int>((rem % 3600) / 60);
    dt.second = static_cast<int>(rem % 60);
    return dt;
}

std::string TimeMath::format(const DateTime& dt)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    return std::string(buf);
}

} // namespace time
} // namespace dhcp
