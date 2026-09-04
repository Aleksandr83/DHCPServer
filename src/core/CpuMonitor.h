/**
 * @file CpuMonitor.h
 * @brief Per-core CPU load and memory usage monitor.
 *
 * Uses FreeRTOS idle/tick hooks (enabled via CONFIG_FREERTOS_USE_IDLE_HOOK
 * and CONFIG_FREERTOS_USE_TICK_HOOK). A dedicated task samples the counters
 * once per second and stores the results read by loadCore0(), loadCore1(),
 * the heap*() getters (internal RAM) and the psram*() getters (external
 * PSRAM, 0 when PSRAM is not enabled).
 */
#ifndef DHCP_CORE_CPUMONITOR_H
#define DHCP_CORE_CPUMONITOR_H

#include <cstdint>

namespace dhcp {
namespace core {

class CpuMonitor {
public:
    /** @brief Start the periodic sampling task. */
    static void start();

    /** @brief CPU load on core 0 in percent (0..100). */
    static int loadCore0();

    /** @brief CPU load on core 1 in percent (0..100). */
    static int loadCore1();

    /** @brief Free internal heap in bytes. */
    static uint32_t freeHeap();

    /** @brief Total 8-bit-capable internal heap size in bytes. */
    static uint32_t totalHeap();

    /** @brief Largest free contiguous 8-bit-capable internal block in bytes. */
    static uint32_t largestBlock();

    /**
     * @brief Physical on-chip internal SRAM size in bytes.
     *
     * On ESP32-P4 this is the full 768 KiB SRAM window (SOC_DRAM0
     * 0x4FF00000-0x4FFC0000), not just the subset the heap allocator
     * manages. The free portion still comes from freeHeap(), so the UI can
     * show true chip utilisation: used = ramTotal() - freeHeap(). On other
     * targets it falls back to totalHeap().
     */
    static uint32_t ramTotal();

    /** @brief Free external PSRAM in bytes (0 if PSRAM is not enabled). */
    static uint32_t psramFree();

    /** @brief Total external PSRAM size in bytes (0 if PSRAM is not enabled). */
    static uint32_t psramTotal();

    /** @brief Largest free contiguous PSRAM block in bytes. */
    static uint32_t psramLargest();

private:
    static void sampleTask(void* arg);

    static volatile int       s_load0;
    static volatile int       s_load1;
    static volatile uint32_t  s_freeHeap;
    static volatile uint32_t  s_totalHeap;
    static volatile uint32_t  s_largestBlock;
    static volatile uint32_t  s_ramTotal;
    static volatile uint32_t  s_psramFree;
    static volatile uint32_t  s_psramTotal;
    static volatile uint32_t  s_psramLargest;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_CPUMONITOR_H
