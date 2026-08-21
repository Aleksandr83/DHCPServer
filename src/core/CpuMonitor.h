/**
 * @file CpuMonitor.h
 * @brief Per-core CPU load and heap usage monitor.
 *
 * Uses FreeRTOS idle/tick hooks (enabled via CONFIG_FREERTOS_USE_IDLE_HOOK
 * and CONFIG_FREERTOS_USE_TICK_HOOK). A dedicated task samples the counters
 * once per second and stores the results read by loadCore0(), loadCore1(),
 * freeHeap() and largestBlock().
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

    /** @brief Total 8-bit-capable heap size in bytes. */
    static uint32_t totalHeap();

    /** @brief Largest free contiguous 8-bit-capable block in bytes. */
    static uint32_t largestBlock();

private:
    static void sampleTask(void* arg);

    static volatile int       s_load0;
    static volatile int       s_load1;
    static volatile uint32_t  s_freeHeap;
    static volatile uint32_t  s_totalHeap;
    static volatile uint32_t  s_largestBlock;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_CPUMONITOR_H
