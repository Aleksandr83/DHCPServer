/**
 * @file test_time.cpp
 * @brief Unit tests for TimeMath (pure date/time arithmetic).
 *
 * Build with: pio test -e esp32dev
 *
 * The class under test has no ESP-IDF dependency, so the same tests can also
 * be compiled and run on a host:
 *   g++ -std=c++17 -Dapp_main=esp_test_app_main -I. \
 *       test/test_time.cpp src/time/TimeMath.cpp host_main.cpp -o test_time
 */

#include <cstdio>
#include <cassert>
#include <string>

// Include the code under test (adjust path as needed for test build)
#include "../src/time/TimeMath.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (string(a) != string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, string(a).c_str(), string(b).c_str()); return 1; } } while(0)

using namespace std;

using dhcp::time::DateTime;
using dhcp::time::TimeMath;

extern "C" {

static int test_leap_year()
{
    TEST_ASSERT_TRUE(TimeMath::isLeapYear(2024));
    TEST_ASSERT_TRUE(TimeMath::isLeapYear(2000));
    TEST_ASSERT_FALSE(TimeMath::isLeapYear(2023));
    TEST_ASSERT_FALSE(TimeMath::isLeapYear(1900));   // divisible by 100, not 400
    TEST_ASSERT_FALSE(TimeMath::isLeapYear(2100));
    TEST_ASSERT_EQ(TimeMath::daysInMonth(2024, 2), 29);
    TEST_ASSERT_EQ(TimeMath::daysInMonth(2023, 2), 28);
    return 0;
}

static int test_valid_date()
{
    TEST_ASSERT_TRUE(TimeMath::isValidDate(2026, 9, 11));
    TEST_ASSERT_TRUE(TimeMath::isValidDate(2024, 2, 29));
    TEST_ASSERT_FALSE(TimeMath::isValidDate(2023, 2, 29));
    TEST_ASSERT_FALSE(TimeMath::isValidDate(2026, 4, 31));
    TEST_ASSERT_FALSE(TimeMath::isValidDate(2026, 13, 1));
    TEST_ASSERT_FALSE(TimeMath::isValidDate(2026, 0, 1));
    TEST_ASSERT_FALSE(TimeMath::isValidDate(2026, 9, 0));
    TEST_ASSERT_FALSE(TimeMath::isValidDate(1969, 12, 31));  // before epoch
    TEST_ASSERT_FALSE(TimeMath::isValidDate(2101, 1, 1));    // too far
    return 0;
}

static int test_valid_time()
{
    TEST_ASSERT_TRUE(TimeMath::isValidTime(0, 0, 0));
    TEST_ASSERT_TRUE(TimeMath::isValidTime(23, 59, 59));
    TEST_ASSERT_FALSE(TimeMath::isValidTime(24, 0, 0));
    TEST_ASSERT_FALSE(TimeMath::isValidTime(0, 60, 0));
    TEST_ASSERT_FALSE(TimeMath::isValidTime(0, 0, 60));
    return 0;
}

static int test_parse_ok()
{
    DateTime dt;
    TEST_ASSERT_TRUE(TimeMath::parseDateTime("2026-09-11 18:32:05", dt));
    TEST_ASSERT_EQ(dt.year, 2026);
    TEST_ASSERT_EQ(dt.month, 9);
    TEST_ASSERT_EQ(dt.day, 11);
    TEST_ASSERT_EQ(dt.hour, 18);
    TEST_ASSERT_EQ(dt.minute, 32);
    TEST_ASSERT_EQ(dt.second, 5);

    // ISO-8601 'T' separator and a missing seconds part
    TEST_ASSERT_TRUE(TimeMath::parseDateTime("2026-09-11T18:32:05", dt));
    TEST_ASSERT_EQ(dt.second, 5);
    TEST_ASSERT_TRUE(TimeMath::parseDateTime("2026-09-11 18:32", dt));
    TEST_ASSERT_EQ(dt.second, 0);
    TEST_ASSERT_EQ(dt.hour, 18);

    // Epoch and upper bound of the supported range
    TEST_ASSERT_TRUE(TimeMath::parseDateTime("1970-01-01 00:00:00", dt));
    TEST_ASSERT_TRUE(TimeMath::parseDateTime("2100-12-31 23:59:59", dt));

    printf("Parsed: %s\n", TimeMath::format(dt).c_str());
    return 0;
}

static int test_parse_fail()
{
    DateTime dt;
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026-09-11", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026-09-11 18:32:05 ", dt)); // trailing space
    TEST_ASSERT_FALSE(TimeMath::parseDateTime(" 2026-09-11 18:32:05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026-09-11 18-32-05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026/09/11 18:32:05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("26-09-11 18:32:05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026-09-11 25:32:05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026-09-31 18:32:05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026-13-11 18:32:05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2023-02-29 18:32:05", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("abcd-ef-gh ij:kl:mn", dt));
    TEST_ASSERT_FALSE(TimeMath::parseDateTime("2026-09-11 18:32:0a", dt));
    return 0;
}

static int test_to_unix_sec()
{
    DateTime dt;

    TEST_ASSERT_TRUE(TimeMath::parseDateTime("1970-01-01 00:00:00", dt));
    TEST_ASSERT_EQ(TimeMath::toUnixSec(dt), 0u);

    TEST_ASSERT_TRUE(TimeMath::parseDateTime("1970-01-02 00:00:00", dt));
    TEST_ASSERT_EQ(TimeMath::toUnixSec(dt), 86400u);

    TEST_ASSERT_TRUE(TimeMath::parseDateTime("2000-03-01 00:00:00", dt));
    TEST_ASSERT_EQ(TimeMath::toUnixSec(dt), 951868800u);   // leap year 2000

    TEST_ASSERT_TRUE(TimeMath::parseDateTime("2026-09-11 18:32:05", dt));
    TEST_ASSERT_EQ(TimeMath::toUnixSec(dt), 1789151525u);

    // Round-trip over a wide range, including leap days
    const uint32_t probes[] = {0u, 86400u, 951868800u, 1789151525u,
                               2147483647u, 1546300800u, 4102444800u};
    for (uint32_t probe : probes) {
        const DateTime back = TimeMath::fromUnixSec(probe);
        TEST_ASSERT_EQ(TimeMath::toUnixSec(back), probe);
    }
    return 0;
}

static int test_format_and_back()
{
    DateTime dt;
    TEST_ASSERT_TRUE(TimeMath::parseDateTime("2026-09-11 18:32:05", dt));
    TEST_ASSERT_STR_EQ(TimeMath::format(dt), "2026-09-11 18:32:05");

    const DateTime back = TimeMath::fromUnixSec(TimeMath::toUnixSec(dt));
    TEST_ASSERT_STR_EQ(TimeMath::format(back), "2026-09-11 18:32:05");

    dt = DateTime{1970, 1, 1, 0, 0, 0};
    TEST_ASSERT_STR_EQ(TimeMath::format(dt), "1970-01-01 00:00:00");
    return 0;
}

void app_main()
{
    printf("Running TimeMath tests...\n");
    int failures = 0;

    failures += test_leap_year();
    failures += test_valid_date();
    failures += test_valid_time();
    failures += test_parse_ok();
    failures += test_parse_fail();
    failures += test_to_unix_sec();
    failures += test_format_and_back();

    if (failures == 0) {
        printf("All TimeMath tests PASSED!\n");
    } else {
        printf("Some TimeMath tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
