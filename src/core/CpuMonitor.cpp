/**
 * @file CpuMonitor.cpp
 * @brief Per-core CPU load and heap usage monitor.
 *
 * CPU load is measured using FreeRTOS idle/tick hooks. The tick hook runs on
 * the core that processes the tick (core 0 in ESP-IDF by default) and counts
 * total ticks; the idle hook runs on each core when that core's idle task is
 * scheduled, counting how many ticks a core spent idle. Load for a core over
 * a window is 100 - idle_percent. Requires:
 *   CONFIG_FREERTOS_USE_IDLE_HOOK=y
 *   CONFIG_FREERTOS_USE_TICK_HOOK=y
 */
#include "core/CpuMonitor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char* TAG = "CpuMonitor";

// ─── FreeRTOS hooks (must be global, extern "C") ─────
//
// IMPORTANT: These hooks run from the tick ISR / idle task. A flash write
// (NVS save, SPIFFS, DNS cache, etc.) temporarily DISABLES the flash cache,
// so any code executed during that window must live in IRAM. If these hooks
// were in regular flash, we'd get "Cache disabled but cached memory region
// accessed" -> Guru Meditation -> reboot.
namespace {
volatile uint32_t s_idle0 = 0;  // idle hook calls on core 0
volatile uint32_t s_idle1 = 0;  // idle hook calls on core 1
volatile uint32_t s_ticks  = 0; // tick hook calls (core 0)
}

extern "C" {
void IRAM_ATTR vApplicationIdleHook(void)
{
    // Note: cannot use ++ on volatile (deprecated in C++20, -Werror=volatile)
    if (xPortGetCoreID() == 0) {
        s_idle0 = s_idle0 + 1;
    } else {
        s_idle1 = s_idle1 + 1;
    }
}

void IRAM_ATTR vApplicationTickHook(void)
{
    s_ticks = s_ticks + 1;
}
}

namespace dhcp {
namespace core {

volatile int      CpuMonitor::s_load0      = 0;
volatile int      CpuMonitor::s_load1      = 0;
volatile uint32_t CpuMonitor::s_freeHeap   = 0;
volatile uint32_t CpuMonitor::s_totalHeap  = 0;
volatile uint32_t CpuMonitor::s_largestBlock = 0;

void CpuMonitor::start()
{
    // FreeRTOS hooks are compiled in; just start the sampling task.
    xTaskCreatePinnedToCore(sampleTask, "cpu_mon", 4096, nullptr, 5,
                            nullptr, tskNO_AFFINITY);
    ESP_LOGI(TAG, "CpuMonitor started");
}

void CpuMonitor::sampleTask(void* arg)
{
    uint32_t lastTicks = 0, lastIdle0 = 0, lastIdle1 = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t ticks = s_ticks;
        uint32_t idle0 = s_idle0;
        uint32_t idle1 = s_idle1;

        uint32_t dTicks = ticks - lastTicks;
        if (dTicks > 0) {
            int load0 = 100 - (int)((idle0 - lastIdle0) * 100 / dTicks);
            int load1 = 100 - (int)((idle1 - lastIdle1) * 100 / dTicks);
            if (load0 < 0) load0 = 0;
            if (load0 > 100) load0 = 100;
            if (load1 < 0) load1 = 0;
            if (load1 > 100) load1 = 100;
            s_load0 = load0;
            s_load1 = load1;
        }

        lastTicks = ticks;
        lastIdle0 = idle0;
        lastIdle1 = idle1;

        s_freeHeap = esp_get_free_heap_size();
        s_totalHeap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        s_largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }
}

int CpuMonitor::loadCore0() { return s_load0; }
int CpuMonitor::loadCore1() { return s_load1; }
uint32_t CpuMonitor::freeHeap() { return s_freeHeap; }
uint32_t CpuMonitor::totalHeap() { return s_totalHeap; }
uint32_t CpuMonitor::largestBlock() { return s_largestBlock; }

} // namespace core
} // namespace dhcp
