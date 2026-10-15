#ifndef DHCP_CORE_LOGFILETASKLIMITS_H
#define DHCP_CORE_LOGFILETASKLIMITS_H

#include <cstdint>

namespace dhcp {
namespace core {

/**
 * @brief What the task that drains a file-log queue is given (stages 191, 204).
 *
 * Every file log in this project writes the same way: the producer (an NTP
 * request, a DNS answer) puts a fixed-size record into a bounded queue and never
 * touches the filesystem, and one low-priority task drains that queue and writes
 * the lines. The numbers below are that arrangement, and they are here - once -
 * because two logs (the NTP request log and the DNS forwarded log) are the same
 * arrangement: a queue deep enough to ride out a burst of a few hundred
 * milliseconds, a task with no TLS and no HTTP on its stack, and a failure report
 * that repeats often enough to be noticed and rarely enough not to bury
 * everything else.
 *
 * Plain integers on purpose: this header is included by classes that already use
 * FreeRTOS, and it must not drag anything in by itself.
 */

/** @brief Records a file log's queue holds before the oldest is dropped. */
constexpr int kFileLogQueueDepth = 16;

/** @brief Stack of the writing task: a FAT write and a string, no TLS here. */
constexpr int kFileLogTaskStack = 4096;

/** @brief Priority of the writing task: below the servers it serves. */
constexpr int kFileLogTaskPriority = 3;

/** @brief How often a write that keeps failing is repeated on the terminal. */
constexpr uint32_t kFileLogWriteWarnEvery = 100;

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_LOGFILETASKLIMITS_H
