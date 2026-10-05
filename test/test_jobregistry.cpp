/**
 * @file test_jobregistry.cpp
 * @brief Unit tests for JobRegistry (the list behind the scheduler page).
 *
 * Build with: pio test -e esp32dev
 *
 * Runs on a host as well — the registry needs nothing but the C++ standard
 * library, and `test/stubs/esp_log.h` stands in for the logging header:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Dapp_main=esp_test_app_main \
 *       -I test/stubs -I. test/test_jobregistry.cpp src/core/JobRegistry.cpp \
 *       host_main.cpp -o test_jobregistry
 */

#include <cstdio>
#include <string>

#include "../src/core/JobRegistry.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (string(a) != string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, string(a).c_str(), string(b).c_str()); return 1; } } while(0)

using namespace std;

using dhcp::core::IJobObserver;
using dhcp::core::JobInfo;
using dhcp::core::JobRegistry;
using dhcp::core::JobState;
using dhcp::core::JobUnit;
using dhcp::core::jobStateText;
using dhcp::core::jobUnitText;

namespace {

JobRegistry& reg()
{
    JobRegistry& r = JobRegistry::instance();
    return r;
}

/** Watches both ends the way the job log does (stage 173). */
class StubObserver : public IJobObserver {
public:
    int started = 0;
    int finished = 0;
    JobInfo lastStarted;
    JobInfo lastFinished;

    void jobStarted(const JobInfo& job) override { ++started; lastStarted = job; }
    void jobFinished(const JobInfo& job) override { ++finished; lastFinished = job; }
};

} // namespace

extern "C" {

/** A running operation reports itself, its progress and its target. */
static int test_begin_and_progress()
{
    reg().clear();

    TEST_ASSERT_TRUE(reg().begin("file_check", "jobs.file_check", "sd", 100));
    auto list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_STR_EQ(list[0].id, "file_check");
    TEST_ASSERT_STR_EQ(list[0].titleKey, "jobs.file_check");
    TEST_ASSERT_STR_EQ(list[0].arg, "sd");
    TEST_ASSERT_EQ(list[0].state, JobState::Running);
    TEST_ASSERT_EQ(list[0].total, 100u);
    TEST_ASSERT_EQ(list[0].done, 0u);
    // A caller that says nothing claims no unit: the page then shows no number
    // rather than a wrong one (stage 178).
    TEST_ASSERT_EQ(list[0].unit, JobUnit::None);
    TEST_ASSERT_EQ(list[0].percent(), 0);

    reg().progress("file_check", 30, 0, "/logs/dns.txt");
    list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_EQ(list[0].done, 30u);
    TEST_ASSERT_EQ(list[0].total, 100u);              // a 0 keeps the old total
    TEST_ASSERT_STR_EQ(list[0].detail, "/logs/dns.txt");
    TEST_ASSERT_EQ(list[0].percent(), 30);

    // Progress of an operation nobody announced changes nothing.
    reg().progress("nobody", 5, 10, "x");
    TEST_ASSERT_EQ(reg().snapshot().size(), 1u);

    // The unit is the operation's own fact (stage 178): a cache job counts
    // records, and the three texts are what travel in JSON.
    TEST_ASSERT_STR_EQ(jobUnitText(JobUnit::None), "none");
    TEST_ASSERT_STR_EQ(jobUnitText(JobUnit::Bytes), "bytes");
    TEST_ASSERT_STR_EQ(jobUnitText(JobUnit::Records), "records");

    reg().clear();
    reg().begin("cache_save", "jobs.cache_save", "", 50, JobUnit::Records);
    list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_EQ(list[0].unit, JobUnit::Records);

    reg().clear();
    return 0;
}

/** A one-off operation leaves the list the moment it ends. */
static int test_finish_removes_one_off()
{
    reg().clear();

    reg().begin("cache_save", "jobs.cache_save", "", 4096);
    TEST_ASSERT_TRUE(reg().contains("cache_save"));
    reg().progress("cache_save", 4096, 0, "cache.dat");
    reg().finish("cache_save", JobState::Done);
    TEST_ASSERT_FALSE(reg().contains("cache_save"));
    TEST_ASSERT_EQ(reg().snapshot().size(), 0u);

    // Also when it failed or was stopped.
    reg().begin("format", "jobs.format", "sd");
    reg().finish("format", JobState::Failed, "no card");
    TEST_ASSERT_EQ(reg().snapshot().size(), 0u);

    reg().begin("upload", "jobs.upload", "/big.bin", 100);
    reg().finish("upload", JobState::Cancelled);
    TEST_ASSERT_EQ(reg().snapshot().size(), 0u);

    reg().clear();
    return 0;
}

/** An operation with a scheduled repeat keeps its record after finishing. */
static int test_repeat_stays()
{
    reg().clear();

    TEST_ASSERT_TRUE(reg().begin("auto_check", "jobs.file_check", "sd", 10,
                                 JobUnit::None, 3600));
    reg().progress("auto_check", 10, 0, "");
    reg().finish("auto_check", JobState::Done);

    auto list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_EQ(list[0].state, JobState::Done);
    TEST_ASSERT_EQ(list[0].repeatSec, 3600u);
    TEST_ASSERT_EQ(list[0].done, 10u);          // a finished job reads 100 %

    reg().clear();
    return 0;
}

/** A paused operation is not finished: the record waits with it. */
static int test_pause_keeps_record()
{
    reg().clear();

    reg().begin("upload", "jobs.upload", "/movie.mkv", 1048576);
    reg().progress("upload", 589824, 0, "/movie.mkv");
    reg().pause("upload", "/movie.mkv");

    auto list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_EQ(list[0].state, JobState::Paused);
    TEST_ASSERT_EQ(list[0].done, 589824u);
    TEST_ASSERT_EQ(list[0].percent(), 56);

    reg().finish("upload", JobState::Cancelled);
    TEST_ASSERT_EQ(reg().snapshot().size(), 0u);

    reg().clear();
    return 0;
}

/** A pause of known length counts down; work and the end of the operation clear
    it (stage 181 — the scheduler draws those seconds instead of "running"). */
static int test_pause_with_seconds()
{
    reg().clear();

    reg().begin("cache_autoupdate", "jobs.cache_autoupdate", "", 2000, JobUnit::Records);
    reg().progress("cache_autoupdate", 150, 0, "example.com");
    TEST_ASSERT_EQ(reg().snapshot()[0].pauseSec, 0u);       // working, not pausing

    // The sweep announces the pause between two blocks with its length.
    reg().pause("cache_autoupdate", "", 60);
    auto list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_EQ(list[0].state, JobState::Paused);
    TEST_ASSERT_EQ(list[0].pauseSec, 60u);
    TEST_ASSERT_STR_EQ(list[0].detail, "example.com");       // the step is kept

    // Once a second it says the same thing again: only the number moves.
    reg().pause("cache_autoupdate", "", 59);
    list = reg().snapshot();
    TEST_ASSERT_EQ(list[0].state, JobState::Paused);
    TEST_ASSERT_EQ(list[0].pauseSec, 59u);
    TEST_ASSERT_EQ(list[0].done, 150u);

    // The first record of the next block is the end of the pause.
    reg().progress("cache_autoupdate", 151, 0, "example.org");
    list = reg().snapshot();
    TEST_ASSERT_EQ(list[0].state, JobState::Running);
    TEST_ASSERT_EQ(list[0].pauseSec, 0u);
    TEST_ASSERT_STR_EQ(list[0].detail, "example.org");

    // A pause nobody timed (an upload waiting for the operator) stays at zero.
    reg().clear();
    reg().begin("upload", "jobs.upload", "/a.bin", 100);
    reg().pause("upload", "/a.bin");
    list = reg().snapshot();
    TEST_ASSERT_EQ(list[0].state, JobState::Paused);
    TEST_ASSERT_EQ(list[0].pauseSec, 0u);

    // Ending the operation drops the pause with it; a repeating record stays.
    reg().pause("upload", "", 30);
    TEST_ASSERT_EQ(reg().snapshot()[0].pauseSec, 30u);
    reg().finish("upload", JobState::Cancelled);
    TEST_ASSERT_EQ(reg().snapshot().size(), 0u);

    reg().begin("auto", "jobs.cache_autoupdate", "", 10, JobUnit::None, 3600);
    reg().pause("auto", "", 45);
    reg().finish("auto", JobState::Done);
    list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_EQ(list[0].state, JobState::Done);
    TEST_ASSERT_EQ(list[0].pauseSec, 0u);

    reg().clear();
    return 0;
}

/** One id is one operation: a resume takes its own entry over. */
static int test_single_flight_per_id()
{
    reg().clear();

    reg().begin("upload", "jobs.upload", "/a.bin", 100);
    reg().progress("upload", 50, 0, "/a.bin");
    TEST_ASSERT_TRUE(reg().requestCancel("upload"));
    TEST_ASSERT_TRUE(reg().cancelRequested("upload"));

    // The upload is resumed: same id, fresh numbers, no leftover cancel flag.
    TEST_ASSERT_TRUE(reg().begin("upload", "jobs.upload", "/a.bin", 200));
    auto list = reg().snapshot();
    TEST_ASSERT_EQ(list.size(), 1u);
    TEST_ASSERT_EQ(list[0].total, 200u);
    TEST_ASSERT_EQ(list[0].done, 0u);
    TEST_ASSERT_FALSE(list[0].cancelRequested);

    reg().clear();
    return 0;
}

/** Cancelling is a request: every unfinished operation takes one. */
static int test_cancel_requests()
{
    reg().clear();

    // A paused transfer is unfinished — the operator may want to drop it rather
    // than continue it, so the request is accepted.
    reg().begin("upload", "jobs.upload", "/movie.mkv", 100);
    reg().progress("upload", 40, 0, "/movie.mkv");
    reg().pause("upload", "/movie.mkv");
    TEST_ASSERT_TRUE(reg().requestCancel("upload"));
    TEST_ASSERT_TRUE(reg().cancelRequested("upload"));
    reg().finish("upload", JobState::Cancelled);
    TEST_ASSERT_FALSE(reg().contains("upload"));

    // There is no "cannot be cancelled" kind any more: everything the list holds
    // is unfinished, which is what the button on the scheduler page relies on.
    reg().begin("cache_save", "jobs.cache_save", "");
    TEST_ASSERT_TRUE(reg().requestCancel("cache_save"));
    TEST_ASSERT_TRUE(reg().cancelRequested("cache_save"));

    reg().begin("file_check", "jobs.file_check", "fat");
    TEST_ASSERT_TRUE(reg().requestCancel("file_check"));
    TEST_ASSERT_TRUE(reg().requestCancel("file_check"));    // idempotent
    TEST_ASSERT_TRUE(reg().cancelRequested("file_check"));

    TEST_ASSERT_FALSE(reg().requestCancel("nobody"));

    reg().finish("file_check", JobState::Cancelled);
    TEST_ASSERT_FALSE(reg().cancelRequested("file_check"));  // record is gone

    // An operation that ended but stays in the list (it repeats) has nothing
    // left to stop, so the request is refused.
    reg().begin("auto", "jobs.file_check", "sd", 10, JobUnit::None, 3600);
    reg().finish("auto", JobState::Done);
    TEST_ASSERT_TRUE(reg().contains("auto"));
    TEST_ASSERT_FALSE(reg().requestCancel("auto"));

    reg().clear();
    return 0;
}

/** The list is bounded; a finished record makes room for the next one. */
static int test_capacity()
{
    reg().clear();

    char id[16];
    for (size_t i = 0; i < JobRegistry::kMaxJobs; i++) {
        snprintf(id, sizeof(id), "job%u", static_cast<unsigned>(i));
        TEST_ASSERT_TRUE(reg().begin(id, "jobs.x"));
    }
    TEST_ASSERT_EQ(reg().snapshot().size(), JobRegistry::kMaxJobs);
    TEST_ASSERT_FALSE(reg().begin("overflow", "jobs.x"));

    reg().finish("job0", JobState::Done);
    TEST_ASSERT_TRUE(reg().begin("later", "jobs.x"));
    TEST_ASSERT_EQ(reg().snapshot().size(), JobRegistry::kMaxJobs);

    reg().clear();
    return 0;
}

/** Percentages and state text travel to the UI as they are. */
static int test_percent_and_state_text()
{
    JobInfo info;
    TEST_ASSERT_EQ(info.percent(), -1);          // total unknown
    info.total = 3;
    info.done = 1;
    TEST_ASSERT_EQ(info.percent(), 33);
    info.done = 4;                               // more than promised
    TEST_ASSERT_EQ(info.percent(), 100);

    TEST_ASSERT_STR_EQ(jobStateText(JobState::Running), "running");
    TEST_ASSERT_STR_EQ(jobStateText(JobState::Paused), "paused");
    TEST_ASSERT_STR_EQ(jobStateText(JobState::Done), "done");
    TEST_ASSERT_STR_EQ(jobStateText(JobState::Failed), "failed");
    TEST_ASSERT_STR_EQ(jobStateText(JobState::Cancelled), "cancelled");
    return 0;
}

/**
 * The two ends of an operation are reported to the subscriber (stage 173), and
 * nothing else is: the job log writes one line per end, so progress must not
 * reach it.
 */
static int test_observer_reports_both_ends()
{
    reg().clear();
    StubObserver obs;
    reg().setObserver(&obs);

    TEST_ASSERT_TRUE(reg().begin("upload", "jobs.upload", "sd", 500));
    TEST_ASSERT_EQ(obs.started, 1);
    TEST_ASSERT_EQ(obs.finished, 0);
    TEST_ASSERT_STR_EQ(obs.lastStarted.id, "upload");
    TEST_ASSERT_STR_EQ(obs.lastStarted.titleKey, "jobs.upload");
    TEST_ASSERT_STR_EQ(obs.lastStarted.arg, "sd");
    TEST_ASSERT_EQ(obs.lastStarted.total, 500u);
    TEST_ASSERT_EQ(obs.lastStarted.state, JobState::Running);

    // Progress is deliberately not reported — only the two ends are.
    reg().progress("upload", 100, 0, "part");
    TEST_ASSERT_EQ(obs.started, 1);
    TEST_ASSERT_EQ(obs.finished, 0);

    reg().finish("upload", JobState::Done, "500 files");
    TEST_ASSERT_EQ(obs.finished, 1);
    TEST_ASSERT_STR_EQ(obs.lastFinished.id, "upload");
    TEST_ASSERT_EQ(obs.lastFinished.state, JobState::Done);
    TEST_ASSERT_STR_EQ(obs.lastFinished.detail, "500 files");
    TEST_ASSERT_EQ(obs.lastFinished.done, 500u);      // Done brings the count up

    // A failure is reported the same way, with its own state and words.
    reg().begin("format", "jobs.format", "sdcard");
    reg().finish("format", JobState::Failed, "no card");
    TEST_ASSERT_EQ(obs.finished, 2);
    TEST_ASSERT_EQ(obs.lastFinished.state, JobState::Failed);
    TEST_ASSERT_STR_EQ(obs.lastFinished.arg, "sdcard");
    TEST_ASSERT_STR_EQ(obs.lastFinished.detail, "no card");

    // Unsubscribing stops the reports for good.
    reg().setObserver(nullptr);
    reg().begin("check", "jobs.file_check");
    reg().finish("check", JobState::Cancelled);
    TEST_ASSERT_EQ(obs.started, 2);
    TEST_ASSERT_EQ(obs.finished, 2);

    reg().clear();
    return 0;
}

} // extern "C"

extern "C" int app_main(void)
{
    struct { const char* name; int (*fn)(void); } tests[] = {
        { "begin and progress", test_begin_and_progress },
        { "finish removes a one-off", test_finish_removes_one_off },
        { "repeat stays", test_repeat_stays },
        { "pause keeps the record", test_pause_keeps_record },
        { "pause with seconds", test_pause_with_seconds },
        { "single flight per id", test_single_flight_per_id },
        { "cancel requests", test_cancel_requests },
        { "capacity", test_capacity },
        { "percent and state text", test_percent_and_state_text },
        { "observer reports both ends", test_observer_reports_both_ends },
    };

    int failed = 0;
    for (auto& t : tests) {
        if (t.fn() != 0) {
            printf("  -> test '%s' FAILED\n", t.name);
            failed++;
        }
    }

    if (failed == 0) {
        printf("All JobRegistry tests PASSED!\n");
        return 0;
    }
    printf("%d JobRegistry test(s) FAILED\n", failed);
    return 1;
}
