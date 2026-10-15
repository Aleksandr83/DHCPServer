#include "AuthLog.h"

#include <ctime>

#include "AuthLogFormat.h"
#include "ErrorLog.h"
#include "LogFileSizes.h"

#include "esp_log.h"
#include "esp_timer.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

constexpr const char* kTag = "AuthLog";
/** Priority below the servers and below the web server: nobody waits for a log. */
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
/** Large enough for the stdio path plus the FATFS/VFS calls underneath it. */
constexpr uint32_t kTaskStack = 4096;
/** How long the writing task waits in the queue when there is nothing to write. */
constexpr uint32_t kWaitMs = 1000;
/** The tag every line carries: `[2026-10-12 19:22:14] auth: …`. */
constexpr const char* kLineTag = "auth";

/** @brief Seconds since boot, used when the clock is not set yet. */
uint32_t uptimeSec()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

/**
 * @brief The shape of a line: the stamp in brackets, then `auth:`, then the words.
 *
 * The same shape the journal uses (`ErrorLogCore`'s formatter, supplied by the
 * owner), so a reader of both files does not have two formats to learn.
 */
string authLine(LogLevel, const char* tag, const string& message)
{
    return "[" + ErrorLogCore::formatStamp(static_cast<uint64_t>(time(nullptr)),
                                           uptimeSec()) +
           "] " + (tag ? tag : kLineTag) + ": " + ErrorLogCore::clampMessage(message);
}

} // namespace

AuthLog& AuthLog::instance()
{
    static AuthLog log;
    return log;
}

AuthLog::~AuthLog()
{
    // The task is deliberately left running: the only way out of this singleton is
    // the end of the program, and on the device that is a restart.
    core_.reset();
    target_.reset();
}

bool AuthLog::start(const string& mountPoint, const string& cardMountPoint,
                    CardMountedCheck cardMounted)
{
    if (task_ != nullptr) return true;   // already started

    string dir = mountPoint;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    // The file is a `LogFileWriter` (through the adapter), so a full auth log is
    // copied to the card like the other file logs. A failed archive is reported
    // into `Errors.log` — the file the operator reads when something is wrong —
    // and not into this one, which is the log of *attempts*, not of faults.
    auto writer = make_unique<AuthLogWriter>(
        dir + "/logs/Auth.log", cardMountPoint, move(cardMounted),
        [](const string& message) {
            ErrorLog::instance().errorf(kTag, "%s", message.c_str());
        },
        fileLogSize_);
    target_ = make_unique<ArchiveFileLogTarget>(move(writer));
    core_ = make_unique<ErrorLogCore>(queue_, *target_, &uptimeSec, authLine);

    if (!queue_.ready()) {
        ESP_LOGE(kTag, "no queue — the authorisation log cannot be started");
        return false;
    }

    const BaseType_t res = xTaskCreate(taskEntry, "auth_log", kTaskStack, this,
                                       kTaskPriority, &task_);
    if (res != pdTRUE) {
        task_ = nullptr;
        ESP_LOGE(kTag, "failed to create the authorisation log task");
        return false;
    }
    ESP_LOGI(kTag, "authorisation log started at %s", target_->description().c_str());
    return true;
}

void AuthLog::taskEntry(void* arg)
{
    static_cast<AuthLog*>(arg)->run();
    vTaskDelete(nullptr);
}

void AuthLog::run()
{
    for (;;) {
        // Waits in the queue, writes what is there, goes back to waiting. The
        // target flushes every line, so a restart cannot lose one.
        core_->drain(kWaitMs);
    }
}

void AuthLog::setFileLogSize(size_t bytes)
{
    fileLogSize_ = bytes;
    if (target_) target_->writer().setMaxBytes(bytes);
}

const string& AuthLog::target() const
{
    static const string empty;
    return target_ ? target_->description() : empty;
}

uint32_t AuthLog::dropped() const
{
    return preStartDropped_ + (core_ ? core_->dropped() : 0);
}

bool AuthLog::submit(LogLevel level, const string& text)
{
    // The switch is asked here and not by the callers: one place decides whether
    // this log keeps anything, and the HTTP task never has to know. A sentence that
    // arrives while the log is off is not a loss — it was not meant to be written.
    if (!logFile_) return false;
    if (!core_) { ++preStartDropped_; return false; }

    return core_->submit(level, kLineTag, text);
}

void AuthLog::noteFailedAttempt(const string& clientIp, uint32_t attempt,
                                uint32_t maxAttempts)
{
    // A refused password is a warning, not an error: the device is doing its job,
    // and the level keeps the line distinguishable from a fault.
    submit(LogLevel::Warn, authFailedText(clientIp, attempt, maxAttempts));
}

void AuthLog::noteLockout(const string& clientIp, uint32_t lockoutSec)
{
    submit(LogLevel::Warn, authLockoutText(clientIp, lockoutSec));
}

void AuthLog::noteLogin(const string& clientIp, uint32_t failedBefore)
{
    submit(LogLevel::Info, authLoginText(clientIp, failedBefore));
}

} // namespace core
} // namespace dhcp
