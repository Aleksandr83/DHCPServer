#ifndef DHCP_TEST_STUB_FREERTOS_SEMPHR_H
#define DHCP_TEST_STUB_FREERTOS_SEMPHR_H

/**
 * @file semphr.h
 * @brief Minimal stand-in for the FreeRTOS semaphore API.
 *
 * The host tests run on one thread, so mutual exclusion cannot be observed
 * here — the calls only have to exist and to hand back something non-null so
 * that `if (mutex_)` in the code under test stays true.
 */

#include "FreeRTOS.h"

inline SemaphoreHandle_t xSemaphoreCreateMutex()
{
    static int token = 1;
    return &token;
}

inline void vSemaphoreDelete(SemaphoreHandle_t /*sem*/)
{
}

inline int xSemaphoreTake(SemaphoreHandle_t /*sem*/, TickType_t /*ticks*/)
{
    return 1;   // pdTRUE
}

inline int xSemaphoreGive(SemaphoreHandle_t /*sem*/)
{
    return 1;   // pdTRUE
}

#endif // DHCP_TEST_STUB_FREERTOS_SEMPHR_H
