#ifndef DHCP_TEST_STUB_ESP_TIMER_H
#define DHCP_TEST_STUB_ESP_TIMER_H

/**
 * @file esp_timer.h
 * @brief Minimal stand-in for the ESP-IDF microsecond timer.
 *
 * The clock is a plain counter the test can move (`testClockUs()`), which is
 * what makes TTL expiry testable on a host without sleeping: a test ages a
 * record by seconds in one statement. See `test/test_internalcache.cpp`.
 */

#include <cstdint>

inline int64_t& testClockUs()
{
    static int64_t us = 0;
    return us;
}

inline int64_t esp_timer_get_time()
{
    return testClockUs();
}

#endif // DHCP_TEST_STUB_ESP_TIMER_H
