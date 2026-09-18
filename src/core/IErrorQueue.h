#ifndef DHCP_CORE_IERRORQUEUE_H
#define DHCP_CORE_IERRORQUEUE_H

#include <cstdint>
#include <string>

#include "IErrorLogTarget.h"

namespace dhcp {
namespace core {

/**
 * @brief One message on its way from the task that found the error to the task
 *        that writes it.
 *
 * The text is already formatted (stamp, level, tag) by the producer: the stamp
 * has to be the moment the error happened, not the moment the log got round to
 * writing it, and a queue of plain text keeps the consumer trivial.
 */
struct ErrorLogEntry {
    LogLevel    level = LogLevel::Error;
    std::string text;
};

/**
 * @brief The hand-off between whoever found an error and the task that writes it.
 *
 * The producers are the tasks that must never be held up — the DNS task, the
 * single httpd task, the file-transfer task — and the consumer writes to FAT,
 * which can take as long as the card wants. So the two are separated by a queue
 * and `push()` **never blocks**: it reports false when the queue is full, and the
 * caller counts the drop. A log that can stall the device it is logging about
 * would be worse than no log.
 */
class IErrorQueue {
public:
    virtual ~IErrorQueue() = default;

    /** @brief Enqueue an entry. Never blocks; false = the queue is full. */
    virtual bool push(const ErrorLogEntry& entry) = 0;

    /**
     * @brief Take one entry, waiting at most @p timeoutMs for it.
     * @return false when nothing arrived in time.
     */
    virtual bool pop(ErrorLogEntry& out, uint32_t timeoutMs) = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_IERRORQUEUE_H
