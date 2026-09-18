#ifndef DHCP_CORE_FREERTOSERRORQUEUE_H
#define DHCP_CORE_FREERTOSERRORQUEUE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "IErrorQueue.h"

namespace dhcp {
namespace core {

/**
 * @brief The error log's queue on the device: a fixed-size FreeRTOS queue.
 *
 * Fixed size on purpose — `push()` must never allocate (that is what would make
 * a caller in the DNS task block) and must never fail for a reason the caller
 * cannot handle. A message that does not fit the item is cut before it gets here
 * (ErrorLogCore::clampMessage), so a `push` can only lose a message when the
 * queue is genuinely full, and the core counts that.
 */
class FreeRtosErrorQueue : public IErrorQueue {
public:
    /** Room for one formatted line (stamp + level + tag + 200 chars + slack). */
    static constexpr size_t kItemBytes = 240;
    /** How many lines may wait before a producer starts losing them. */
    static constexpr size_t kLength = 16;

    FreeRtosErrorQueue();
    ~FreeRtosErrorQueue() override;

    FreeRtosErrorQueue(const FreeRtosErrorQueue&) = delete;
    FreeRtosErrorQueue& operator=(const FreeRtosErrorQueue&) = delete;

    bool push(const ErrorLogEntry& entry) override;
    bool pop(ErrorLogEntry& out, uint32_t timeoutMs) override;

    /** @brief True when the queue was created (the log may be started). */
    bool ready() const { return queue_ != nullptr; }

private:
    /** What actually travels through the FreeRTOS queue (no std::string in it). */
    struct Item {
        uint8_t level = 0;
        char    text[kItemBytes] = {};
    };

    QueueHandle_t queue_ = nullptr;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_FREERTOSERRORQUEUE_H
