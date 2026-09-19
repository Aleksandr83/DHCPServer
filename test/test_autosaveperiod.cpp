/**
 * @file test_autosaveperiod.cpp
 * @brief Host test for the autosave period rules (stages 153/154, rule 23).
 *
 * The header has no ESP-IDF dependency on purpose, so the numbers the timer uses
 * — and the bounds the page offers — can be checked on the development machine
 * rather than on the board.
 */

#include "core/AutosavePeriod.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace dhcp::core;

static_assert(kSecondsPerMinute == 60, "a minute changed");
static_assert(kSecondsPerHour == 3600, "an hour changed");
static_assert(kSecondsPerDay == 86400, "a day changed");
static_assert(kAutosavePeriodCount == 3, "the number of units changed");
static_assert(kMinutesPerHour == 60, "the minutes bound changed");
static_assert(kHoursPerDay == 24, "the hours bound changed");

int main()
{
    // The stored indices are 0, 1, 2. Stage 153 stored hour = 0 and day = 1, and
    // those two keep their meaning; minutes took the index the month left.
    assert(static_cast<uint8_t>(AutosavePeriod::Hour) == 0);
    assert(static_cast<uint8_t>(AutosavePeriod::Day) == 1);
    assert(static_cast<uint8_t>(AutosavePeriod::Minute) == 2);
    assert(autosavePeriodFromIndex(0) == AutosavePeriod::Hour);
    assert(autosavePeriodFromIndex(1) == AutosavePeriod::Day);
    assert(autosavePeriodFromIndex(2) == AutosavePeriod::Minute);
    assert(autosavePeriodFromIndex(3) == AutosavePeriod::Hour);
    assert(autosavePeriodFromIndex(255) == AutosavePeriod::Hour);

    // The bound is the next unit up: 60 minutes, 24 hours, and for days the
    // length of the month that is running.
    assert(autosaveIntervalMax(AutosavePeriod::Minute, 0) == 60);
    assert(autosaveIntervalMax(AutosavePeriod::Hour, 0) == 24);
    assert(autosaveIntervalMax(AutosavePeriod::Day, 0) == 31);   // month unknown
    assert(autosaveIntervalMax(AutosavePeriod::Day, 28) == 28);  // February
    assert(autosaveIntervalMax(AutosavePeriod::Day, 30) == 30);  // September
    assert(autosaveIntervalMax(AutosavePeriod::Day, 31) == 31);  // January
    assert(autosaveIntervalMax(AutosavePeriod::Day, 40) == 31);  // not a month

    // A value outside the bounds is brought inside them, never rejected: the
    // settings must survive a device that once accepted something else.
    assert(autosaveClampInterval(AutosavePeriod::Minute, 0, 0) == 1);
    assert(autosaveClampInterval(AutosavePeriod::Minute, 61, 0) == 60);
    assert(autosaveClampInterval(AutosavePeriod::Minute, 60, 0) == 60);
    assert(autosaveClampInterval(AutosavePeriod::Hour, 0, 0) == 1);
    assert(autosaveClampInterval(AutosavePeriod::Hour, 25, 0) == 24);
    assert(autosaveClampInterval(AutosavePeriod::Hour, 24, 0) == 24);
    assert(autosaveClampInterval(AutosavePeriod::Day, 0, 28) == 1);
    assert(autosaveClampInterval(AutosavePeriod::Day, 31, 28) == 28);
    assert(autosaveClampInterval(AutosavePeriod::Day, 30, 30) == 30);
    assert(autosaveClampInterval(AutosavePeriod::Day, 31, 0) == 31);

    // Seconds: the interval times the unit.
    assert(autosavePeriodSec(AutosavePeriod::Minute, 1) == 60);
    assert(autosavePeriodSec(AutosavePeriod::Minute, 60) == 3600);
    assert(autosavePeriodSec(AutosavePeriod::Hour, 1) == 3600);
    assert(autosavePeriodSec(AutosavePeriod::Hour, 24) == 86400);
    assert(autosavePeriodSec(AutosavePeriod::Day, 1) == 86400);
    assert(autosavePeriodSec(AutosavePeriod::Day, 30) == 2592000);

    // The worst case of the range still fits the 32-bit countdown, and one unit
    // up is worth exactly its own number of the smaller ones.
    assert(autosavePeriodSec(AutosavePeriod::Day, autosaveIntervalMax(AutosavePeriod::Day, 31))
           <= 0xFFFFFFFFu);
    assert(autosavePeriodSec(AutosavePeriod::Minute, kMinutesPerHour) ==
           autosavePeriodSec(AutosavePeriod::Hour, 1));
    assert(autosavePeriodSec(AutosavePeriod::Hour, kHoursPerDay) ==
           autosavePeriodSec(AutosavePeriod::Day, 1));

    std::printf("All AutosavePeriod tests PASSED!\n");
    return 0;
}
