/**
 * @file test_subnetfilter.cpp
 * @brief Unit tests for SubnetFilter — the LAN-only filter shared by DNS, NTP and
 * the file manager (stage 200).
 *
 * The same five decisions lived in three copies (DnsServer::applySubnetFilter,
 * TimeServer::applyAccessFilter, FileManager::applyAccessFilter), drifted apart
 * in wording, and could not be tested at all because they sat inside ESP-IDF
 * classes. This file pins the decisions themselves, and the most important one
 * is a defect the copies shared: `Subnet::isValidMask(0)` answers true, while
 * `Subnet::contains()` always answers false for a zero mask — so a subnet of
 * `0.0.0.0` used to switch the filter ON and drop every client, on a device whose
 * operator asked for the opposite of an open resolver but certainly not for a
 * silent LAN blackout. "The subnet cannot be determined" has to mean fail-open,
 * which is what the class now guarantees for a zero mask, a zero address, a
 * non-contiguous mask and unparsable text alike.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_subnetfilter.cpp src/core/SubnetFilter.cpp src/core/Subnet.cpp \
 *       -o test_subnetfilter
 */
#include "src/core/SubnetFilter.h"

#include <cstdio>
#include <string>

using namespace std;
using dhcp::core::SubnetFilter;

/// The project's default LAN: 192.168.1.0/24, device at 192.168.1.201.
static const char* const kIp = "192.168.1.201";
static const char* const kMask24 = "255.255.255.0";

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

/** Host-order address of a dotted quad, without depending on the class under test. */
static uint32_t ip(const char* text)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d);
    return (a << 24) | (b << 16) | (c << 8) | d;
}

static SubnetFilter::Settings settings(const char* deviceIp, const char* netmask,
                                       bool enabled = true)
{
    SubnetFilter::Settings s;
    s.enabled = enabled;
    s.deviceIp = deviceIp;
    s.netmask = netmask;
    return s;
}

static void test_inside_is_allowed_outside_is_not()
{
    SubnetFilter f;
    f.apply(settings(kIp, kMask24));

    check(f.active(), "a device address and a /24 mask switch the filter on");
    check(f.network() == ip("192.168.1.0"), "the network is the masked address");
    check(f.mask() == ip("255.255.255.0"), "the mask is kept as it was read");

    check(f.contains(ip("192.168.1.1")), "a client inside the subnet is allowed");
    check(f.contains(ip("192.168.1.201")), "the device itself is inside its own subnet");
    check(f.contains(ip("192.168.1.255")), "the broadcast address of the subnet is inside it");
    check(!f.contains(ip("192.168.2.1")), "the next subnet over is not");
    check(!f.contains(ip("10.0.0.1")), "an unrelated private range is not");
    check(!f.contains(ip("8.8.8.8")), "a public address is not");
}

static void test_switch_off_allows_everything()
{
    SubnetFilter f;
    f.apply(settings(kIp, kMask24, false));

    check(!f.active(), "the switch being off means the filter does not apply");
    check(f.contains(ip("8.8.8.8")), "and then every address is allowed");
}

static void test_zero_mask_does_not_block_the_lan()
{
    // The defect the three copies shared: isValidMask(0) is true, contains() with
    // a zero mask is always false — so this used to drop every client.
    SubnetFilter f;
    f.apply(settings(kIp, "0.0.0.0"));

    check(!f.active(), "a zero mask cannot be used to decide, so the filter stays off");
    check(f.contains(ip("192.168.1.5")), "a client of the LAN is not blocked by it");
    check(f.contains(ip("8.8.8.8")), "and neither is anybody else (fail-open)");
    check(f.skippedText().find("0.0.0.0") != string::npos,
          "the warning quotes the value that could not be used");
}

static void test_zero_address_does_not_block_the_lan()
{
    SubnetFilter f;
    f.apply(settings("0.0.0.0", kMask24));

    check(!f.active(), "an address of 0.0.0.0 is not an address this device has");
    check(f.contains(ip("192.168.1.5")), "so the filter fails open");
    check(f.skippedText().find("0.0.0.0") != string::npos, "and the warning says which value");
}

static void test_unparsable_values_fail_open()
{
    SubnetFilter f;
    f.apply(settings("", kMask24));
    check(!f.active(), "an empty device address cannot be used");
    check(f.contains(ip("192.168.1.5")), "and the LAN keeps its time and files");
    check(f.skippedText().find("server_ip") != string::npos,
          "the warning names the setting that has to be fixed");

    f.apply(settings(kIp, ""));
    check(!f.active(), "an empty netmask cannot be used either");
    check(f.contains(ip("192.168.1.5")), "still fail-open");

    f.apply(settings("275.1.1.1", kMask24));
    check(!f.active(), "an address that is not an IPv4 address is refused");

    f.apply(settings(kIp, "192.168.1"));
    check(!f.active(), "an incomplete netmask is refused");
    check(f.contains(ip("192.168.1.5")), "and every one of those cases fails open");
}

static void test_non_contiguous_mask_fails_open()
{
    SubnetFilter f;
    f.apply(settings(kIp, "255.0.255.0"));

    check(!f.active(), "a mask whose bits are not contiguous cannot define a subnet");
    check(f.contains(ip("192.168.1.5")), "so the filter fails open rather than guessing");
    check(f.contains(ip("8.8.8.8")), "for every address");
}

static void test_settings_are_read_as_free_form_text()
{
    SubnetFilter f;
    f.apply(settings("  192.168.1.201  ", " 255.255.255.0 "));

    check(f.active(), "spaces around the settings values are tolerated");
    check(f.contains(ip("192.168.1.5")), "and the subnet works as written");
}

static void test_a_single_host_subnet()
{
    SubnetFilter f;
    f.apply(settings(kIp, "255.255.255.255"));

    check(f.active(), "/32 is a subnet the operator may choose deliberately");
    check(f.contains(ip("192.168.1.201")), "it holds the device address itself");
    check(!f.contains(ip("192.168.1.202")), "and nothing else");
}

static void test_reapply_recomputes_everything()
{
    SubnetFilter f;
    f.apply(settings(kIp, kMask24));
    check(f.active(), "one usable subnet is applied");

    f.apply(settings(kIp, "255.0.255.0"));
    check(!f.active(), "a later unusable mask turns the filter off again");
    check(f.mask() == 0, "and leaves no stale mask behind");
    check(f.contains(ip("192.168.1.5")), "still fail-open");

    f.apply(settings("10.1.2.3", "255.255.0.0"));
    check(f.active(), "and a later usable subnet turns it back on");
    check(f.network() == ip("10.1.0.0"), "with its own network");
    check(f.contains(ip("10.1.9.9")), "allowing the new LAN");
    check(!f.contains(ip("192.168.1.5")), "and refusing the old one");
}

static void test_the_log_line_is_written_once_here()
{
    SubnetFilter f;
    f.apply(settings(kIp, kMask24));
    const string enabled = f.enabledText();
    check(enabled.find("192.168.1.0/24") != string::npos,
          "the enabled line names the network and its prefix length: " + enabled);
    check(enabled.find("192.168.1.0") == enabled.rfind("192.168.1.0"),
          "and names that network once, not twice");

    f.apply(settings(kIp, "0.0.0.0"));
    const string skipped = f.skippedText();
    check(skipped.find("SKIPPED") != string::npos,
          "the fail-open line says the filter is skipped: " + skipped);
    check(skipped.find(kIp) != string::npos && skipped.find("0.0.0.0") != string::npos,
          "and quotes both settings values");
}

int main()
{
    printf("Running SubnetFilter tests...\n");
    test_inside_is_allowed_outside_is_not();
    test_switch_off_allows_everything();
    test_zero_mask_does_not_block_the_lan();
    test_zero_address_does_not_block_the_lan();
    test_unparsable_values_fail_open();
    test_non_contiguous_mask_fails_open();
    test_settings_are_read_as_free_form_text();
    test_a_single_host_subnet();
    test_reapply_recomputes_everything();
    test_the_log_line_is_written_once_here();

    if (g_failed == 0) {
        printf("All %d SubnetFilter checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d SubnetFilter checks FAILED!\n", g_failed, g_checks);
    return 1;
}
