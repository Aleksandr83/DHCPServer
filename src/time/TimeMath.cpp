#include "TimeMath.h"

#include <cstdio>

// ─────────────────────────────────────────────────────
// Calendar arithmetic
// ─────────────────────────────────────────────────────
//
// Nothing here depends on the C library's time zone tables or on a leap-year
// table: a date is turned into "days since 1970-01-01" and back with Howard
// Hinnant's days-from-civil algorithm, which is exact over the whole supported
// range (kMinYear..kMaxYear) and has no "year 2100 is not a leap year" trap.
//
// The trick of that algorithm is to count the year from March instead of from
// January. Done that way the leap day is the last day of the year, every month
// starts on the same offset inside a 153-day group of five months, and the day
// of the year needs no lookup table at all. On top of that a 400-year era holds
// exactly 146097 days, so the era, the year inside it and the day inside it can
// be divided out one after another, in both directions.

using namespace std;

namespace dhcp {
namespace time {

namespace {

// Rule 39: the Gregorian rules and the vocabulary of the calendar the
// days-from-civil algorithm below is written in.
constexpr int kLeapYearCycle     = 4;   // a leap year every four years...
constexpr int kLeapYearSkip      = 100; // ...except every hundred...
constexpr int kLeapYearFullCycle = 400; // ...but not every four hundred
constexpr int kFirstMonthOfYear  = 1;
constexpr int kFebruary          = 2;   // whose length depends on the year
constexpr int kFebruaryLeapDays  = 29;
constexpr int kDayNumberBase     = 1;   // a day of a month counts from one
constexpr int kDecimalBase       = 10;  // the base parseDigits() accumulates in

// Rule 39: days-from-civil works in 400-year eras and 153-day groups of five
// months. These numbers are the arithmetic of the calendar itself, not limits
// to tune, so each one is named by what it counts.
constexpr int64_t kEraDays         = 146097; // days in one 400-year era
constexpr uint32_t kEraDaysLast    = 146096; // the last day index inside an era
constexpr int64_t kEraYears        = 400;
constexpr int64_t kEraYearShift    = 399;    // keeps the era division right below 0
constexpr int     kCommonYearDays  = 365;
constexpr uint32_t kFourYearDays   = 1460;   // 4*365, the /4 correction of an era
constexpr uint32_t kCenturyDays    = 36524;  // 100*365 + 24, its /100 correction
constexpr int64_t kCivilEpochDays  = 719468; // 0000-03-01 -> 1970-01-01
constexpr int     kCycleDays       = 153;    // five whole months
constexpr int     kCycleMonths     = 5;
constexpr int     kCycleRound      = 2;      // rounds the two cycle divisions
constexpr int     kMarchShift      = 3;      // March is month 0 of the shifted year
constexpr int     kMonthShiftBack  = -9;     // January and February close the year
constexpr int     kMonthBeforeWrap = 10;     // the shifted year wraps after this index
constexpr int     kShiftedYearBump = 1;      // Jan and Feb belong to the next year

/** Days from 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
    // Count the year from March: January and February belong to the previous
    // one, which is exactly what puts the leap day at the end of the year.
    year -= month <= kFebruary;

    // Which 400-year era this year falls into, and where inside it. Subtracting
    // the shift before dividing keeps the era right for years below zero too.
    const int64_t era = (year >= 0 ? year : year - kEraYearShift) / kEraYears;
    const unsigned yoe = static_cast<unsigned>(year - era * kEraYears);    // [0, 399]

    // Day of the shifted year: whole 153-day groups of five months (the months
    // of the shifted year all start on the same offset inside such a group),
    // plus the days of the leading months of the group, plus the day of the
    // month. Note the day is counted from zero here, so what comes out is the
    // number of days *before* this one.
    const unsigned doy = (kCycleDays * (month + (month > kFebruary
                            ? -kMarchShift : TimeMath::kMonthsPerYear - kMarchShift))
                          + kCycleRound) / kCycleMonths
                         + day - kDayNumberBase;

    // Day of the era: the whole years, plus one day for each leap year — "every
    // four years, except every hundred, but not every four hundred" — plus the
    // day of the year. Leap days need no other handling than these two terms.
    const unsigned doe = yoe * kCommonYearDays + yoe / kLeapYearCycle -
                         yoe / kLeapYearSkip + doy;                        // [0, 146096]

    // All the days of the eras before this one, then the days inside it, then
    // back from the algorithmic zero year (0000-03-01) to the Unix epoch.
    return era * kEraDays + static_cast<int64_t>(doe) - kCivilEpochDays;
}

// Rule 39: the only two shapes of an ISO-8601 date/time text that are read,
// and where every field and separator sits inside them.
constexpr size_t kIsoDateLen = 16;         // "YYYY-MM-DD HH:MM"
constexpr size_t kIsoDateTimeLen = 19;     // "YYYY-MM-DD HH:MM:SS"
constexpr size_t kSepDate1 = 4;            // after the year
constexpr size_t kSepDate2 = 7;            // after the month
constexpr size_t kSepTime = 10;            // between the date and the time
constexpr size_t kSepMinute = 13;          // after the hour
constexpr size_t kSepSecond = 16;          // after the minute
constexpr size_t kPosYear = 0;
constexpr size_t kPosMonth = 5;
constexpr size_t kPosDay = 8;
constexpr size_t kPosHour = 11;
constexpr size_t kPosMinute = 14;
constexpr size_t kPosSecond = 17;
constexpr int kDigitsYear = 4;
constexpr int kDigitsTwo = 2;
constexpr size_t kIsoTextBytes = 32;       // fits the longest form with slack

/** Inverse of daysFromCivil(): civil date for days since 1970-01-01. */
void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day)
{
    // Move the origin to the algorithmic zero year (0000-03-01), so that the
    // same era / year-of-era / day-of-era arithmetic applies in this direction.
    days += kCivilEpochDays;

    // Which era this day falls into and where inside it. The shift before the
    // division is the mirror image of the one in daysFromCivil().
    const int64_t era = (days >= 0 ? days : days - kEraDaysLast) / kEraDays;
    const unsigned doe = static_cast<unsigned>(days - era * kEraDays);     // [0, 146096]

    // Day of era -> year of era: divide by the average length of a year, then
    // correct for the leap days that the division swallowed. These corrections
    // are the same 4 / 100 / 400 rules as above, read backwards: 1460 is four
    // years, 36524 is a hundred years, 146096 an era without its last day.
    const unsigned yoe = (doe - doe / kFourYearDays + doe / kCenturyDays -
                          doe / kEraDaysLast) / kCommonYearDays;
    const int64_t y = static_cast<int64_t>(yoe) + era * kEraYears;

    // What is left inside the era is the day of the March-based year: first the
    // leap days of the years that have passed, then those whole years.
    const unsigned doy = doe - (kCommonYearDays * yoe + yoe / kLeapYearCycle -
                                yoe / kLeapYearSkip);                      // [0, 365]

    // Day of year -> month, with the same 153-day group of five months as
    // daysFromCivil(), only this time read as a division with a remainder.
    const unsigned mp = (kCycleMonths * doy + kCycleRound) / kCycleDays;   // [0, 11]

    // The remainder inside the group is the day of the month — counting from
    // one again — and the month index becomes a calendar month: the indices
    // below the wrap point are March..December, the last two are January and
    // February of the next calendar year.
    day = doy - (kCycleDays * mp + kCycleRound) / kCycleMonths
          + kDayNumberBase;                                                // [1, 31]
    month = mp + (mp < kMonthBeforeWrap ? kMarchShift : kMonthShiftBack);  // [1, 12]

    // A March-based year is one ahead of the calendar year for those two
    // months, which are the only ones that ever moved.
    year = static_cast<int>(y + (month <= kFebruary ? kShiftedYearBump : 0));
}

} // namespace

// ─────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────

bool TimeMath::isLeapYear(int year)
{
    // The Gregorian rule: every fourth year, except every hundredth — but the
    // four-hundredth is a leap year again.
    return (year % kLeapYearCycle == 0 && year % kLeapYearSkip != 0) ||
           (year % kLeapYearFullCycle == 0);
}

int TimeMath::daysInMonth(int year, int month)
{
    // February is the only month whose length depends on the year, so the table
    // carries its common length and the leap year is handled below.
    static const int kDays[kMonthsPerYear] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < kFirstMonthOfYear || month > kMonthsPerYear) return 0;
    if (month == kFebruary && isLeapYear(year)) return kFebruaryLeapDays;
    return kDays[month - kFirstMonthOfYear];
}

bool TimeMath::isValidDate(int year, int month, int day)
{
    // The year range is limited so that the resulting Unix seconds always fit
    // into a uint32_t, which is what the rest of the firmware stores.
    if (year < kMinYear || year > kMaxYear) return false;
    const int dim = daysInMonth(year, month);
    return dim != 0 && day >= kDayNumberBase && day <= dim;
}

bool TimeMath::isValidTime(int hour, int minute, int second)
{
    // A plain 24-hour clock: no leap second, and no "24:00:00" as a special
    // case for midnight.
    return hour >= 0 && hour <= kMaxHour &&
           minute >= 0 && minute <= kMaxMinute &&
           second >= 0 && second <= kMaxSecond;
}

// ─────────────────────────────────────────────────────
// Parsing / formatting
// ─────────────────────────────────────────────────────

bool TimeMath::parseDigits(const char* text, int count, int& out)
{
    // Exactly |count| decimal digits, accumulated by hand. The text comes from
    // a web form, so it is not trusted to be anything in particular.
    int value = 0;
    for (int i = 0; i < count; ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') return false;
        value = value * kDecimalBase + (ch - '0');
    }
    out = value;
    return true;
}

bool TimeMath::parseDateTime(const string& text, DateTime& out)
{
    // Accepted forms: "YYYY-MM-DD HH:MM" (16) and "YYYY-MM-DD HH:MM:SS" (19),
    // with a space or 'T' between the date and the time (ISO-8601 writes the
    // 'T', the input fields of the web interface produce the space).
    //
    // The separators are checked by position before anything is read: every
    // length below is therefore known to be inside the string.
    const size_t len = text.size();
    if (len != kIsoDateLen && len != kIsoDateTimeLen) return false;
    if (text[kSepDate1] != '-' || text[kSepDate2] != '-') return false;
    if (text[kSepTime] != ' ' && text[kSepTime] != 'T') return false;
    if (text[kSepMinute] != ':') return false;
    if (len == kIsoDateTimeLen && text[kSepSecond] != ':') return false;

    // Every field is read at its own fixed position. The seconds exist only
    // in the longer form, which is why the last value stays zero otherwise.
    DateTime dt;
    if (!parseDigits(text.c_str() + kPosYear, kDigitsYear, dt.year)) return false;
    if (!parseDigits(text.c_str() + kPosMonth, kDigitsTwo, dt.month)) return false;
    if (!parseDigits(text.c_str() + kPosDay, kDigitsTwo, dt.day)) return false;
    if (!parseDigits(text.c_str() + kPosHour, kDigitsTwo, dt.hour)) return false;
    if (!parseDigits(text.c_str() + kPosMinute, kDigitsTwo, dt.minute)) return false;
    if (len == kIsoDateTimeLen &&
        !parseDigits(text.c_str() + kPosSecond, kDigitsTwo, dt.second)) return false;

    // Shape is not enough: a well-formed but impossible date ("2025-02-30")
    // has to be rejected here, before the value reaches the clock.
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
    // Seconds = whole days since the epoch * seconds per day + the time of day.
    // The time of day is added in three steps so that each product stays small
    // and the whole sum fits comfortably into 64 bits.
    const int64_t days = daysFromCivil(dt.year, static_cast<unsigned>(dt.month),
                                       static_cast<unsigned>(dt.day));
    const int64_t secs = days * kSecondsPerDay + dt.hour * kSecondsPerHour +
                         dt.minute * kSecondsPerMinute + dt.second;
    return static_cast<uint32_t>(secs);
}

DateTime TimeMath::fromUnixSec(uint32_t unixSec)
{
    // Split the timestamp into whole days and the seconds inside the day. The
    // day count goes through civilFromDays(), the rest is divisions.
    int64_t secs = static_cast<int64_t>(unixSec);
    int64_t days = secs / kSecondsPerDay;
    int64_t rem = secs % kSecondsPerDay;
    if (rem < 0) {
        // Never taken for the unsigned input this function is given today: it
        // is kept so that the split stays correct (both parts non-negative) if
        // it is ever called with a signed, pre-epoch value.
        rem += kSecondsPerDay;
        days -= 1;
    }

    DateTime dt;
    unsigned month = 1;
    unsigned day = 1;
    civilFromDays(days, dt.year, month, day);
    dt.month = static_cast<int>(month);
    dt.day = static_cast<int>(day);
    dt.hour = static_cast<int>(rem / kSecondsPerHour);
    dt.minute = static_cast<int>((rem % kSecondsPerHour) / kSecondsPerMinute);
    dt.second = static_cast<int>(rem % kSecondsPerMinute);
    return dt;
}

string TimeMath::format(const DateTime& dt)
{
    // "YYYY-MM-DD HH:MM:SS" — the form the REST API accepts back and the one
    // the web interface displays.
    char buf[kIsoTextBytes];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    return string(buf);
}

} // namespace time
} // namespace dhcp
