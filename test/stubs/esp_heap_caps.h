#ifndef DHCP_TEST_STUB_ESP_HEAP_CAPS_H
#define DHCP_TEST_STUB_ESP_HEAP_CAPS_H

/**
 * @file esp_heap_caps.h
 * @brief Minimal stand-in for the ESP-IDF capability allocator.
 *
 * Lets host tests compile code that pins memory to PSRAM (see
 * `test/test_internalcache.cpp`): here the arena becomes an ordinary heap
 * allocation and the capability flags lose their meaning — the host has only
 * one kind of memory. Pass `-I test/stubs` *before* `-I.` so this file wins
 * over the real one, which only exists inside the ESP-IDF tree.
 */

#include <cstddef>
#include <cstdlib>

#define MALLOC_CAP_SPIRAM   0x0001
#define MALLOC_CAP_INTERNAL 0x0002
#define MALLOC_CAP_8BIT     0x0004

inline void* heap_caps_malloc(size_t size, int /*caps*/)
{
    return std::malloc(size);
}

inline void heap_caps_free(void* ptr)
{
    std::free(ptr);
}

#endif // DHCP_TEST_STUB_ESP_HEAP_CAPS_H
