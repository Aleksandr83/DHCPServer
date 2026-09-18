#include "ErrorLogCore.h"

#include <ctime>

namespace dhcp {
namespace core {

namespace {

/** 2020-01-01T00:00:00Z — anything before it means "the clock was never set". */
constexpr uint64_t kSaneClockSec = 1577836800ULL;

/** What a truncated message ends with, so a cut is never mistaken for an end. */
constexpr const char* kTruncatedMark = " ...(truncated)";

/**
 * @brief Local time, thread-safe — and spelled differently on the two platforms.
 *
 * The device (newlib) has `localtime_r`; MinGW has `localtime_s` with its own
 * argument order. Keeping the difference here means the rest of the file — the
 * part the host test covers — is one implementation, not two.
 */
bool localTime(std::time_t t, std::tm& out)
{
#if defined(_WIN32)
    return ::localtime_s(&out, &t) == 0;
#else
    return ::localtime_r(&t, &out) != nullptr;
#endif
}

} // namespace

ErrorLogCore::ErrorLogCore(IErrorQueue& queue, IErrorLogTarget& target,
                           std::function<uint32_t()> uptimeSec)
    : queue_(queue)
    , target_(target)
    , uptimeSec_(std::move(uptimeSec))
{
}

bool ErrorLogCore::clockIsSet(uint64_t nowEpochSec)
{
    return nowEpochSec >= kSaneClockSec;
}

const char* ErrorLogCore::levelTag(LogLevel level)
{
    return level == LogLevel::Warn ? "[W]" : "[E]";
}

std::string ErrorLogCore::clampMessage(const std::string& message)
{
    if (message.size() <= kMaxMessage) return message;
    return message.substr(0, kMaxMessage) + kTruncatedMark;
}

std::string ErrorLogCore::formatLine(LogLevel level, const char* tag,
                                     const std::string& message,
                                     uint64_t nowEpochSec, uint32_t uptimeSec)
{
    std::string stamp;
    if (clockIsSet(nowEpochSec)) {
        const std::time_t t = static_cast<std::time_t>(nowEpochSec);
        std::tm tmv{};
        // Local time rather than UTC: the operator reads this next to the
        // device's own pages, which show the same clock.
        if (localTime(t, tmv)) {
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
            stamp = buf;
        }
    }
    if (stamp.empty()) {
        // No clock yet (a device that has not synced): an uptime stamp is a true
        // statement, "1970-01-01" is not.
        char buf[24];
        std::snprintf(buf, sizeof(buf), "t+%us", static_cast<unsigned>(uptimeSec));
        stamp = buf;
    }

    return stamp + " " + levelTag(level) + " " + (tag ? tag : "app") + ": " +
           clampMessage(message);
}

bool ErrorLogCore::submit(LogLevel level, const char* tag, const std::string& message)
{
    uint32_t uptime = 0;
    if (uptimeSec_) uptime = uptimeSec_();

    ErrorLogEntry entry;
    entry.level = level;
    entry.text = formatLine(level, tag, message,
                            static_cast<uint64_t>(std::time(nullptr)), uptime);

    if (queue_.push(entry)) return true;

    // The queue is full. Do **not** wait for it: the caller is a task the device
    // cannot spare. The loss is recorded and reported by the next line written.
    ++dropped_;
    dropNoticePending_ = true;
    return false;
}

uint32_t ErrorLogCore::drain(uint32_t timeoutMs)
{
    uint32_t written = 0;
    ErrorLogEntry entry;

    if (!queue_.pop(entry, timeoutMs)) {
        // Nothing at all to write: do not invent an empty line. The drop notice
        // waits for the next real message, so the log stays a log.
        return 0;
    }

    for (;;) {
        if (dropNoticePending_ && dropped_ > 0) {
            const std::string notice = std::to_string(dropped_) +
                                       " message(s) lost (queue full or the target refused them)";
            ErrorLogEntry note;
            note.level = LogLevel::Warn;
            note.text = formatLine(LogLevel::Warn, "log", notice,
                                   static_cast<uint64_t>(std::time(nullptr)),
                                   uptimeSec_ ? uptimeSec_() : 0);
            if (!target_.append(note.level, note.text)) ++dropped_;
            else { dropNoticePending_ = false; dropped_ = 0; ++written; }
        }

        if (target_.append(entry.level, entry.text)) {
            ++written;
        } else {
            // The target itself refused the line (volume gone, no space): count it
            // and do not loop on it — the next successful write will say so.
            ++dropped_;
            dropNoticePending_ = true;
        }

        if (!queue_.pop(entry, 0)) break;
    }

    return written;
}

} // namespace core
} // namespace dhcp
