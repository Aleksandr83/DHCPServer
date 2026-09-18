#include "FreeRtosErrorQueue.h"

#include <cstring>

#include "esp_heap_caps.h"

namespace dhcp {
namespace core {

namespace {

/** Level travels as a byte so the queue item stays a plain buffer. */
uint8_t levelToByte(LogLevel level)
{
    return level == LogLevel::Warn ? 1 : 0;
}

LogLevel byteToLevel(uint8_t value)
{
    return value == 1 ? LogLevel::Warn : LogLevel::Error;
}

/**
 * @brief Where the queue's storage goes.
 *
 * PSRAM when the chip has it: the queue is the only part of this log that is big
 * (16 lines × 240 bytes), and internal RAM is the scarce one on a device that
 * runs a DNS server, an HTTP server and several tasks. The *task stack* stays in
 * internal RAM on purpose — FreeRTOS requires that, and it is 4 KB.
 */
uint32_t queueMemoryCaps()
{
#if CONFIG_SPIRAM
    // heap_caps_get_total_size() is the honest question: it answers 0 when the
    // chip has no PSRAM at all (the legacy ESP32 build), and then the queue goes
    // to internal RAM instead of failing to be created.
    return (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) ? MALLOC_CAP_SPIRAM
                                                             : MALLOC_CAP_INTERNAL;
#else
    return MALLOC_CAP_INTERNAL;
#endif
}

} // namespace

FreeRtosErrorQueue::FreeRtosErrorQueue()
{
    queue_ = xQueueCreateWithCaps(kLength, sizeof(Item), queueMemoryCaps());
}

FreeRtosErrorQueue::~FreeRtosErrorQueue()
{
    if (queue_ != nullptr) {
        vQueueDeleteWithCaps(queue_);
        queue_ = nullptr;
    }
}

bool FreeRtosErrorQueue::push(const ErrorLogEntry& entry)
{
    if (queue_ == nullptr) return false;

    Item item;
    item.level = levelToByte(entry.level);
    // The text is already clamped by ErrorLogCore; this is the belt to that
    // braces, because a silent overflow here would corrupt the log itself.
    std::strncpy(item.text, entry.text.c_str(), sizeof(item.text) - 1);
    item.text[sizeof(item.text) - 1] = '\0';

    // Zero timeout: the producer never waits for the log.
    return xQueueSend(queue_, &item, 0) == pdTRUE;
}

bool FreeRtosErrorQueue::pop(ErrorLogEntry& out, uint32_t timeoutMs)
{
    if (queue_ == nullptr) return false;

    Item item;
    if (xQueueReceive(queue_, &item, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;

    out.level = byteToLevel(item.level);
    out.text = item.text;
    return true;
}

} // namespace core
} // namespace dhcp
