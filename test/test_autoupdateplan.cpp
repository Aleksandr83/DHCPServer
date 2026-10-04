/**
 * @file test_autoupdateplan.cpp
 * @brief Host test for the auto-update bounds (stage 172, rule 23).
 *
 * The header has no ESP-IDF dependency on purpose, so the numbers the timer uses
 * — and the bounds the page offers — can be checked on the development machine
 * rather than on the board.
 */

#include "core/AutoUpdatePlan.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace std;

using namespace dhcp::core;

static_assert(kAutoUpdateBatchMin == 1, "the batch lower bound changed");
static_assert(kAutoUpdateBatchMax == 1000, "the batch upper bound changed");
static_assert(kAutoUpdatePauseMinSec == 1, "the pause lower bound changed");
static_assert(kAutoUpdatePauseMaxSec == 3600, "the pause upper bound changed");
static_assert(kAutoUpdateDaysMax == 30, "the days bound changed");

int main()
{
    // The interval bound is the next unit up, exactly as the page offers it:
    // 24 hours in a day, 30 "sweep days" — deliberately not a calendar month,
    // because a refresh sweep is not a file growing stale.
    assert(autoUpdateIntervalMax(AutosavePeriod::Hour) == 24);
    assert(autoUpdateIntervalMax(AutosavePeriod::Day) == 30);
    // Minutes are not offered by the auto-update page; that unit falls back to
    // the hour bound rather than inventing one.
    assert(autoUpdateIntervalMax(AutosavePeriod::Minute) == 24);

    // A value outside the bounds is brought inside them, never rejected: the
    // settings must survive a device that once accepted something else.
    assert(autoUpdateClampInterval(AutosavePeriod::Hour, 0) == 1);
    assert(autoUpdateClampInterval(AutosavePeriod::Hour, 24) == 24);
    assert(autoUpdateClampInterval(AutosavePeriod::Hour, 25) == 24);
    assert(autoUpdateClampInterval(AutosavePeriod::Day, 0) == 1);
    assert(autoUpdateClampInterval(AutosavePeriod::Day, 30) == 30);
    assert(autoUpdateClampInterval(AutosavePeriod::Day, 31) == 30);

    assert(autoUpdateClampBatch(0) == 1);
    assert(autoUpdateClampBatch(1) == 1);
    assert(autoUpdateClampBatch(50) == 50);
    assert(autoUpdateClampBatch(1000) == 1000);
    assert(autoUpdateClampBatch(1001) == 1000);

    assert(autoUpdateClampPause(0) == 1);
    assert(autoUpdateClampPause(1) == 1);
    assert(autoUpdateClampPause(60) == 60);
    assert(autoUpdateClampPause(3600) == 3600);
    assert(autoUpdateClampPause(3601) == 3600);

    // The worst case still fits the 32-bit second countdown the task keeps.
    assert(autosavePeriodSec(AutosavePeriod::Day,
                             autoUpdateIntervalMax(AutosavePeriod::Day)) <= 0xFFFFFFFFu);

    printf("All AutoUpdatePlan tests PASSED!\n");
    return 0;
}
