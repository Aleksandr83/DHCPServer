#include "DeviceLog.h"

#include <ctime>

#include "DeviceLogFormat.h"
#include "ErrorLog.h"
#include "LogFileSizes.h"

#include "esp_log.h"
#include "esp_timer.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

constexpr const char* kTag = "DeviceLog";
/** Priority below the servers and below the web server: nobody waits for a log. */
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
/** Large enough for the stdio path plus the FATFS/VFS calls underneath it. */
constexpr uint32_t kTaskStack = 4096;
/** How long the writing task waits in the queue when there is nothing to write. */
constexpr uint32_t kWaitMs = 1000;
/** The tag every line carries: `[2026-10-19 02:40:11] device: …`. */
constexpr const char* kLineTag = "device";

/** @brief Seconds since boot, used when the clock is not set yet. */
uint32_t uptimeSec()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

/** @brief The shape of a line: the stamp in brackets, then `device:`, then the words. */
string deviceLine(LogLevel, const char* tag, const string& message)
{
    return "[" + ErrorLogCore::formatStamp(static_cast<uint64_t>(time(nullptr)),
                                           uptimeSec()) +
           "] " + (tag ? tag : kLineTag) + ": " + ErrorLogCore::clampMessage(message);
}

} // namespace

DeviceLog& DeviceLog::instance()
{
    static DeviceLog log;
    return log;
}

DeviceLog::~DeviceLog()
{
    // The task is deliberately left running: the only way out of this singleton is
    // the end of the program, and on the device that is a restart.
    core_.reset();
    target_.reset();
}

bool DeviceLog::start(const string& mountPoint, const string& cardMountPoint,
                      CardMountedCheck cardMounted)
{
    if (task_ != nullptr) return true;   // already started

    string dir = mountPoint;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    auto writer = make_unique<DeviceLogWriter>(
        dir + "/logs/Device.log", cardMountPoint, move(cardMounted),
        [](const string& message) {
            ErrorLog::instance().errorf(kTag, "%s", message.c_str());
        },
        fileLogSize_);
    target_ = make_unique<ArchiveFileLogTarget>(move(writer));
    core_ = make_unique<ErrorLogCore>(queue_, *target_, &uptimeSec, deviceLine);

    if (!queue_.ready()) {
        ESP_LOGE(kTag, "no queue — the device journal cannot be started");
        return false;
    }

    const BaseType_t res = xTaskCreate(taskEntry, "device_log", kTaskStack, this,
                                       kTaskPriority, &task_);
    if (res != pdTRUE) {
        task_ = nullptr;
        ESP_LOGE(kTag, "failed to create the device journal task");
        return false;
    }
    ESP_LOGI(kTag, "device journal started at %s", target_->description().c_str());
    return true;
}

void DeviceLog::taskEntry(void* arg)
{
    static_cast<DeviceLog*>(arg)->run();
    vTaskDelete(nullptr);
}

void DeviceLog::run()
{
    for (;;) {
        // Waits in the queue, writes what is there, goes back to waiting. The
        // target flushes every line, so a restart cannot lose one.
        core_->drain(kWaitMs);
    }
}

void DeviceLog::setFileLogSize(size_t bytes)
{
    fileLogSize_ = bytes;
    if (target_) target_->writer().setMaxBytes(bytes);
}

const string& DeviceLog::target() const
{
    static const string empty;
    return target_ ? target_->description() : empty;
}

uint32_t DeviceLog::dropped() const
{
    return preStartDropped_ + (core_ ? core_->dropped() : 0);
}

bool DeviceLog::submit(LogLevel level, const string& text)
{
    // The switch is asked here, not by the callers: one place decides what this
    // journal keeps, and no caller has to know.
    if (!logFile_) return false;
    if (!core_) { ++preStartDropped_; return false; }

    return core_->submit(level, kLineTag, text);
}

void DeviceLog::noteBoot(const string& version)
{
    submit(LogLevel::Info, deviceBootText(version));
}

void DeviceLog::noteBootAfter(const string& version, const string& reason)
{
    submit(LogLevel::Info, deviceBootAfterText(version, reason));
}

void DeviceLog::noteRestart(const string& source)
{
    submit(LogLevel::Info, deviceRestartText(source));
}

void DeviceLog::noteFactoryReset(const string& source)
{
    submit(LogLevel::Warn, deviceFactoryResetText(source));
}

void DeviceLog::noteSettingsImported()
{
    submit(LogLevel::Info, deviceSettingsImportText());
}

void DeviceLog::noteSshCrash(const string& stage)
{
    submit(LogLevel::Warn, deviceSshCrashText(stage));
}

void DeviceLog::noteLibssh(const string& function, const string& message)
{
    submit(LogLevel::Info, deviceLibsshText(function, message));
}

} // namespace core
} // namespace dhcp
