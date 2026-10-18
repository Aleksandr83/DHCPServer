/**
 * @file test_sshlogformat.cpp
 * @brief Unit tests for the sentences of `SSH.log` (stage 223).
 *
 * The SSH journal is a file an operator reads after the fact, so its sentences are
 * a promise: an address, an attempt number out of the budget, how long a session
 * lasted. They are built without a timestamp (`ErrorLogCore` adds that), which is
 * what lets a host test check them.
 *
 * There is nothing to compare against in the past: before this stage the header
 * did not exist, so this test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_sshlogformat.cpp host_main.cpp -o test_sshlogformat
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>

#include "../src/core/SshLogFormat.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using namespace dhcp::core;

namespace {

/** A session opens with the address and the name that was typed. */
int test_the_session_opening()
{
    TEST_ASSERT_STREQ(sshSessionOpenText("192.168.1.5", "admin"),
                      "session opened from 192.168.1.5 as admin");
    TEST_ASSERT_STREQ(sshSessionOpenText("192.168.1.5", ""),
                      "session opened from 192.168.1.5 as (no name)");
    TEST_ASSERT_STREQ(sshSessionOpenText("unknown", "admin"),
                      "session opened from unknown as admin");
    return 0;
}

/** Closing names the address and how long the session lasted. */
int test_the_session_closing()
{
    TEST_ASSERT_STREQ(sshSessionCloseText("192.168.1.5", 0), "session closed from 192.168.1.5 after 0 s");
    TEST_ASSERT_STREQ(sshSessionCloseText("192.168.1.5", 42), "session closed from 192.168.1.5 after 42 s");
    TEST_ASSERT_STREQ(sshSessionCloseText("192.168.1.5", 3725),
                      "session closed from 192.168.1.5 after 1 h 2 min 5 s");
    // A minute exactly is minutes, not "0 h 1 min 0 s".
    TEST_ASSERT_STREQ(sshSessionCloseText("192.168.1.5", 60),
                      "session closed from 192.168.1.5 after 1 min");
    TEST_ASSERT_STREQ(sshSessionCloseText("192.168.1.5", 3600),
                      "session closed from 192.168.1.5 after 1 h");
    TEST_ASSERT_STREQ(sshSessionCloseText("192.168.1.5", 90),
                      "session closed from 192.168.1.5 after 1 min 30 s");

    // The other orderings the reader can meet.
    TEST_ASSERT_STREQ(sshDurationText(5), "5 s");
    TEST_ASSERT_STREQ(sshDurationText(59), "59 s");
    TEST_ASSERT_STREQ(sshDurationText(61), "1 min 1 s");
    TEST_ASSERT_STREQ(sshDurationText(3599), "59 min 59 s");
    TEST_ASSERT_STREQ(sshDurationText(3601), "1 h 0 min 1 s");
    TEST_ASSERT_STREQ(sshDurationText(86399), "23 h 59 min 59 s");
    return 0;
}

/** A refused password carries the attempt and the budget, exactly like Auth.log. */
int test_the_refused_password()
{
    TEST_ASSERT_STREQ(sshAuthFailedText("192.168.1.5", 1, 3),
                      "refused password from 192.168.1.5 (1 of 3)");
    TEST_ASSERT_STREQ(sshAuthFailedText("192.168.1.5", 3, 3),
                      "refused password from 192.168.1.5 (3 of 3)");
    return 0;
}

/** Two refusals that are not about a password: the subnet filter and the limit. */
int test_the_other_refusals()
{
    TEST_ASSERT_STREQ(sshSubnetRefusedText("10.0.0.7"),
                      "refused a session from 10.0.0.7: outside the own subnet");
    TEST_ASSERT_STREQ(sshSessionLimitText("192.168.1.9", 2),
                      "refused a session from 192.168.1.9: 2 of 2 already open");
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running SshLogFormat tests...\n");
    int failures = 0;

    failures += test_the_session_opening();
    failures += test_the_session_closing();
    failures += test_the_refused_password();
    failures += test_the_other_refusals();

    if (failures == 0) {
        printf("All SshLogFormat tests PASSED!\n");
    } else {
        printf("Some SshLogFormat tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
