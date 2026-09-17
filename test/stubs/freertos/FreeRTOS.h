#ifndef DHCP_TEST_STUB_FREERTOS_H
#define DHCP_TEST_STUB_FREERTOS_H

/**
 * @file FreeRTOS.h
 * @brief Minimal stand-in for the FreeRTOS kernel header.
 *
 * There is no scheduler on the host and the tests are single-threaded, so the
 * mutex a class creates is a token pointer and taking/giving it does nothing
 * (see the pair in `freertos/semphr.h`). Only the types and constants used by
 * the code under test are provided.
 */

#include <cstdint>

typedef void* SemaphoreHandle_t;
typedef uint32_t TickType_t;

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)

#endif // DHCP_TEST_STUB_FREERTOS_H
