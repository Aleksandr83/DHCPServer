#include "ErrorLog.h"

#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

constexpr const char* kTag = "ErrorLog";
/** Priority below the servers: nothing waits for the log, ever. */
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
/** Large enough for the stdio path plus the FATFS/VFS calls underneath it. */
constexpr uint32_t kTaskStack = 4096;
/** How long the task sleeps in the queue when there is nothing to write. */
constexpr uint32_t kWaitMs = 1000;

uint32_t uptimeSec()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
}

} // namespace

ErrorLog& ErrorLog::instance()
{
    static ErrorLog log;
    return log;
}

ErrorLog::~ErrorLog()
{
    // The task is deliberately left running: the only way out of this singleton
    // is the end of the program, and on the device that is a restart.
    core_.reset();
    target_.reset();
}

bool ErrorLog::start(const string& mountPoint)
{
    if (task_ != nullptr) return true;   // already started

    string dir = mountPoint;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    // Look before setting anything up: a volume that is not there is worth
    // saying out loud (the terminal is the only place left for that), and the
    // target refuses to create it — see FileErrorLogTarget.
    struct stat st {};
    if (dir.empty() || stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(kTag, "volume %s is not mounted — the error log will stay empty "
                       "until it is (nothing is created blindly)",
                 dir.empty() ? "?" : dir.c_str());
    }

    target_ = make_unique<FileErrorLogTarget>(dir + "/logs/Errors.log");
    core_ = make_unique<ErrorLogCore>(queue_, *target_, &uptimeSec);

    if (!queue_.ready()) {
        ESP_LOGE(kTag, "no queue — the error log cannot be started");
        return false;
    }

    const BaseType_t res = xTaskCreate(taskEntry, "err_log", kTaskStack, this,
                                       kTaskPriority, &task_);
    if (res != pdTRUE) {
        task_ = nullptr;
        ESP_LOGE(kTag, "failed to create the error log task");
        return false;
    }
    ESP_LOGI(kTag, "error log started at %s", target_->description().c_str());
    return true;
}

void ErrorLog::taskEntry(void* arg)
{
    static_cast<ErrorLog*>(arg)->run();
    vTaskDelete(nullptr);
}

void ErrorLog::run()
{
    for (;;) {
        // Waits in the queue, writes what is there, goes back to waiting. The
        // target flushes and closes every line, so a restart cannot lose one.
        core_->drain(kWaitMs);
    }
}

const string& ErrorLog::target() const
{
    static const string empty;
    return target_ ? target_->description() : empty;
}

bool ErrorLog::error(const char* tag, const string& message)
{
    if (!core_) { ++preStartDropped_; return false; }
    return core_->submit(LogLevel::Error, tag, message);
}

bool ErrorLog::warn(const char* tag, const string& message)
{
    if (!core_) { ++preStartDropped_; return false; }
    return core_->submit(LogLevel::Warn, tag, message);
}

bool ErrorLog::errorf(const char* tag, const char* fmt, ...)
{
    char buffer[ErrorLogCore::kMaxMessage + 1];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    (void)n;
    return error(tag, string(buffer));
}

uint32_t ErrorLog::dropped() const
{
    return preStartDropped_ + (core_ ? core_->dropped() : 0);
}

} // namespace core
} // namespace dhcp
