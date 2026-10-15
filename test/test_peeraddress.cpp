/**
 * @file test_peeraddress.cpp
 * @brief Unit tests for the peer address of an HTTP request: what its bytes mean
 *        (stage 207).
 *
 * The board showed `auth: failed login from 0.0.0.0` in `Auth.log` while the
 * operator was on the LAN: with `CONFIG_LWIP_IPV6` the HTTP server listens on a
 * **dual-stack** socket, so an IPv4 client is reported as an IPv4-mapped IPv6
 * address and the code that only accepted `AF_INET` read nothing at all. Two
 * things hang on that address: the LAN-only filter of the file endpoints (needs
 * it as a number) and the failed-attempt counter of the login (keys on it as
 * text), so "nothing at all" meant "every client is the same client".
 *
 * The decision is therefore built here from plain bytes, without a socket and
 * without ESP-IDF: the family dispatch stays in `RestApi.cpp`, and what a family
 * means is checked on the host, where the bytes are handed in deliberately.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_peeraddress.cpp src/core/Subnet.cpp -o test_peeraddress
 */
#include "src/web/PeerAddress.h"
#include "src/core/Subnet.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace std;

using dhcp::core::Subnet;
using dhcp::web::PeerAddress;
using dhcp::web::peerAddressFromBytes;

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

PeerAddress from(initializer_list<int> values)
{
    vector<uint8_t> bytes;
    bytes.reserve(values.size());
    for (const int value : values) bytes.push_back(static_cast<uint8_t>(value));
    return peerAddressFromBytes(bytes.data(), bytes.size());
}

/** `::ffff:a.b.c.d` — what the board's dual-stack socket reports for an IPv4 peer. */
PeerAddress mapped(int a, int b, int c, int d)
{
    return from({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, a, b, c, d});
}

const uint8_t kExampleCom[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x01, 0x00, 0x02,
                                 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06};

} // namespace

static void test_an_ipv4_peer()
{
    const PeerAddress peer = from({192, 168, 1, 100});

    check(peer.ipv4, "four bytes are an IPv4 address the filters can judge");
    check(peer.ipv4HostOrder == 0xC0A80164u,
          "in host byte order, the form every filter and every log line of the device uses");
    check(peer.text == "192.168.1.100", "and named as a dotted quad");
    check(peer.text == Subnet::toString(peer.ipv4HostOrder),
          "the text is the same one `Subnet` prints, so one address has one spelling");
}

static void test_the_v4_mapped_peer_of_a_dual_stack_socket()
{
    const PeerAddress peer = mapped(192, 168, 1, 100);

    check(peer.ipv4,
          "an IPv4-mapped IPv6 peer is an IPv4 peer: this is the case the board hit");
    check(peer.ipv4HostOrder == 0xC0A80164u,
          "its four bytes are read as the address the client really has");
    check(peer.text == "192.168.1.100",
          "so `Auth.log` names the browser instead of the 0.0.0.0 that sent us here");
    check(mapped(10, 0, 0, 7).text == "10.0.0.7",
          "the mapping is not a special case for one network");
}

static void test_a_real_ipv6_peer()
{
    check(!from({0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}).ipv4,
          "an address of the second family is not an IPv4 address");
    check(from({0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}).text == "fe80::1",
          "it is named as an IPv6 address, with the run of zeros compressed");
    check(from({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}).text == "::1",
          "the loopback keeps its one leading group inside the compression");
    check(from({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}).text == "::",
          "an address of nothing at all is still not a dotted quad");
    check(peerAddressFromBytes(kExampleCom, sizeof(kExampleCom)).text ==
              "2001:db8:1:2:3:4:5:6",
          "leading zeros are dropped and the letters are lower case");
    check(from({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,
                0x00, 0x04, 0x00, 0x05})
              .text == "2001:db8:0:1:2:3:4:5",
          "a single zero group is written as a zero, not compressed into a false gap");
    check(from({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0x00, 0x01, 0, 0, 0, 0, 0x00, 0x01})
              .text == "2001:db8::1:0:0:1",
          "with two runs of the same length the first one is the one compressed");
}

static void test_what_is_not_a_mapping()
{
    // The two 0xFF bytes alone do not make a mapping: the ten bytes in front of
    // them are what says "this is an IPv4 address in an IPv6 coat".
    const PeerAddress peer = from({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF,
                                   0xC0, 0xA8, 0x01, 0x64});

    check(!peer.ipv4, "an address that only ends in ffff:c0a8:164 is an IPv6 address");
    check(peer.text == "2001:db8::ffff:c0a8:164", "and is named as one");
    check(!from({0, 0, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0, 192, 168, 1, 100}).ipv4,
          "nor does a prefix that is zero everywhere except one byte");
}

static void test_nothing_to_read()
{
    for (const size_t length : {size_t{0}, size_t{3}, size_t{6}, size_t{20}}) {
        vector<uint8_t> bytes(length, 0xAB);
        const PeerAddress peer = peerAddressFromBytes(bytes.data(), bytes.size());
        check(!peer.ipv4, "a peer address of " + to_string(length) +
                              " bytes is not an IPv4 address this device can judge");
        check(peer.text == "unknown",
              "and it is named `unknown`, never a plausible address a log reader "
              "would take for the client (that is what 0.0.0.0 did)");
    }

    const PeerAddress zero = from({0, 0, 0, 0});
    check(!zero.ipv4, "0.0.0.0 is not a peer, whoever sent it");
    check(zero.text == "unknown",
          "so it is named `unknown` too, instead of an address that exists nowhere");
    check(!mapped(0, 0, 0, 0).ipv4, "the mapped form of it is the same nothing");
    check(mapped(0, 0, 0, 0).text == "unknown", "and is named the same way");
    check(peerAddressFromBytes(nullptr, 0).text == "unknown",
          "a null pointer with no length is not a reason to read or to crash");
}

int main()
{
    printf("Running PeerAddress tests...\n");
    test_an_ipv4_peer();
    test_the_v4_mapped_peer_of_a_dual_stack_socket();
    test_a_real_ipv6_peer();
    test_what_is_not_a_mapping();
    test_nothing_to_read();

    if (g_failed == 0) {
        printf("All %d PeerAddress checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d PeerAddress checks FAILED!\n", g_failed, g_checks);
    return 1;
}
