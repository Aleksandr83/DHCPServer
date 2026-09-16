#ifndef DHCP_TEST_STUB_ESP_LOG_H
#define DHCP_TEST_STUB_ESP_LOG_H

/**
 * @file esp_log.h
 * @brief Minimal stand-in for the ESP-IDF logging header.
 *
 * Lets host tests compile code that does nothing but log (see
 * `test/test_filesink.cpp`): pass `-I test/stubs` *before* `-I.` so this file
 * wins over the real one, which only exists inside the ESP-IDF tree.
 */

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#endif // DHCP_TEST_STUB_ESP_LOG_H
