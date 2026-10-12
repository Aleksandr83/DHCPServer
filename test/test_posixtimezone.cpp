/**
 * @file test_posixtimezone.cpp
 * @brief Unit tests for the `TZ` value built from the time zone offset (stage 186).
 *
 * The project never set `TZ`, so `localtime()` was UTC while the pages showed
 * UTC+offset — the difference this rule exists to close. Its one dangerous
 * property is the inverted sign of POSIX, and a wrong sign is invisible: both
 * spellings set a zone, and the mistake only shows as stamps that are wrong by
 * twice the offset. That is what these tests are for.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_posixtimezone.cpp -o test_posixtimezone
 */
#include "src/time/PosixTimezone.h"

#include <cstdio>
#include <string>

using namespace std;

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

/** The sign is the whole point: east of Greenwich is a minus in POSIX. */
static void test_sign_is_inverted()
{
    printf("the sign\n");
    check(dhcp::time::posixTimezone(3) == "UTC-3",
          "Moscow (UTC+3) is UTC-3 in POSIX: " + dhcp::time::posixTimezone(3));
    check(dhcp::time::posixTimezone(-5) == "UTC+5",
          "New York (UTC-5) is UTC+5: " + dhcp::time::posixTimezone(-5));
    check(dhcp::time::posixTimezone(1) == "UTC-1", "UTC+1 is UTC-1");
    check(dhcp::time::posixTimezone(-1) == "UTC+1", "UTC-1 is UTC+1");
}

/** Zero has a form of its own, and it carries no sign at all. */
static void test_zero()
{
    printf("zero\n");
    check(dhcp::time::posixTimezone(0) == "UTC0",
          "zero is UTC0, not UTC-0: " + dhcp::time::posixTimezone(0));
}

/** Every offset the settings can hold has to produce a usable value. */
static void test_the_whole_range()
{
    printf("the range of the setting (-12…+14)\n");
    bool allNumeric = true;
    for (int hours = -12; hours <= 14; ++hours) {
        const string tz = dhcp::time::posixTimezone(hours);
        // "UTC", then an optional sign (only when the offset is not zero), then
        // the absolute value as digits.
        const size_t digits = tz.find_first_of("0123456789");
        if (digits == string::npos) { allNumeric = false; break; }
        if (tz.substr(0, 3) != "UTC") { allNumeric = false; break; }
        const int value = stoi(tz.substr(digits));
        const int want = hours == 0 ? 0 : (hours < 0 ? -hours : hours);
        if (value != want) { allNumeric = false; break; }
        if (hours == 0 && tz != "UTC0") { allNumeric = false; break; }
        if (hours != 0 && tz.find_first_of("+-") == string::npos) {
            allNumeric = false;
            break;
        }
    }
    check(allNumeric, "-12…+14 all give UTC with the absolute value");
    check(dhcp::time::posixTimezone(-12) == "UTC+12", "the western limit");
    check(dhcp::time::posixTimezone(14) == "UTC-14", "the eastern limit");
}

/** Two different zones must never produce the same value. */
static void test_zones_do_not_collide()
{
    printf("no two offsets share a value\n");
    bool distinct = true;
    for (int a = -12; a <= 14 && distinct; ++a) {
        for (int b = a + 1; b <= 14; ++b) {
            if (dhcp::time::posixTimezone(a) == dhcp::time::posixTimezone(b)) {
                distinct = false;
                printf("       %d and %d both give %s\n", a, b,
                       dhcp::time::posixTimezone(a).c_str());
                break;
            }
        }
    }
    check(distinct, "each offset has its own TZ value");
}

int main()
{
    test_sign_is_inverted();
    test_zero();
    test_the_whole_range();
    test_zones_do_not_collide();

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
