/**
 * @file test_restartsaveverify.cpp
 * @brief Unit tests for the "write, read it back, try exactly once more" policy
 *        (stage 169).
 *
 * The operator's rule for the files a planned restart keeps is one sentence
 * long: *if the content does not match, save it once more; if it still does not
 * match, ask me*. Every part of that sentence is checkable without a board, and
 * two of them are exactly what this project has got wrong before — a step that
 * stayed silent (stage 120) and a verdict that did not describe the file it was
 * about (stage 117). So the policy lives in a class free of ESP-IDF and is
 * checked here against a scripted volume.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_restartsaveverify.cpp src/dns/RestartSaveVerify.cpp \
 *       -o test_restartsaveverify
 */
#include "src/dns/RestartSaveVerify.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace std;

using dhcp::dns::RestartSaveVerify;
using AttemptResult = RestartSaveVerify::AttemptResult;
using Outcome = RestartSaveVerify::Outcome;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const string& what)
{
    ++g_checks;
    if (ok) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        ++g_failed;
    }
}

/**
 * @brief A scripted volume: every attempt answers what the script says.
 *
 * It records what the policy asked for (the attempt numbers) and what it
 * reported, because both are part of the contract: "one retry" is a promise
 * about the number of writes, and the report is what reaches the error log.
 */
struct Volume {
    vector<AttemptResult> script;    ///< The results, in order of the attempts
    vector<int> asked;               ///< Attempt numbers the policy handed out
    vector<AttemptResult> reported;  ///< What the report callback saw
    vector<string> reasons;          ///< ...and the reason of each attempt
    size_t next = 0;

    RestartSaveVerify::Attempt attempt()
    {
        return [this](int n, string& why) {
            asked.push_back(n);
            const AttemptResult r =
                (next < script.size()) ? script[next++] : AttemptResult::Failed;
            if (r != AttemptResult::Ok) why = "reason of attempt " + to_string(n);
            return r;
        };
    }

    RestartSaveVerify::Report report()
    {
        return [this](int, AttemptResult r, const string& why) {
            reported.push_back(r);
            reasons.push_back(why);
        };
    }
};

/** A save that worked is not repeated: one write, and no noise. */
static void test_one_attempt_when_it_works()
{
    printf("a save that verifies is not repeated\n");
    Volume v;
    v.script = {AttemptResult::Ok};

    const Outcome outcome = RestartSaveVerify::run(v.attempt(), v.report());
    check(outcome == Outcome::Ok, "the outcome is ok");
    check(v.asked.size() == 1 && v.asked[0] == 1, "exactly one attempt was made");
    check(v.reported.size() == 1 && v.reported[0] == AttemptResult::Ok,
          "and the log was told about it");
    check(v.reasons[0].empty(), "a successful attempt has no reason to give");
}

/** The rule itself: a mismatch is written once more — and then it is over. */
static void test_mismatch_is_saved_once_more()
{
    printf("a mismatch is saved once more\n");
    Volume v;
    v.script = {AttemptResult::Mismatch, AttemptResult::Ok};

    const Outcome outcome = RestartSaveVerify::run(v.attempt(), v.report());
    check(outcome == Outcome::Ok, "the retry succeeded, so the outcome is ok");
    check(v.asked.size() == 2 && v.asked[0] == 1 && v.asked[1] == 2,
          "the second attempt was told it is the second one");
    check(v.reported.size() == 2 && v.reported[0] == AttemptResult::Mismatch &&
              v.reported[1] == AttemptResult::Ok,
          "both attempts reached the log, in the order they happened");
    check(v.reasons[0] == "reason of attempt 1",
          "with the first attempt's own words: " + v.reasons[0]);
}

/** One retry means one: a third write is what the operator did not ask for. */
static void test_there_is_no_third_attempt()
{
    printf("there is no third attempt\n");
    Volume v;
    v.script = {AttemptResult::Failed, AttemptResult::Failed, AttemptResult::Ok};

    const Outcome outcome = RestartSaveVerify::run(v.attempt(), v.report());
    check(outcome == Outcome::Failed, "two failures end the policy");
    check(v.asked.size() == 2, "and only two attempts were made (" +
                                   to_string(v.asked.size()) + ")");
    check(v.reported.size() == 2, "both of them were reported");
}

/** The verdict is the volume as it is now — that is, the last attempt's answer. */
static void test_the_verdict_describes_the_volume_now()
{
    printf("the verdict describes the volume as it is now\n");
    {
        Volume v;
        v.script = {AttemptResult::Mismatch, AttemptResult::Failed};
        check(RestartSaveVerify::run(v.attempt(), v.report()) == Outcome::Failed,
              "a mismatch followed by a failed write leaves no usable file: failed");
    }
    {
        Volume v;
        v.script = {AttemptResult::Failed, AttemptResult::Mismatch};
        check(RestartSaveVerify::run(v.attempt(), v.report()) == Outcome::Mismatch,
              "a failed write followed by a mismatch leaves a wrong file: mismatch");
    }
    {
        Volume v;
        v.script = {AttemptResult::Mismatch, AttemptResult::Mismatch};
        check(RestartSaveVerify::run(v.attempt(), v.report()) == Outcome::Mismatch,
              "two mismatches stay a mismatch: the page asks a different question");
    }
}

/** A policy without an attempt is a failed attempt, not a crash or a silence. */
static void test_a_missing_attempt_is_a_failure()
{
    printf("no attempt at all\n");
    Volume v;
    const Outcome outcome = RestartSaveVerify::run(RestartSaveVerify::Attempt(), v.report());
    check(outcome == Outcome::Failed, "an empty attempt counts as failed");
    check(v.reported.size() == static_cast<size_t>(RestartSaveVerify::kAttempts) &&
              v.reported[0] == AttemptResult::Failed && v.reported[1] == AttemptResult::Failed,
          "and the log is still told about every attempt, so the step cannot stay silent");
}

/** The words the log and the page show. */
static void test_names()
{
    printf("names\n");
    check(string(RestartSaveVerify::attemptName(AttemptResult::Ok)) == "ok", "attempt ok");
    check(string(RestartSaveVerify::attemptName(AttemptResult::Mismatch)) == "mismatch",
          "attempt mismatch");
    check(string(RestartSaveVerify::attemptName(AttemptResult::Failed)) == "failed",
          "attempt failed");
    check(string(RestartSaveVerify::outcomeName(Outcome::Mismatch)) == "mismatch",
          "outcome mismatch");
}

int main()
{
    test_one_attempt_when_it_works();
    test_mismatch_is_saved_once_more();
    test_there_is_no_third_attempt();
    test_the_verdict_describes_the_volume_now();
    test_a_missing_attempt_is_a_failure();
    test_names();

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
