/**
 * @file test_fileshellpath.cpp
 * @brief Unit tests for the path policy of the SSH file commands.
 *
 * The shell may only touch the two volumes the device exposes — `/fat` and the
 * card at `/sdcard`. Everything a person types is turned into an absolute path by
 * this policy, and the rules are the whole sandbox: a relative path is read from
 * the session's current directory, `..` stops at the volume root instead of
 * climbing out of it, and an absolute path has to name a volume. A mistake here is
 * a command that reaches a partition it was never meant to see, so the policy is
 * pure and this test is the proof.
 *
 * There is nothing to compare against in the past: the header did not exist before
 * this change, so the test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_fileshellpath.cpp host_main.cpp -o test_fileshellpath
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>

#include "../src/net/FileShellPath.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using namespace dhcp::net;

namespace {

/** A fresh shell stands in the internal volume. */
int test_the_starting_point()
{
    FileShellPath path;
    TEST_ASSERT_STREQ(path.current(), "/fat");
    return 0;
}

/** A relative name is read from the current directory. */
int test_a_relative_name()
{
    FileShellPath path;
    TEST_ASSERT_TRUE(path.changeTo("/fat/logs"));

    TEST_ASSERT_STREQ(path.resolve("SSH.log"), "/fat/logs/SSH.log");
    TEST_ASSERT_STREQ(path.resolve("./SSH.log"), "/fat/logs/SSH.log");
    TEST_ASSERT_STREQ(path.resolve("a/../SSH.log"), "/fat/logs/SSH.log");
    // A trailing slash is what a directory looks like when it is typed.
    TEST_ASSERT_STREQ(path.resolve("sub/"), "/fat/logs/sub");
    return 0;
}

/** An absolute path has to name a volume: there is nothing else to name. */
int test_an_absolute_path()
{
    FileShellPath path;
    string why;

    TEST_ASSERT_STREQ(path.resolve("/fat/cache.dat"), "/fat/cache.dat");
    TEST_ASSERT_STREQ(path.resolve("/sdcard"), "/sdcard");
    TEST_ASSERT_STREQ(path.resolve("/sdcard/log/Archive"), "/sdcard/log/Archive");

    // `/logs` is not a volume: the sandbox has exactly two.
    TEST_ASSERT_STREQ(path.resolve("/logs", &why), "");
    TEST_ASSERT_TRUE(!why.empty());
    TEST_ASSERT_STREQ(path.resolve("/", &why), "");
    return 0;
}

/**
 * `..` in the root of a volume is refused, not clamped: the operator's rule is
 * that a path may not leave its volume, and silently reaching a different place
 * than the one typed is worse than saying no.
 */
int test_the_volume_root_is_a_ceiling()
{
    FileShellPath path;
    string why;

    TEST_ASSERT_STREQ(path.resolve("..", &why), "");
    TEST_ASSERT_TRUE(!why.empty());
    TEST_ASSERT_STREQ(path.resolve("../..", &why), "");
    TEST_ASSERT_STREQ(path.resolve("/fat/..", &why), "");
    TEST_ASSERT_STREQ(path.resolve("/fat/../nvs", &why), "");
    TEST_ASSERT_TRUE(!why.empty());

    // Inside a volume the step up is ordinary: only the root is a ceiling.
    TEST_ASSERT_TRUE(path.changeTo("/fat/logs"));
    TEST_ASSERT_STREQ(path.resolve(".."), "/fat");
    TEST_ASSERT_STREQ(path.resolve("/fat/logs/.."), "/fat");
    return 0;
}

/** `cd` moves only when the target is inside a volume. */
int test_the_change_of_directory()
{
    FileShellPath path;
    string why;

    TEST_ASSERT_TRUE(path.changeTo("/sdcard/log", &why));
    TEST_ASSERT_STREQ(path.current(), "/sdcard/log");

    TEST_ASSERT_TRUE(path.changeTo("..", &why));
    TEST_ASSERT_STREQ(path.current(), "/sdcard");

    // At the root of a volume there is nowhere up to go, and the refusal leaves
    // the shell where it was.
    TEST_ASSERT_TRUE(!path.changeTo("..", &why));
    TEST_ASSERT_STREQ(path.current(), "/sdcard");
    TEST_ASSERT_TRUE(!why.empty());

    TEST_ASSERT_TRUE(!path.changeTo("/nvs", &why));
    TEST_ASSERT_STREQ(path.current(), "/sdcard");
    TEST_ASSERT_TRUE(!why.empty());
    return 0;
}

/** An empty name means the current directory, and control bytes are refused. */
int test_the_odd_names()
{
    FileShellPath path;
    string why;

    TEST_ASSERT_STREQ(path.resolve(""), "/fat");
    TEST_ASSERT_STREQ(path.resolve("", &why), "/fat");

    TEST_ASSERT_STREQ(path.resolve("a\tb", &why), "");
    TEST_ASSERT_TRUE(!why.empty());
    TEST_ASSERT_STREQ(path.resolve(string("a\0b", 3), &why), "");
    return 0;
}

/** The volume of a path is what `df` and the prompt need. */
int test_the_volume_name()
{
    TEST_ASSERT_STREQ(FileShellPath::volumeOf("/fat/logs"), "/fat");
    TEST_ASSERT_STREQ(FileShellPath::volumeOf("/sdcard"), "/sdcard");
    TEST_ASSERT_STREQ(FileShellPath::volumeOf("/fat"), "/fat");
    TEST_ASSERT_STREQ(FileShellPath::volumeOf("/nvs"), "");
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running FileShellPath tests...\n");
    int failures = 0;

    failures += test_the_starting_point();
    failures += test_a_relative_name();
    failures += test_an_absolute_path();
    failures += test_the_volume_root_is_a_ceiling();
    failures += test_the_change_of_directory();
    failures += test_the_odd_names();
    failures += test_the_volume_name();

    if (failures == 0) {
        printf("All FileShellPath tests PASSED!\n");
    } else {
        printf("Some FileShellPath tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
