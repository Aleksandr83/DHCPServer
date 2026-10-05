/**
 * @file test_autoupdateplan.cpp
 * @brief Host test for the auto-update bounds (stages 172 and 177, rule 23).
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

    // The countdown re-arms when the sweep turns on, and when the period
    // changes; applying the same period again keeps the remaining time (stage
    // 176 — every DNS settings save reaches the timer through the same call).
    assert(autoUpdateReArmNeeded(false, 0, 3600) == true);    // first enable
    assert(autoUpdateReArmNeeded(true, 3600, 3600) == false); // same period
    assert(autoUpdateReArmNeeded(true, 3600, 7200) == true);  // period grew
    assert(autoUpdateReArmNeeded(true, 7200, 3600) == true);  // period shrank
    assert(autoUpdateReArmNeeded(false, 3600, 3600) == true); // re-enable
    // Equal seconds are one period whatever the unit spelling is: 60 minutes
    // and 1 hour both mean 3600 s, so neither must restart the countdown.
    assert(autoUpdateReArmNeeded(true, 86400, 86400) == false);

    // A sweep the operator asks for is allowed only when the cycle itself would
    // do something, and a refusal names the reason (stage 177). The reasons are
    // the guards runCycle() already follows, so the button can refuse in words
    // instead of promising a sweep that walks nothing.
    using Refusal = AutoUpdateRunNowRefusal;
    assert(autoUpdateRunNowRefusal(true, true, true, false) == Refusal::None);
    assert(autoUpdateRunNowRefusal(false, true, true, false) == Refusal::NotEnabled);
    assert(autoUpdateRunNowRefusal(true, false, true, false) == Refusal::IgnoreTtlOff);
    assert(autoUpdateRunNowRefusal(true, true, false, false) == Refusal::Unavailable);
    assert(autoUpdateRunNowRefusal(true, true, true, true) == Refusal::AlreadyRunning);
    // Several reasons at once keep the order of the switches the operator sees:
    // a device with everything off says "auto update is off" rather than blaming
    // Ignore TTL or the cache.
    assert(autoUpdateRunNowRefusal(false, false, false, true) == Refusal::NotEnabled);
    assert(autoUpdateRunNowRefusal(true, false, false, true) == Refusal::IgnoreTtlOff);
    assert(autoUpdateRunNowRefusal(true, true, false, true) == Refusal::Unavailable);

    // A sweep walks the whole cache in blocks (stage 180): the block count is what
    // one "update" costs, and the configured period counts between sweeps.
    assert(autoUpdateBlockCount(0, 50) == 0);       // nothing to walk
    assert(autoUpdateBlockCount(1, 50) == 1);
    assert(autoUpdateBlockCount(50, 50) == 1);      // an exact fit is one block
    assert(autoUpdateBlockCount(51, 50) == 2);      // a part-filled last block
    assert(autoUpdateBlockCount(2000, 50) == 40);   // the operator's own case
    assert(autoUpdateBlockCount(2000, 1000) == 2);
    assert(autoUpdateBlockCount(7, 0) == 0);        // no batch, no block

    // The pause sits between blocks: the last one must not send the task to sleep
    // for the whole pause before the countdown is re-armed.
    assert(autoUpdatePauseAfterBlock(0, 1) == false);
    assert(autoUpdatePauseAfterBlock(0, 3) == true);
    assert(autoUpdatePauseAfterBlock(1, 3) == true);
    assert(autoUpdatePauseAfterBlock(2, 3) == false);

    // The worst case still fits the 32-bit second countdown the task keeps.
    assert(autosavePeriodSec(AutosavePeriod::Day,
                             autoUpdateIntervalMax(AutosavePeriod::Day)) <= 0xFFFFFFFFu);

    printf("All AutoUpdatePlan tests PASSED!\n");
    return 0;
}
