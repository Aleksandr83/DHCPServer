#include "JobLog.h"
#include "ErrorLog.h"
#include "ArchiveFileLogTarget.h"
#include "JobsLogWriter.h"

#include <ctime>

#include "JobLogFormat.h"
#include "SweepLogFormat.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage/IFileSystem.h"

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
/** Tag of the lines this log writes about the sweep log itself. */
constexpr const char* kDetailTag = "cache_autoupdate";
/** How long the sweep's own log gets to flush what the queue still holds. */
constexpr uint32_t kDetailDrainMs = 5000;

uint32_t uptimeSec()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
}

/**
 * @brief The line shape the operator asked for: the stamp first, in brackets.
 *
 * The message is clamped like the error log's, so one queue item still holds a
 * whole line. Shared by both files this class writes — the journal and the
 * per-record detail log (stage 179) read the same way.
 */
string jobLine(LogLevel, const char* tag, const string& message)
{
    return "[" + ErrorLogCore::formatStamp(
                     static_cast<uint64_t>(time(nullptr)), uptimeSec()) +
           "] " + (tag ? tag : "job") + ": " +
           ErrorLogCore::clampMessage(message);
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
    detailCore_.reset();
    detailTarget_.reset();
}

bool JobLog::start(const string& mountPoint, storage::IFileSystem* sweepVolume)
{
    if (task_ != nullptr) return true;   // already started

    string dir = mountPoint;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    // The journal's file is a `LogFileWriter` too (stage 206), so a full one is
    // copied to the card as `log/Archive/Jobs/...`; the card is the same volume the
    // per-sweep files live on, and it is asked at every rotation whether it is
    // still there. A failed archive is reported into `Errors.log`, like every
    // other log's.
    {
        auto writer = make_unique<JobsLogWriter>(
            dir + "/logs/Jobs.log",
            sweepVolume != nullptr ? sweepVolume->mountPoint() : string(),
            [sweepVolume] {
                return sweepVolume != nullptr && sweepVolume->isMounted();
            },
            [](const string& message) {
                ErrorLog::instance().errorf(kTag, "%s", message.c_str());
            });
        target_ = make_unique<ArchiveFileLogTarget>(move(writer));
    }
    core_ = make_unique<ErrorLogCore>(queue_, *target_, &uptimeSec, jobLine);

    // The auto-update's per-record log (stage 179): a target of its own, drained
    // by this same task. It never changes once it exists — the file behind it is
    // opened and closed by begin/endAutoUpdateFile, one sweep at a time (stage
    // 186) — because the writing task reads it from another thread.
    sweepVolume_ = sweepVolume;
    detailTarget_ = make_unique<SweepDetailTarget>(
        sweepVolume != nullptr ? sweepVolume->mountPoint() + "/" + kSweepLogFolder
                               : string("(no volume)"));
    detailCore_ = make_unique<ErrorLogCore>(detailQueue_, *detailTarget_, &uptimeSec,
                                            jobLine);

    if (!queue_.ready() || !detailQueue_.ready()) {
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
    ESP_LOGI(kTag, "job log started at %s (details: %s)",
             target_->description().c_str(), detailTarget_->description().c_str());
    return true;}

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
        // Whatever the detail queue holds goes out right after, without waiting:
        // it is the same promise (a line on disk survives the restart) in a
        // second file, and the auto-update's records are the only producers.
        if (detailCore_) detailCore_->drain(0);
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
    return preStartDropped_ + (core_ ? core_->dropped() : 0) +
           (detailCore_ ? detailCore_->dropped() : 0);
}

string JobLog::autoUpdateTarget() const
{
    // The file of the sweep being logged rather than the folder: a page asking
    // "where is the detail log" wants the file to open (stage 186); between
    // sweeps there is none, and that is an empty answer, not the folder.
    return detailTarget_ ? detailTarget_->currentFile() : string();
}

bool JobLog::autoUpdate(const char* tag, const string& text)
{
    if (!detailCore_) { ++preStartDropped_; return false; }
    // Outside a sweep there is no file to write into, and that is not a lost
    // line: answering false here keeps the drop counter honest (a sweep that
    // cannot log — no card, no clock — would otherwise report tens of thousands
    // of "lost messages" at the end of the journal).
    if (detailTarget_ == nullptr || !detailTarget_->active()) return false;
    return detailCore_->submit(LogLevel::Info, tag, text);
}

void JobLog::noteDetailSkipped(const string& reason)
{
    if (!core_) return;
    core_->submit(LogLevel::Info, kDetailTag,
                  "detailed log skipped: " + reason);
}

bool JobLog::beginAutoUpdateFile()
{
    if (detailTarget_ == nullptr || detailCore_ == nullptr) return false;

    if (sweepVolume_ == nullptr || !sweepVolume_->isMounted()) {
        noteDetailSkipped("no card");
        return false;
    }

    const time_t now = time(nullptr);
    if (!ErrorLogCore::clockIsSet(static_cast<uint64_t>(now))) {
        // A file named 1970.01.01_00.00.log would be a lie, and "Update now" is
        // allowed without a synced clock — the sweep goes on, the file does not.
        noteDetailSkipped("clock is not set");
        return false;
    }

    tm local{};
    if (!ErrorLogCore::localTime(now, local)) {
        noteDetailSkipped("no local time");
        return false;
    }

    const string name = sweepLogName(local);
    if (!detailTarget_->begin(sweepVolume_->mountPoint(), name, kSweepLogKeep)) {
        noteDetailSkipped("cannot create " + name);
        return false;
    }

    ESP_LOGI(kTag, "sweep log: %s", detailTarget_->currentFile().c_str());
    return true;
}

void JobLog::endAutoUpdateFile()
{
    if (detailTarget_ == nullptr || !detailTarget_->active()) return;

    const string file = detailTarget_->currentFile();
    // The lines of the sweep are still in the queue: the file is closed only
    // after the writer has put them in it, or the end of a sweep — the part that
    // says how it went — would be the part that is missing.
    if (detailCore_) detailCore_->drain(kDetailDrainMs);
    const uint64_t lines = detailTarget_->lines();
    detailTarget_->end();

    if (core_) {
        core_->submit(LogLevel::Info, kDetailTag,
                      "detailed log closed: " + file + " (" + to_string(lines) +
                          " lines)");
    }
}

} // namespace core
} // namespace dhcp
