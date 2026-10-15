/**
 * @file test_logfilesizes.cpp
 * @brief Unit tests for the sizes a file log can be given (stage 204).
 *
 * The operator asked for a "log file size" setting with 64 KB (the default),
 * 128 KB and 256 KB, on the two pages that own a file log. A value like that
 * arrives from three places that can each be wrong in their own way - the
 * settings page, an NVS entry written by an older firmware, and an imported
 * settings file typed by a human - and it ends up as the cap a writer rotates at.
 * So the rule "which of the offered sizes does this mean" is one function, and
 * this file is where it is pinned: a chosen size survives, anything else falls
 * back to the default rather than becoming a cap of zero (a log that rotates on
 * every line) or of four gigabytes (a log that fills the volume).
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_logfilesizes.cpp -o test_logfilesizes
 */
#include "src/core/LogFileSizes.h"

#include <cstdio>
#include <string>

using namespace std;

using dhcp::core::kLogFileDefaultMaxBytes;
using dhcp::core::kLogFileSizeCount;
using dhcp::core::kLogFileSizes;
using dhcp::core::logFileSizeIndex;
using dhcp::core::logFileSizeOrDefault;
using dhcp::core::logFileSizeValid;

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

static void test_the_offered_sizes()
{
    check(kLogFileSizeCount == 3, "three sizes are offered");
    check(kLogFileSizes[0] == 64u * 1024u, "the first is the 64 KB the operator asked for");
    check(kLogFileSizes[1] == 128u * 1024u, "the second is 128 KB");
    check(kLogFileSizes[2] == 256u * 1024u, "and the third 256 KB");
    check(kLogFileSizes[0] < kLogFileSizes[1] && kLogFileSizes[1] < kLogFileSizes[2],
          "they are in ascending order, so the combobox reads as a scale");
    // The one thing that must not drift: a device that has never been given a
    // size rotates at the same cap the first entry of the combobox shows, or the
    // page would tell the operator something about a file it does not do.
    check(kLogFileSizes[0] == kLogFileDefaultMaxBytes,
          "the default of the combobox is the writer's own default cap");
}

static void test_valid_sizes()
{
    check(logFileSizeValid(kLogFileSizes[0]), "64 KB is a size the operator can choose");
    check(logFileSizeValid(kLogFileSizes[1]), "so is 128 KB");
    check(logFileSizeValid(kLogFileSizes[2]), "and 256 KB");
    check(!logFileSizeValid(0), "zero is not: a cap of zero rotates on every line");
    check(!logFileSizeValid(1), "nor is one byte");
    check(!logFileSizeValid(100u * 1024u), "nor a plausible size nobody offers");
    check(!logFileSizeValid(kLogFileSizes[2] + 1), "nor one just past the largest");
    check(!logFileSizeValid(static_cast<size_t>(-1)),
          "nor a value that would come out of a negative number read as unsigned");
}

static void test_the_stored_value_means()
{
    // Absent (NVS before the setting existed) is 0, and means "the default".
    check(logFileSizeOrDefault(0) == kLogFileSizes[0], "a setting that was never written is 64 KB");
    check(logFileSizeOrDefault(-1) == kLogFileSizes[0],
          "a negative value from a damaged entry is 64 KB, not a huge cap");
    check(logFileSizeOrDefault(100 * 1024) == kLogFileSizes[0],
          "a value nobody offers is replaced, not honoured");
    check(logFileSizeOrDefault(1024 * 1024 * 1024LL) == kLogFileSizes[0],
          "a cap that would fill the volume is replaced too");

    check(logFileSizeOrDefault(64 * 1024) == kLogFileSizes[0], "64 KB stays 64 KB");
    check(logFileSizeOrDefault(128 * 1024) == kLogFileSizes[1], "128 KB stays 128 KB");
    check(logFileSizeOrDefault(256 * 1024) == kLogFileSizes[2], "256 KB stays 256 KB");

    check(logFileSizeOrDefault(128 * 1024) != logFileSizeOrDefault(0),
          "a chosen size and the default are different answers, not the same one twice");
}

static void test_index_for_the_page()
{
    check(logFileSizeIndex(kLogFileSizes[0]) == 0, "64 KB is the first entry of the combobox");
    check(logFileSizeIndex(kLogFileSizes[1]) == 1, "128 KB the second");
    check(logFileSizeIndex(kLogFileSizes[2]) == 2, "and 256 KB the third");
    check(logFileSizeIndex(0) == 0, "an unknown size selects the default rather than nothing");
    check(logFileSizeIndex(777) == 0, "and so does a size nobody offers");
}

int main()
{
    printf("Running LogFileSizes tests...\n");
    test_the_offered_sizes();
    test_valid_sizes();
    test_the_stored_value_means();
    test_index_for_the_page();

    if (g_failed == 0) {
        printf("All %d LogFileSizes checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d LogFileSizes checks FAILED!\n", g_failed, g_checks);
    return 1;
}
