#include "SshLog.h"

#include <ctime>

#include "ErrorLog.h"
#include "LogFileSizes.h"
#include "SshLogFormat.h"

#include "esp_log.h"
#include "esp_timer.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

constexpr const char* kTag = "SshLog";
/** Priority below the servers and below the web server: nobody waits for a log. */
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
/** Large enough for the stdio path plus the FATFS/VFS calls underneath it. */
constexpr uint32_t kTaskStack = 4096;
/** How long the writing task waits in the queue when there is nothing to write. */
constexpr uint32_t kWaitMs = 1000;
/** The tag every line carries: `[2026-10-18 20:15:02] ssh: …`. */
constexpr const char* kLineTag = "ssh";

/** @brief Seconds since boot, used when the clock is not set yet. */
uint32_t uptimeSec()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

/** @brief The shape of a line: the stamp in brackets, then `ssh:`, then the words. */
string sshLine(LogLevel, const char* tag, const string& message)
{
    return "[" + ErrorLogCore::formatStamp(static_cast<uint64_t>(time(nullptr)),
                                           uptimeSec()) +
           "] " + (tag ? tag : kLineTag) + ": " + ErrorLogCore::clampMessage(message);
}

} // namespace

SshLog& SshLog::instance()
{
    static SshLog log;
    return log;
}

SshLog::~SshLog()
{
    // The task is deliberately left running: the only way out of this singleton is
    // the end of the program, and on the device that is a restart.
    core_.reset();
    target_.reset();
}

bool SshLog::start(const string& mountPoint, const string& cardMountPoint,
                   CardMountedCheck cardMounted)
{
    if (task_ != nullptr) return true;   // already started

    string dir = mountPoint;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    auto writer = make_unique<SshLogWriter>(
        dir + "/logs/SSH.log", cardMountPoint, move(cardMounted),
        [](const string& message) {
            ErrorLog::instance().errorf(kTag, "%s", message.c_str());
        },
        fileLogSize_);
    target_ = make_unique<ArchiveFileLogTarget>(move(writer));
    core_ = make_unique<ErrorLogCore>(queue_, *target_, &uptimeSec, sshLine);

    if (!queue_.ready()) {
        ESP_LOGE(kTag, "no queue — the SSH journal cannot be started");
        return false;
    }

    const BaseType_t res = xTaskCreate(taskEntry, "ssh_log", kTaskStack, this,
                                       kTaskPriority, &task_);
    if (res != pdTRUE) {
        task_ = nullptr;
        ESP_LOGE(kTag, "failed to create the SSH journal task");
        return false;
    }
    ESP_LOGI(kTag, "SSH journal started at %s", target_->description().c_str());
    return true;
}

void SshLog::taskEntry(void* arg)
{
    static_cast<SshLog*>(arg)->run();
    vTaskDelete(nullptr);
}

void SshLog::run()
{
    for (;;) {
        // Waits in the queue, writes what is there, goes back to waiting. The
        // target flushes every line, so a restart cannot lose one.
        core_->drain(kWaitMs);
    }
}

void SshLog::setFileLogSize(size_t bytes)
{
    fileLogSize_ = bytes;
    if (target_) target_->writer().setMaxBytes(bytes);
}

const string& SshLog::target() const
{
    static const string empty;
    return target_ ? target_->description() : empty;
}

uint32_t SshLog::dropped() const
{
    return preStartDropped_ + (core_ ? core_->dropped() : 0);
}

bool SshLog::submit(LogLevel level, const string& text)
{
    // Both switches are asked here and not by the callers: one place decides what
    // this journal keeps and shows, and the SSH task never has to know.
    if (!logFile_ && !logTerminal_) return false;

    if (logTerminal_) {
        ESP_LOGI(kTag, "%s", text.c_str());
    }
    if (!logFile_) return false;
    if (!core_) { ++preStartDropped_; return false; }

    return core_->submit(level, kLineTag, text);
}

void SshLog::noteSessionOpen(const string& clientIp, const string& user)
{
    submit(LogLevel::Info, sshSessionOpenText(clientIp, user));
}

void SshLog::noteSessionClose(const string& clientIp, uint32_t durationSec)
{
    submit(LogLevel::Info, sshSessionCloseText(clientIp, durationSec));
}

void SshLog::noteAuthFailed(const string& clientIp, uint32_t attempt,
                            uint32_t maxAttempts)
{
    // A refused password is a warning, not an error: the device is doing its job.
    submit(LogLevel::Warn, sshAuthFailedText(clientIp, attempt, maxAttempts));
}

void SshLog::noteSubnetRefused(const string& clientIp)
{
    submit(LogLevel::Warn, sshSubnetRefusedText(clientIp));
}

void SshLog::noteSessionLimit(const string& clientIp, uint32_t limit)
{
    submit(LogLevel::Warn, sshSessionLimitText(clientIp, limit));
}

} // namespace core
} // namespace dhcp
