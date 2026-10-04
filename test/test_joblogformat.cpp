/**
 * @file test_joblogformat.cpp
 * @brief Host test for the job log's line text (stage 173, rule 23).
 *
 * `Jobs.log` promises what it will say about a long-running operation — the
 * operation, its result and how long it took — and this is where that promise is
 * checked, on the machine rather than on the board. The stamp is not here: it
 * belongs to ErrorLogCore, is shared with the error log, and is tested there.
 *
 * The header has no ESP-IDF dependency on purpose.
 */

#include "core/JobLogFormat.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace std;

using namespace dhcp::core;

static_assert(kMsPerSecond == 1000, "the millisecond unit changed");

int main()
{
    // Durations: one decimal, no float, and a sub-second operation never reads
    // as a bare "0 s".
    assert(jobDurationText(0) == "0.0 s");
    assert(jobDurationText(400) == "0.4 s");
    assert(jobDurationText(999) == "0.9 s");
    assert(jobDurationText(1000) == "1.0 s");
    assert(jobDurationText(12300) == "12.3 s");
    assert(jobDurationText(60000) == "60.0 s");

    // Started: the operation, what it works on, then "started".
    assert(jobStartedText("file_check", "sd") == "file_check (sd): started");
    assert(jobStartedText("cache_save", "") == "cache_save: started");

    // Finished: result and duration, and the operation's own words when it has
    // any.
    assert(jobFinishedText("cache_save", "", "done", 4096, "") ==
           "cache_save: finished (done, 4.0 s)");
    assert(jobFinishedText("format", "sdcard", "failed", 1230, "no card") ==
           "format (sdcard): finished (failed, 1.2 s) — no card");

    // A missing state text must not print a null pointer.
    assert(jobFinishedText("x", "", nullptr, 0, "") == "x: finished (unknown, 0.0 s)");

    printf("All JobLogFormat tests PASSED!\n");
    return 0;
}
