/**
 * @file test_config.cpp
 * @brief Unit tests for Config class.
 *
 * NOTE: These tests require NVS to be initialized and run on target hardware.
 * For host testing, mock the NVS layer.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/Config.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)

extern "C" {

static int test_config_wifi()
{
    auto& cfg = dhcp::core::Config::instance();

    // Write
    dhcp::core::WifiConfig w;
    w.ssid = "TestWiFi";
    w.password = "secret123";
    cfg.setWifi(w);

    // Read back
    auto r = cfg.getWifi();
    TEST_ASSERT_STR_EQ(r.ssid.c_str(), "TestWiFi");
    TEST_ASSERT_STR_EQ(r.password.c_str(), "secret123");

    printf("WiFi config test PASSED\n");
    return 0;
}

static int test_config_dhcp()
{
    auto& cfg = dhcp::core::Config::instance();

    dhcp::core::DhcpConfig d;
    d.enabled = true;
    d.startIp = "10.0.0.1";
    d.endIp = "10.0.0.100";
    d.subnet = "255.255.255.0";
    d.gateway = "10.0.0.254";
    d.leaseTimeSec = 3600;
    cfg.setDhcp(d);

    auto r = cfg.getDhcp();
    TEST_ASSERT_TRUE(r.enabled);
    TEST_ASSERT_STR_EQ(r.startIp.c_str(), "10.0.0.1");
    TEST_ASSERT_STR_EQ(r.endIp.c_str(), "10.0.0.100");
    TEST_ASSERT_STR_EQ(r.subnet.c_str(), "255.255.255.0");
    TEST_ASSERT_STR_EQ(r.gateway.c_str(), "10.0.0.254");
    TEST_ASSERT_EQ(static_cast<int>(r.leaseTimeSec), 3600);

    printf("DHCP config test PASSED\n");
    return 0;
}

static int test_config_static_bindings()
{
    auto& cfg = dhcp::core::Config::instance();

    std::vector<dhcp::core::StaticBinding> bindings;
    bindings.push_back({"24:0A:C4:01:23:45", "192.168.1.50", "Printer"});
    bindings.push_back({"AA:BB:CC:DD:EE:FF", "192.168.1.60", "Camera"});

    TEST_ASSERT_TRUE(cfg.setStaticBindings(bindings));

    auto r = cfg.getStaticBindings();
    TEST_ASSERT_EQ(r.size(), static_cast<size_t>(2));
    TEST_ASSERT_STR_EQ(r[0].mac.c_str(), "24:0A:C4:01:23:45");
    TEST_ASSERT_STR_EQ(r[0].ip.c_str(), "192.168.1.50");
    TEST_ASSERT_STR_EQ(r[0].name.c_str(), "Printer");
    TEST_ASSERT_STR_EQ(r[1].mac.c_str(), "AA:BB:CC:DD:EE:FF");

    printf("Static bindings test PASSED\n");
    return 0;
}

static int test_config_security()
{
    auto& cfg = dhcp::core::Config::instance();

    dhcp::core::SecurityConfig s;
    s.username = "root";
    s.password = "superpass";
    s.maxAttempts = 3;
    s.lockoutPeriodSec = 600;
    cfg.setSecurity(s);

    auto r = cfg.getSecurity();
    TEST_ASSERT_STR_EQ(r.username.c_str(), "root");
    TEST_ASSERT_STR_EQ(r.password.c_str(), "superpass");
    TEST_ASSERT_EQ(static_cast<int>(r.maxAttempts), 3);
    TEST_ASSERT_EQ(static_cast<int>(r.lockoutPeriodSec), 600);

    printf("Security config test PASSED\n");
    return 0;
}

void app_main()
{
    printf("Running Config tests...\n");
    int failures = 0;

    failures += test_config_wifi();
    failures += test_config_dhcp();
    failures += test_config_static_bindings();
    failures += test_config_security();

    if (failures == 0) {
        printf("All Config tests PASSED!\n");
    } else {
        printf("Some Config tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
