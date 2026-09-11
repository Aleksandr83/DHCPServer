/**
 * @file test_subnet.cpp
 * @brief Unit tests for Subnet (IPv4 subnet arithmetic used by the LAN filters).
 *
 * Build with: pio test -e esp32dev
 *
 * `Subnet` has no ESP-IDF dependency, so the same tests also run on a host:
 *   g++ -std=c++17 -Dapp_main=esp_test_app_main -I. \
 *       test/test_subnet.cpp src/core/Subnet.cpp host_main.cpp -o test_subnet
 */

#include <cstdio>
#include <string>

// Include the code under test (adjust path as needed for test build)
#include "../src/core/Subnet.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, std::string(a).c_str(), std::string(b).c_str()); return 1; } } while(0)

using dhcp::core::Subnet;

// 192.168.1.0/24 — the project's default LAN
static const uint32_t kNet = 0xC0A80100u;
static const uint32_t kMask24 = 0xFFFFFF00u;

extern "C" {

static int test_parse_ip4()
{
    uint32_t ip = 0;
    TEST_ASSERT_TRUE(Subnet::parseIp4("192.168.1.201", ip));
    TEST_ASSERT_EQ(ip, 0xC0A801C9u);
    TEST_ASSERT_TRUE(Subnet::parseIp4("0.0.0.0", ip));
    TEST_ASSERT_EQ(ip, 0u);
    TEST_ASSERT_TRUE(Subnet::parseIp4("255.255.255.255", ip));
    TEST_ASSERT_EQ(ip, 0xFFFFFFFFu);
    TEST_ASSERT_TRUE(Subnet::parseIp4("10.0.0.1", ip));
    TEST_ASSERT_EQ(ip, 0x0A000001u);
    // surrounding whitespace is tolerated (free-form DHCP settings)
    TEST_ASSERT_TRUE(Subnet::parseIp4("  192.168.1.1  ", ip));
    TEST_ASSERT_EQ(ip, 0xC0A80101u);
    TEST_ASSERT_TRUE(Subnet::parseIp4("1.2.3.4", ip));
    TEST_ASSERT_EQ(ip, 0x01020304u);
    return 0;
}

static int test_parse_ip4_rejects()
{
    uint32_t ip = 0;
    TEST_ASSERT_FALSE(Subnet::parseIp4("", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("   ", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168.1", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168.1.2.3", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("256.1.1.1", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168.1.256", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168.1.", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168.1.:", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168.1.-1", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168.1.1a", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("a.b.c.d", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168. 1.1", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("192.168..1", ip));
    TEST_ASSERT_FALSE(Subnet::parseIp4("1234.1.1.1", ip));
    return 0;
}

static int test_masks()
{
    TEST_ASSERT_TRUE(Subnet::isValidMask(kMask24));
    TEST_ASSERT_TRUE(Subnet::isValidMask(0xFFFFFFFFu));
    TEST_ASSERT_TRUE(Subnet::isValidMask(0xFFFFFE00u));   // /23
    TEST_ASSERT_TRUE(Subnet::isValidMask(0xFF000000u));   // /8
    TEST_ASSERT_FALSE(Subnet::isValidMask(0xFFFFFF0Fu));  // hole
    TEST_ASSERT_FALSE(Subnet::isValidMask(0x00FFFFFFu));  // reversed
    // 0.0.0.0 is "no mask", not a /0 network (contains() refuses it)
    TEST_ASSERT_FALSE(Subnet::contains(kNet, 0u, 0xC0A801C9u));

    TEST_ASSERT_EQ(Subnet::maskFromPrefix(24), kMask24);
    TEST_ASSERT_EQ(Subnet::maskFromPrefix(32), 0xFFFFFFFFu);
    TEST_ASSERT_EQ(Subnet::maskFromPrefix(16), 0xFFFF0000u);
    TEST_ASSERT_EQ(Subnet::maskFromPrefix(0), 0u);
    TEST_ASSERT_EQ(Subnet::maskFromPrefix(33), 0u);
    TEST_ASSERT_EQ(Subnet::maskFromPrefix(-1), 0u);

    TEST_ASSERT_EQ(Subnet::prefixLength(kMask24), 24);
    TEST_ASSERT_EQ(Subnet::prefixLength(0xFFFFFFFFu), 32);
    TEST_ASSERT_EQ(Subnet::prefixLength(0xFFFFFE00u), 23);
    TEST_ASSERT_EQ(Subnet::prefixLength(0xFFFFFF0Fu), -1);
    TEST_ASSERT_EQ(Subnet::hostBits(kMask24), 8);
    TEST_ASSERT_EQ(Subnet::hostBits(0xFFFFFFFFu), 0);
    return 0;
}

static int test_contains()
{
    // 192.168.1.0/24
    TEST_ASSERT_TRUE(Subnet::contains(kNet, kMask24, 0xC0A80101u));    // .1
    TEST_ASSERT_TRUE(Subnet::contains(kNet, kMask24, 0xC0A801C9u));    // .201 (the device)
    TEST_ASSERT_TRUE(Subnet::contains(kNet, kMask24, 0xC0A801FFu));    // .255 broadcast
    TEST_ASSERT_FALSE(Subnet::contains(kNet, kMask24, 0xC0A80201u));   // 192.168.2.1
    TEST_ASSERT_FALSE(Subnet::contains(kNet, kMask24, 0x0A000001u));   // 10.0.0.1
    TEST_ASSERT_FALSE(Subnet::contains(kNet, kMask24, 0xC0A80001u));   // 192.168.0.1

    // /32 — only the exact address
    TEST_ASSERT_TRUE(Subnet::contains(0xC0A801C9u, 0xFFFFFFFFu, 0xC0A801C9u));
    TEST_ASSERT_FALSE(Subnet::contains(0xC0A801C9u, 0xFFFFFFFFu, 0xC0A801C8u));

    // /16
    TEST_ASSERT_TRUE(Subnet::contains(0xC0A80000u, 0xFFFF0000u, 0xC0A8FFFEu));
    TEST_ASSERT_FALSE(Subnet::contains(0xC0A80000u, 0xFFFF0000u, 0xC0A90001u));

    // a non-contiguous mask never matches
    TEST_ASSERT_FALSE(Subnet::contains(kNet, 0xFFFFFF0Fu, 0xC0A801C9u));
    return 0;
}

static int test_network_and_text()
{
    TEST_ASSERT_EQ(Subnet::network(0xC0A801C9u, kMask24), kNet);
    TEST_ASSERT_EQ(Subnet::network(0xC0A801C9u, 0xFFFFFFFFu), 0xC0A801C9u);
    TEST_ASSERT_EQ(Subnet::network(0x0A000001u, 0xFF000000u), 0x0A000000u);

    TEST_ASSERT_STR_EQ(Subnet::toString(0xC0A80101u), "192.168.1.1");
    TEST_ASSERT_STR_EQ(Subnet::toString(0u), "0.0.0.0");
    TEST_ASSERT_STR_EQ(Subnet::toString(0xFFFFFFFFu), "255.255.255.255");

    // round-trip through the text form
    uint32_t ip = 0;
    TEST_ASSERT_TRUE(Subnet::parseIp4(Subnet::toString(0xC0A801C9u), ip));
    TEST_ASSERT_EQ(ip, 0xC0A801C9u);
    return 0;
}

void app_main()
{
    printf("Running Subnet tests...\n");
    int failures = 0;

    failures += test_parse_ip4();
    failures += test_parse_ip4_rejects();
    failures += test_masks();
    failures += test_contains();
    failures += test_network_and_text();

    if (failures == 0) {
        printf("All Subnet tests PASSED!\n");
    } else {
        printf("Some Subnet tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
