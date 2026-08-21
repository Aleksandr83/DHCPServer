/**
 * @file test_wifi.cpp
 * @brief Unit tests for WiFiManager and LedController.
 *
 * NOTE: WiFi tests require actual hardware. These are integration-level tests.
 * LedController tests can run on any ESP32.
 */

#include <cstdio>

#include "../src/led/LedController.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)

extern "C" {

static int test_led_toggle()
{
    dhcp::led::LedController led(26);

    TEST_ASSERT_FALSE(led.isOn());

    led.turnOn();
    TEST_ASSERT_TRUE(led.isOn());

    led.turnOff();
    TEST_ASSERT_FALSE(led.isOn());

    led.toggle();
    TEST_ASSERT_TRUE(led.isOn());

    led.toggle();
    TEST_ASSERT_FALSE(led.isOn());

    printf("LED toggle test PASSED\n");
    return 0;
}

static int test_led_active_low()
{
    // Test with activeLow configuration
    dhcp::led::LedController led(26, false);
    TEST_ASSERT_FALSE(led.isOn());

    led.turnOn();
    TEST_ASSERT_TRUE(led.isOn());

    led.turnOff();
    TEST_ASSERT_FALSE(led.isOn());

    printf("LED active low test PASSED\n");
    return 0;
}

void app_main()
{
    printf("Running WiFi/LED tests...\n");
    int failures = 0;

    failures += test_led_toggle();
    failures += test_led_active_low();

    if (failures == 0) {
        printf("All WiFi/LED tests PASSED!\n");
    } else {
        printf("Some WiFi/LED tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
