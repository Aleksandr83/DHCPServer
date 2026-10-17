/**
 * @file test_localname.cpp
 * @brief Unit tests for "is this query the device's own name?" (stage 219).
 *
 * The device answers two names about itself: the bare host name (through its
 * own DNS server) and `<hostname>.local` (through that server and through mDNS).
 * DNS names are case-insensitive and may arrive with the root dot, so the rule
 * is a pure function — the DNS task, whose answer depends on it, cannot be run
 * on the host, but this decision can.
 *
 * There is nothing to compare against in the past: before this stage the header
 * did not exist, so this test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_localname.cpp host_main.cpp -o test_localname
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>

#include "../src/core/LocalName.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)

using dhcp::core::kLocalNameSuffix;
using dhcp::core::localNameMatches;

namespace {

/** The suffix is the one mDNS reserves, not a free choice. */
int test_the_suffix()
{
    TEST_ASSERT_TRUE(string(kLocalNameSuffix) == ".local");
    return 0;
}

/** Both names the device publishes: the bare one and `.local`. */
int test_the_two_names_of_the_device()
{
    TEST_ASSERT_TRUE(localNameMatches("DhcpServer", "DhcpServer"));
    TEST_ASSERT_TRUE(localNameMatches("dhcpserver.local", "DhcpServer"));
    return 0;
}

/** DNS does not distinguish case, and the root dot is part of the question. */
int test_case_and_the_root_dot()
{
    TEST_ASSERT_TRUE(localNameMatches("DHCPSERVER", "DhcpServer"));
    TEST_ASSERT_TRUE(localNameMatches("DhcpServer.", "DhcpServer"));
    TEST_ASSERT_TRUE(localNameMatches("DHCPSERVER.LOCAL.", "DhcpServer"));
    TEST_ASSERT_TRUE(localNameMatches("dhcp-server.local", "dhcp-server"));
    return 0;
}

/** A name that merely contains ours is somebody else's. */
int test_names_that_are_not_ours()
{
    TEST_ASSERT_FALSE(localNameMatches("dhcpserver.localdomain", "DhcpServer"));
    TEST_ASSERT_FALSE(localNameMatches("xdhcpserver", "DhcpServer"));
    TEST_ASSERT_FALSE(localNameMatches("dhcpserver2", "DhcpServer"));
    TEST_ASSERT_FALSE(localNameMatches("dhcpserver.local.local", "DhcpServer"));
    TEST_ASSERT_FALSE(localNameMatches("local", "DhcpServer"));
    TEST_ASSERT_FALSE(localNameMatches("", "DhcpServer"));
    TEST_ASSERT_FALSE(localNameMatches(".", "DhcpServer"));
    return 0;
}

/** A device without a name answers to none (the setting always has one, but
 *  the rule must not turn an empty string into a match for the root). */
int test_an_empty_host_name_matches_nothing()
{
    TEST_ASSERT_FALSE(localNameMatches("dhcpserver", ""));
    TEST_ASSERT_FALSE(localNameMatches("", ""));
    TEST_ASSERT_FALSE(localNameMatches(".local", ""));
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running LocalName tests...\n");
    int failures = 0;

    failures += test_the_suffix();
    failures += test_the_two_names_of_the_device();
    failures += test_case_and_the_root_dot();
    failures += test_names_that_are_not_ours();
    failures += test_an_empty_host_name_matches_nothing();

    if (failures == 0) {
        printf("All LocalName tests PASSED!\n");
    } else {
        printf("Some LocalName tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
