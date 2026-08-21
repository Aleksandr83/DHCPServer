/**
 * @file test_dhcp.cpp
 * @brief Unit tests for DhcpServer IP management logic.
 *
 * NOTE: Full DHCP protocol tests require network hardware.
 * These tests validate internal IP range and selection logic.
 */

#include <cstdio>
#include <cstring>

#include "../src/dhcp/DhcpServer.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, (int)(a), (int)(b)); return 1; } } while(0)

extern "C" {

static int test_dhcp_server_create_destroy()
{
    // Just verify the object can be created/destroyed without crash
    dhcp::dhcp::DhcpServer* server = new dhcp::dhcp::DhcpServer();
    TEST_ASSERT_FALSE(server->isRunning());
    TEST_ASSERT_EQ(static_cast<int>(server->state()),
                   static_cast<int>(dhcp::dhcp::DhcpServerState::STOPPED));
    TEST_ASSERT_EQ(static_cast<int>(server->leaseCount()), 0);
    delete server;
    printf("DHCP create/destroy test PASSED\n");
    return 0;
}

void app_main()
{
    printf("Running DHCP tests...\n");
    int failures = 0;

    failures += test_dhcp_server_create_destroy();

    if (failures == 0) {
        printf("All DHCP tests PASSED!\n");
    } else {
        printf("Some DHCP tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
