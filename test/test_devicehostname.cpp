/**
 * @file test_devicehostname.cpp
 * @brief Unit tests for the device host name rules (stage 218).
 *
 * The name is typed on Settings ▸ General and goes two places: into NVS and into
 * the network interface (`esp_netif_set_hostname`). Neither can judge it: NVS
 * stores text as given, and lwIP's setter accepts the string it is handed. So the
 * rules are a pure header — a name is an RFC 1123 label (letters, digits and
 * hyphens, no hyphen at either end, at most 63 characters), the operator's spaces
 * around it are not part of it, and a stored value that does not pass is replaced
 * by the factory name instead of being pushed into the interface.
 *
 * There is nothing to compare against in the past: before this stage the header
 * did not exist, so this test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_devicehostname.cpp host_main.cpp -o test_devicehostname
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>

#include "../src/core/DeviceHostName.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using dhcp::core::deviceHostNameOrDefault;
using dhcp::core::isValidDeviceHostName;
using dhcp::core::kDefaultDeviceHostName;
using dhcp::core::kDeviceHostNameMaxLen;
using dhcp::core::trimmedDeviceHostName;

namespace {

/** The factory name is what a device that was never asked shows. */
int test_the_default_name()
{
    TEST_ASSERT_STREQ(kDefaultDeviceHostName, "DhcpServer");
    return 0;
}

/** Every shape the operator is likely to type. */
int test_names_that_are_allowed()
{
    TEST_ASSERT_TRUE(isValidDeviceHostName("DhcpServer"));
    TEST_ASSERT_TRUE(isValidDeviceHostName("dhcpserver"));
    TEST_ASSERT_TRUE(isValidDeviceHostName("dhcp-server"));
    TEST_ASSERT_TRUE(isValidDeviceHostName("dhcp-server-2"));
    TEST_ASSERT_TRUE(isValidDeviceHostName("a"));
    TEST_ASSERT_TRUE(isValidDeviceHostName("0"));
    TEST_ASSERT_TRUE(isValidDeviceHostName("123"));
    // RFC 1123 allows a label to start with a digit.
    TEST_ASSERT_TRUE(isValidDeviceHostName("3com"));
    return 0;
}

/** The 63-byte label limit, one character on each side of it. */
int test_the_length_limit()
{
    const string longest(kDeviceHostNameMaxLen, 'a');
    const string tooLong(kDeviceHostNameMaxLen + 1, 'a');
    TEST_ASSERT_TRUE(isValidDeviceHostName(longest));
    TEST_ASSERT_FALSE(isValidDeviceHostName(tooLong));
    return 0;
}

/** What a host name is not. */
int test_names_that_are_refused()
{
    TEST_ASSERT_FALSE(isValidDeviceHostName(""));
    TEST_ASSERT_FALSE(isValidDeviceHostName("-"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("-dhcp"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("dhcp-"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("dhcp server"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("dhcp_server"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("dhcp.server"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("dhcp/server"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("dhcp\n"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("хост"));
    TEST_ASSERT_FALSE(isValidDeviceHostName("dhcp!"));
    return 0;
}

/** Spaces around the value are the field's, not the name's. */
int test_trimming()
{
    TEST_ASSERT_STREQ(trimmedDeviceHostName("  DhcpServer  "), "DhcpServer");
    TEST_ASSERT_STREQ(trimmedDeviceHostName("\tDhcpServer\r\n"), "DhcpServer");
    TEST_ASSERT_STREQ(trimmedDeviceHostName("DhcpServer"), "DhcpServer");
    TEST_ASSERT_STREQ(trimmedDeviceHostName("   "), "");
    TEST_ASSERT_STREQ(trimmedDeviceHostName("dhcp server"), "dhcp server");
    return 0;
}

/** A stored value is used only while it still passes; otherwise the factory name. */
int test_the_stored_value_or_the_default()
{
    TEST_ASSERT_STREQ(deviceHostNameOrDefault("my-router"), "my-router");
    TEST_ASSERT_STREQ(deviceHostNameOrDefault(""), kDefaultDeviceHostName);
    TEST_ASSERT_STREQ(deviceHostNameOrDefault("bad name"), kDefaultDeviceHostName);
    TEST_ASSERT_STREQ(deviceHostNameOrDefault(string(kDeviceHostNameMaxLen + 1, 'a')),
                      kDefaultDeviceHostName);
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running DeviceHostName tests...\n");
    int failures = 0;

    failures += test_the_default_name();
    failures += test_names_that_are_allowed();
    failures += test_the_length_limit();
    failures += test_names_that_are_refused();
    failures += test_trimming();
    failures += test_the_stored_value_or_the_default();

    if (failures == 0) {
        printf("All DeviceHostName tests PASSED!\n");
    } else {
        printf("Some DeviceHostName tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
