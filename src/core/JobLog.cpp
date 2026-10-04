#include "JobLog.h"

#include <ctime>

#include "JobLogFormat.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

constexpr const char* kTag = "JobLog";
/** Priority below the servers: nothing waits for the log, ever. */
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
/** Large enough for the stdio path plus the FATFS/VFS calls underneath it. */
constexpr uint32_t kTaskStack = 4096;
/** How long the task sleeps in the queue when there is nothing to write. */
constexpr uint32_t kWaitMs = 1000;
/** The word between the stamp and the operation ("job: …"). */
constexpr const char* kLineTag = "job";

uint32_t uptimeSec()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
}

} // namespace

JobLog& JobLog::instance()
{
    static JobLog log;
    return log;
}

JobLog::~JobLog()
{
    // The task is deliberately left running: the only way out of this singleton
    // is the end of the program, and on the device that is a restart.
    core_.reset();
    target_.reset();
}

bool JobLog::start(const string& mountPoint)
{
    if (task_ != nullptr) return true;   // already started

    string dir = mountPoint;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    target_ = make_unique<FileErrorLogTarget>(dir + "/logs/Jobs.log");
    // The line shape the operator asked for: the stamp first, in brackets. The
    // message is clamped like the error log's, so one queue item still holds a
    // whole line.
    core_ = make_unique<ErrorLogCore>(
        queue_, *target_, &uptimeSec,
        [](LogLevel, const char* tag, const string& message) {
            return "[" + ErrorLogCore::formatStamp(
                             static_cast<uint64_t>(time(nullptr)), uptimeSec()) +
                   "] " + (tag ? tag : "job") + ": " +
                   ErrorLogCore::clampMessage(message);
        });

    if (!queue_.ready()) {
        ESP_LOGE(kTag, "no queue — the job log cannot be started");
        return false;
    }

    const BaseType_t res = xTaskCreate(taskEntry, "job_log", kTaskStack, this,
                                       kTaskPriority, &task_);
    if (res != pdTRUE) {
        task_ = nullptr;
        ESP_LOGE(kTag, "failed to create the job log task");
        return false;
    }
    ESP_LOGI(kTag, "job log started at %s", target_->description().c_str());
    return true;
}

void JobLog::taskEntry(void* arg)
{
    static_cast<JobLog*>(arg)->run();
    vTaskDelete(nullptr);
}

void JobLog::run()
{
    for (;;) {
        // Waits in the queue, writes what is there, goes back to waiting. The
        // target flushes and closes every line, so a restart cannot lose one.
        core_->drain(kWaitMs);
    }
}

const string& JobLog::target() const
{
    static const string empty;
    return target_ ? target_->description() : empty;
}

void JobLog::jobStarted(const JobInfo& job)
{
    if (!core_) { ++preStartDropped_; return; }
    core_->submit(LogLevel::Info, kLineTag, jobStartedText(job.id, job.arg));
}

void JobLog::jobFinished(const JobInfo& job)
{
    if (!core_) { ++preStartDropped_; return; }
    core_->submit(LogLevel::Info, kLineTag,
                  jobFinishedText(job.id, job.arg, jobStateText(job.state),
                                  job.durationMs, job.detail));
}

uint32_t JobLog::dropped() const
{
    return preStartDropped_ + (core_ ? core_->dropped() : 0);
}

} // namespace core
} // namespace dhcp
