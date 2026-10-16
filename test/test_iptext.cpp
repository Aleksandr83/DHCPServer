/**
 * @file test_iptext.cpp
 * @brief Unit tests for the text of an IP address (stage 209).
 *
 * Two call sites print an address the operator reads â€” the peer of a connection
 * and every record of the cache view â€” and a second copy of RFC 5952 is how the
 * same IPv6 address ends up spelled two ways on two pages. The spelling therefore
 * lives in `core/IpText.h`, and this is where it is checked: no `inet_ntop` on the
 * host, and the board's one is not what is being tested anyway â€” the bytes are
 * handed in deliberately.
 *
 * The IPv6 cases are the near-misses rather than the pretty one: a single zero
 * group that must **not** become a gap, two equally long runs where the first one
 * wins, a run at either end of the address, and the shape `::` for an address of
 * nothing at all.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_iptext.cpp src/core/Subnet.cpp -o test_iptext
 */
#include "src/core/IpText.h"
#include "src/core/Subnet.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace std;

using dhcp::core::ip4HostOrder;
using dhcp::core::ip4Text;
using dhcp::core::ip6Text;
using dhcp::core::Subnet;

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

namespace {

string text4(initializer_list<int> values)
{
    const uint8_t bytes[4] = {static_cast<uint8_t>(values.begin()[0]),
                              static_cast<uint8_t>(values.begin()[1]),
                              static_cast<uint8_t>(values.begin()[2]),
                              static_cast<uint8_t>(values.begin()[3])};
    return ip4Text(bytes);
}

uint32_t order4(initializer_list<int> values)
{
    const uint8_t bytes[4] = {static_cast<uint8_t>(values.begin()[0]),
                              static_cast<uint8_t>(values.begin()[1]),
                              static_cast<uint8_t>(values.begin()[2]),
                              static_cast<uint8_t>(values.begin()[3])};
    return ip4HostOrder(bytes);
}

string text6(const vector<uint8_t>& bytes)
{
    return ip6Text(bytes.data());
}

/** Sixteen bytes from the eight groups of an address, in the order given. */
string text8(initializer_list<int> groups)
{
    vector<uint8_t> bytes(16, 0);
    int i = 0;
    for (const int group : groups) {
        if (i >= 8) break;
        bytes[2 * i] = static_cast<uint8_t>((group >> 8) & 0xFF);
        bytes[2 * i + 1] = static_cast<uint8_t>(group & 0xFF);
        ++i;
    }
    return text6(bytes);
}

} // namespace

static void test_the_byte_order_conversion()
{
    check(order4({192, 168, 1, 100}) == 0xC0A80164u,
          "the first byte is the most significant one â€” the order `Subnet` works in");
    check(order4({10, 0, 0, 7}) == 0x0A000007u, "and it holds for a second network");
    check(order4({255, 255, 255, 255}) == 0xFFFFFFFFu, "a full mask does not overflow");
    check(order4({0, 0, 0, 0}) == 0u, "nothing is nothing");
}

static void test_a_dotted_quad()
{
    check(text4({192, 168, 1, 100}) == "192.168.1.100", "four bytes are a dotted quad");
    check(text4({10, 0, 0, 7}) == "10.0.0.7", "a single-digit byte keeps its place");
    check(text4({255, 255, 255, 255}) == "255.255.255.255", "and 255 is not truncated");
    check(text4({0, 0, 0, 0}) == "0.0.0.0", "zeros are printed, not dropped");
}

static void test_one_address_one_spelling()
{
    // The board prints addresses of both kinds (host order for the filters, bytes
    // for a DNS answer), and two spellings of one address is how an operator ends
    // up comparing two columns that should have matched.
    for (const uint32_t host : {0x0A000007u, 0xC0A80164u, 0xFFFFFFFFu, 0x01000000u}) {
        const uint8_t bytes[4] = {static_cast<uint8_t>((host >> 24) & 0xFF),
                                  static_cast<uint8_t>((host >> 16) & 0xFF),
                                  static_cast<uint8_t>((host >> 8) & 0xFF),
                                  static_cast<uint8_t>(host & 0xFF)};
        check(ip4Text(bytes) == Subnet::toString(host),
              "the bytes and the host-order value are spelled the same way");
    }
}

static void test_ipv6_text()
{
    check(text6(vector<uint8_t>(16, 0)) == "::",
          "an address of nothing is `::`, not eight zeros");
    check(text6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) == "::1",
          "the loopback keeps its one group inside the compression");
    check(text6({0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) == "fe80::1",
          "a link-local address is compressed the same way");
    check(text8({0x2001, 0x0db8, 1, 2, 3, 4, 5, 6}) == "2001:db8:1:2:3:4:5:6",
          "an address with nothing to compress is written in full");
    check(text8({0x2001, 0x0db8, 1, 2, 3, 4, 5, 0}) == "2001:db8:1:2:3:4:5:0",
          "a single zero group at the end is a zero, not a `::`");
    check(text8({0x2001, 0x0db8, 0x10, 0, 0, 0, 0, 0x20}) == "2001:db8:10::20",
          "and it is still compressed when the run is long enough");
    check(text8({0x2001, 0x0db8, 0, 2, 3, 4, 5, 8}) == "2001:db8:0:2:3:4:5:8",
          "one zero group in the middle is a zero as well");
    check(text8({0x2001, 0x0db8, 1, 0, 0, 1, 0, 0}) == "2001:db8:1::1:0:0",
          "with two runs of the same length the first one is compressed");
}

static void test_lower_case_and_the_mapped_form()
{
    vector<uint8_t> bytes(16, 0);
    bytes[0] = 0x20;
    bytes[1] = 0x01;
    bytes[2] = 0x0D;
    bytes[3] = 0xB8;
    bytes[14] = 0x00;
    bytes[15] = 0xAB;
    check(text6(bytes) == "2001:db8::ab",
          "hex letters are lower case â€” a browser and the device agree on the text");

    const vector<uint8_t> mapped = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF,
                                    0xC0, 0xA8, 0x01, 0x64};
    check(text6(mapped) == "::ffff:c0a8:164",
          "an IPv4-mapped address is formatted as the IPv6 address it looks like â€” "
          "reading it as the IPv4 address inside is the caller's decision, not this one");
}

int main()
{
    printf("Running IpText tests...\n");
    test_the_byte_order_conversion();
    test_a_dotted_quad();
    test_one_address_one_spelling();
    test_ipv6_text();
    test_lower_case_and_the_mapped_form();

    if (g_failed == 0) {
        printf("All %d IpText checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d IpText checks FAILED!\n", g_failed, g_checks);
    return 1;
}
