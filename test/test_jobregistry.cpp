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

using dhcp::core::JobInfo;
using dhcp::core::JobRegistry;
using dhcp::core::JobState;
using dhcp::core::jobStateText;

namespace {

JobRegistry& reg()
{
    JobRegistry& r = JobRegistry::instance();
    return r;
}

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

    TEST_ASSERT_TRUE(reg().begin("auto_check", "jobs.file_check", "sd", 10, 3600));
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
    reg().begin("auto", "jobs.file_check", "sd", 10, 3600);
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

} // extern "C"

extern "C" int app_main(void)
{
    struct { const char* name; int (*fn)(void); } tests[] = {
        { "begin and progress", test_begin_and_progress },
        { "finish removes a one-off", test_finish_removes_one_off },
        { "repeat stays", test_repeat_stays },
        { "pause keeps the record", test_pause_keeps_record },
        { "single flight per id", test_single_flight_per_id },
        { "cancel requests", test_cancel_requests },
        { "capacity", test_capacity },
        { "percent and state text", test_percent_and_state_text },
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
