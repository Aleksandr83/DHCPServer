/**
 * @file test_restartsavejobstate.cpp
 * @brief Unit tests for the "write a file before the restart" job state
 *        (single-flight, the verdict the page reads, no inherited verdict).
 *
 * The three rules tested here are the ones the page depends on, and the ones
 * this project has already broken twice: a verdict that reported a save which
 * never happened (stage 117) and a step that stayed silent about what it did
 * (stage 120). Both bugs lived in exactly this kind of logic, so it lives in a
 * class free of ESP-IDF and is checked on the host.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_restartsavejobstate.cpp src/dns/RestartSaveJobState.cpp \
 *       -o test_restartsavejobstate
 */
#include "src/dns/RestartSaveJobState.h"

#include <cstdio>
#include <string>

using namespace std;

using dhcp::dns::RestartSaveJobState;
using StartResult = RestartSaveJobState::StartResult;
using Verdict = RestartSaveJobState::Verdict;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const string& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failed;
        printf("  FAIL  %s\n", what.c_str());
    }
}

static const char* name(StartResult r)
{
    switch (r) {
        case StartResult::Started: return "Started";
        case StartResult::Busy:    return "Busy";
        case StartResult::Skipped: return "Skipped";
        case StartResult::Failed:  return "Failed";
    }
    return "?";
}

static const char* name(Verdict v)
{
    switch (v) {
        case Verdict::None:    return "None";
        case Verdict::Ok:      return "Ok";
        case Verdict::Skipped: return "Skipped";
        case Verdict::Failed:  return "Failed";
    }
    return "?";
}

static void expect_start(RestartSaveJobState& s, bool enabled, StartResult want,
                         const string& what)
{
    const StartResult got = s.request(enabled);
    check(got == want, what + ": got " + name(got) + ", want " + name(want));
}

static void expect_verdict(const RestartSaveJobState& s, Verdict want,
                           const string& what)
{
    check(s.verdict() == want,
          what + ": got " + name(s.verdict()) + ", want " + name(want));
}

/** A fresh state has no verdict and no running job. */
static void test_initial()
{
    printf("initial state\n");
    RestartSaveJobState s;
    check(!s.busy(), "a fresh job state is not busy");
    expect_verdict(s, Verdict::None, "and has no verdict to report");
}

/** A request with the switch off starts nothing and says so. */
static void test_skipped()
{
    printf("switch off\n");
    RestartSaveJobState s;
    expect_start(s, false, StartResult::Skipped, "a disabled request is skipped");
    check(!s.busy(), "and leaves nothing running");
    expect_verdict(s, Verdict::Skipped, "and reports the truthful verdict");
}

/** Started → busy; a second request must not start a second writer. */
static void test_single_flight()
{
    printf("single flight\n");
    RestartSaveJobState s;
    expect_start(s, true, StartResult::Started, "the first request starts the job");
    check(s.busy(), "and the job is busy");

    expect_start(s, true, StartResult::Busy, "a second one is refused as busy");
    check(s.busy(), "while the first still runs");

    // A page polling during the run must not read the previous outcome — here
    // there is none, and after a finished run it must be cleared again.
    expect_verdict(s, Verdict::None, "no verdict while the job runs");
}

/** finish() ends the job and stores its verdict. */
static void test_finish()
{
    printf("finish\n");
    RestartSaveJobState s;
    s.request(true);
    s.finish(Verdict::Ok);
    check(!s.busy(), "a finished job is not busy");
    expect_verdict(s, Verdict::Ok, "the verdict is the one the job reported");

    s.request(true);
    expect_verdict(s, Verdict::None, "starting again clears the old verdict");
    s.finish(Verdict::Failed);
    expect_verdict(s, Verdict::Failed, "and the new verdict replaces it");
}

/** No inherited luck: Ok from the previous run is gone the moment a new starts. */
static void test_no_inherited_verdict()
{
    printf("no inherited verdict\n");
    RestartSaveJobState s;
    s.request(true);
    s.finish(Verdict::Ok);
    expect_verdict(s, Verdict::Ok, "the first run ends Ok");

    s.request(true);
    expect_verdict(s, Verdict::None,
                   "the second run must not inherit the first run's Ok");

    s.request(true);
    expect_verdict(s, Verdict::None, "and asking again changes nothing");
}

/** Turning the switch off while a job runs must not fake its outcome. */
static void test_skip_during_run()
{
    printf("switch off while running\n");
    RestartSaveJobState s;
    s.request(true);
    expect_start(s, false, StartResult::Skipped, "a request with the switch off is skipped");
    check(s.busy(), "but the running job keeps running");
    expect_verdict(s, Verdict::Skipped, "and the page is told nothing is being written");

    // The file is either on the card or it is not, and only the job knows.
    s.finish(Verdict::Ok);
    expect_verdict(s, Verdict::Ok, "the running job has the last word");
}

/** A start the owner accepted and then could not perform is a failure. */
static void test_abort_start()
{
    printf("aborted start\n");
    RestartSaveJobState s;
    s.request(true);
    s.abortStart();
    check(!s.busy(), "an aborted start leaves nothing running");
    expect_verdict(s, Verdict::Failed, "and the file was not written, so: failed");

    // ...and the next run starts from a clean slate again.
    expect_start(s, true, StartResult::Started, "the next run may start");
    expect_verdict(s, Verdict::None, "with no verdict inherited");
}

/** What a page sees when it polls across two consecutive runs. */
static void test_poll_sequence()
{
    printf("what the page reads while polling\n");
    RestartSaveJobState s;

    s.request(true);
    check(s.busy() && s.verdict() == Verdict::None, "run 1: running, no verdict");
    s.finish(Verdict::Failed);
    check(!s.busy() && s.verdict() == Verdict::Failed, "run 1: failed");

    s.request(true);
    check(s.busy() && s.verdict() == Verdict::None,
          "run 2: running again, no verdict (not the failure of run 1)");
    s.finish(Verdict::Ok);
    check(!s.busy() && s.verdict() == Verdict::Ok, "run 2: ok");
}

int main()
{
    test_initial();
    test_skipped();
    test_single_flight();
    test_finish();
    test_no_inherited_verdict();
    test_skip_during_run();
    test_abort_start();
    test_poll_sequence();

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
