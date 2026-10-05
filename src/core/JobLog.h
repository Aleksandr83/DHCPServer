#ifndef DHCP_CORE_JOBLOG_H
#define DHCP_CORE_JOBLOG_H

#include <cstdint>
#include <memory>
#include <string>

#include "ErrorLogCore.h"
#include "FileErrorLogTarget.h"
#include "FreeRtosErrorQueue.h"
#include "JobRegistry.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dhcp {
namespace core {

/**
 * @brief The long-running-operations log: `logs/Jobs.log` on a volume (stage 173).
 *
 * The error log answers "what went wrong"; this file answers "what has the device
 * been doing" — every long-running operation (a cache save, an upload, a volume
 * check, a firmware update) writes one line when it starts and one when it ends,
 * with its result and how long it took. The terminal is not always attached, and
 * the scheduler page only ever shows what is running *now*; a reboot wipes it.
 *
 * Shape and guarantees are the error log's, because this class is a subscriber of
 * the same machinery rather than a second implementation of it:
 *
 *   * producers (the registry, from whatever task began the operation) only
 *     enqueue — they never touch the filesystem and never block;
 *   * one low-priority task writes the lines, so a FAT write cannot stall the
 *     caller;
 *   * a line whose queue is full is lost **and counted**, and the count is
 *     written into the file with the next line;
 *   * the file is capped and rotated by `FileErrorLogTarget` (64 KB → `.1`).
 *
 * The line shape is the operator's: the stamp comes **first, in brackets**
 * (`[2026-10-04 02:06:43] job: file_check (/sdcard): finished (done, 12.3 s)`),
 * which is why this log supplies its own formatter to @ref ErrorLogCore instead
 * of reusing the error log's — that one's shape is left exactly as it was.
 *
 * Only the two ends are written, never progress: an operation reports its
 * percentage dozens of times, and a file that answered "what was going on" would
 * drown in it.
 *
 * One operation needed more than that — the cache auto-update, whose whole job is
 * a list of records — so its per-record lines go to a **second file**
 * (`logs/AutoUpdate.log`) written by this same task (stage 179). The journal is
 * untouched: a reader of `Jobs.log` still sees two lines per operation.
 */
class JobLog : public IJobObserver {
public:
    static JobLog& instance();

    /**
     * @brief Point the log at a volume and start the writing task.
     * @param mountPoint e.g. "/fat" → the file is `<mountPoint>/logs/Jobs.log`
     * @return false when the task could not be created (lines keep queueing until
     *         the queue fills, then they are counted as drops).
     */
    bool start(const std::string& mountPoint = "/fat");

    // ─── IJobObserver: one line per end, no progress ───
    void jobStarted(const JobInfo& job) override;
    void jobFinished(const JobInfo& job) override;

    /** @brief The file the lines go to ("" before start()). */
    const std::string& target() const;

    /** @brief Lines lost so far (queue full, target refused them, or no task). */
    uint32_t dropped() const;

    /**
     * @brief One detail line into `logs/AutoUpdate.log` (stage 179).
     *
     * The cache auto-update refreshes one record at a time, and the interesting
     * part is **which** record and what came back — the journal deliberately
     * holds only the two ends of an operation (stage 173), so those lines go to a
     * file of their own. The guarantees are the journal's, and so is the writer:
     * a queue the producer never blocks on, one low-priority task, the same 64 KB
     * cap and one `.1` generation, and a FAT write that never stalls the refresh
     * cycle because it happens in the log's task.
     *
     * @param tag  the operation the line belongs to (`cache_autoupdate`)
     * @param text the line without the stamp (`example.com A refreshed (ttl 300 s)`)
     * @return false when the line did not fit the queue or the file refused it.
     */
    bool autoUpdate(const char* tag, const std::string& text);

    /** @brief The detail file the lines go to ("" before start()). */
    const std::string& autoUpdateTarget() const;

private:
    JobLog() = default;
    ~JobLog();
    JobLog(const JobLog&) = delete;
    JobLog& operator=(const JobLog&) = delete;

    static void taskEntry(void* arg);
    void run();

    FreeRtosErrorQueue queue_;
    std::unique_ptr<FileErrorLogTarget> target_;
    std::unique_ptr<ErrorLogCore> core_;
    /// The per-record detail log of the cache auto-update (stage 179): its own
    /// queue and target, drained by the same task.
    FreeRtosErrorQueue detailQueue_;
    std::unique_ptr<FileErrorLogTarget> detailTarget_;
    std::unique_ptr<ErrorLogCore> detailCore_;
    TaskHandle_t task_ = nullptr;
    /** Kept separately: before start() there is no core to count into. */
    uint32_t preStartDropped_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_JOBLOG_H
