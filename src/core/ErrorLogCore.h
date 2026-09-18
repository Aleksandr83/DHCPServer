#ifndef DHCP_CORE_ERRORLOGCORE_H
#define DHCP_CORE_ERRORLOGCORE_H

#include <cstdint>
#include <functional>
#include <string>

#include "IErrorLogTarget.h"
#include "IErrorQueue.h"

namespace dhcp {
namespace core {

/**
 * @brief The part of the error log that knows nothing about FreeRTOS or FAT:
 *        formatting a line and moving queued messages into a target.
 *
 * The producers only ever call @ref submit, which never touches the filesystem
 * and never blocks — the writing happens in the consumer task (see ErrorLog,
 * which owns the task and the FreeRTOS queue). Everything in this class is plain
 * C++ on purpose, so the rules that make the log trustworthy are host-tested:
 *
 *   * a line always carries **when** it happened, and when the device clock is
 *     not set yet it says `t+152s` instead of pretending to be 1970;
 *   * a message longer than the queue's capacity is truncated **with a marker**,
 *     so nobody can mistake a cut message for a short one;
 *   * messages lost to a full queue are **counted and reported** in the log
 *     itself, because a log with silent holes is worse than no log.
 */
class ErrorLogCore {
public:
    /** Longest message that fits one queue item (the rest is truncated). */
    static constexpr size_t kMaxMessage = 200;

    /**
     * @param queue  hand-off to the writing task
     * @param target where the lines end up
     * @param uptimeSec seconds since boot, used when the clock is unset
     *                  (null on the host: the stamp is then `t+0s`)
     */
    ErrorLogCore(IErrorQueue& queue, IErrorLogTarget& target,
                 std::function<uint32_t()> uptimeSec = {});

    /**
     * @brief Producer side: format one line and enqueue it. Never blocks.
     * @return false when the entry did not fit the queue or was thrown away by a
     *         broken target — either way it is counted as a drop.
     */
    bool submit(LogLevel level, const char* tag, const std::string& message);

    /**
     * @brief Consumer side: write everything that is waiting.
     * @param timeoutMs how long to wait for the first entry (0 = do not wait)
     * @return number of lines handed to the target (including a drop notice).
     */
    uint32_t drain(uint32_t timeoutMs);

    /** @brief Messages lost so far (queue full, or the target refused them). */
    uint32_t dropped() const { return dropped_; }

    /**
     * @brief Format one line.
     *
     * @param nowEpochSec `time(nullptr)`; values before 2020 mean "the clock is
     *        not set" and the stamp falls back to the uptime.
     */
    static std::string formatLine(LogLevel level, const char* tag,
                                  const std::string& message,
                                  uint64_t nowEpochSec,
                                  uint32_t uptimeSec);

    /** @brief `[E]` / `[W]` — what a human sees at the start of the level column. */
    static const char* levelTag(LogLevel level);

    /** @brief True when the stamp can be a real date instead of an uptime. */
    static bool clockIsSet(uint64_t nowEpochSec);

    /** @brief Cut a message to one queue item, marking the cut. */
    static std::string clampMessage(const std::string& message);

private:
    IErrorQueue& queue_;
    IErrorLogTarget& target_;
    std::function<uint32_t()> uptimeSec_;
    uint32_t dropped_ = 0;
    /** When true, the next successful write starts by reporting the drops. */
    bool dropNoticePending_ = false;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_ERRORLOGCORE_H
