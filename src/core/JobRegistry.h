#ifndef DHCP_CORE_JOBREGISTRY_H
#define DHCP_CORE_JOBREGISTRY_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dhcp {
namespace core {

/** @brief State of a long-running operation. */
enum class JobState {
    Running,    ///< Working right now.
    Paused,     ///< Stopped on purpose, waiting to be continued (resumable upload).
    Done,       ///< Finished successfully.
    Failed,     ///< Finished with an error.
    Cancelled,  ///< Stopped on request.
};

/** @brief Text id of a state, as it travels in JSON. */
const char* jobStateText(JobState state);

/**
 * @brief One long-running operation, as the scheduler page and the API see it.
 *
 * `titleKey` is an i18n key (`jobs.file_check`) and `arg` what the operation
 * works on (a volume id, a file path) — the UI composes the label from both and
 * never has to know which operation it is looking at.
 */
struct JobInfo {
    std::string id;         ///< Stable short id (`file_check`, `upload`, …).
    std::string titleKey;   ///< i18n key of the operation name.
    std::string arg;        ///< What it works on (volume, file path), may be empty.
    std::string detail;     ///< Current step (a file being read/written), may be empty.
    JobState state = JobState::Running;
    uint32_t done = 0;      ///< Progress; `total == 0` means "unknown".
    uint32_t total = 0;
    uint32_t durationMs = 0;    ///< Running time so far (frozen once finished).
    bool cancelRequested = false;
    uint32_t repeatSec = 0;     ///< 0 = one-off: the record goes when it finishes.

    /** @brief Percent for a bar, or -1 when the total is unknown. */
    int percent() const;
};

/**
 * @brief The list of long-running operations ("Task Scheduler").
 *
 * One place where an operation of any subsystem announces itself: what it is,
 * what it works on, how far it is and whether it may be stopped. The REST layer
 * serialises @ref snapshot, the scheduler page draws it, and a new operation only
 * has to call @ref begin / @ref progress / @ref finish — no endpoint of its own.
 *
 * Deliberately free of ESP-IDF dependencies beyond logging, so the rules below
 * are host-tested (`test/test_jobregistry.cpp`):
 *
 *  - a **one-off** operation is removed from the list the moment it ends; the
 *    list therefore answers "what is running now" and never grows stale. An
 *    operation that knows it will run again (`repeatSec > 0`) stays instead.
 *  - a **paused** operation is not finished (a resumable upload keeps its
 *    `<name>.part`), so its record stays until it is continued or discarded.
 *  - **every unfinished operation can be asked to stop** — there is no
 *    "cancellable" flag to check: while the record is there, it is running or
 *    waiting, and the operator may want it gone. `requestCancel` only *asks*: the
 *    subsystem that owns the operation acts on it (its own flag, its own
 *    cleanup) and decides how far it can get — a walk stops at the next entry, a
 *    format only once the card answers again. That keeps the layering clean: the
 *    registry never calls into the subsystems. Only a request for an operation
 *    that already ended is refused, because there is nothing left to stop.
 */
class JobRegistry {
public:
    /** @brief Slots available at once; the oldest finished record is reused. */
    static constexpr size_t kMaxJobs = 8;

    static JobRegistry& instance();

    /**
     * @brief Announce an operation that starts now.
     *
     * A record with the same @p id is replaced (single-flight per id): an
     * operation that resumes, or a second run of the same kind, takes over its
     * own entry instead of adding a duplicate.
     *
     * @param[in] id          Stable short id (`file_check`, `upload`, …).
     * @param[in] titleKey    i18n key of the name shown to the operator.
     * @param[in] arg         What it works on (volume id, file path), may be empty.
     * @param[in] total       Expected units of work (0 = unknown, no percentage).
     * @param[in] repeatSec   Seconds until the next scheduled run (0 = one-off).
     * @return false when every slot is taken by a running operation.
     */
    bool begin(const std::string& id, const std::string& titleKey,
               const std::string& arg = "", uint32_t total = 0,
               uint32_t repeatSec = 0);

    /**
     * @brief Update progress of a running operation.
     * @param[in] done   Units finished so far.
     * @param[in] total  Expected units in total (0 = keep the previous value).
     * @param[in] detail Current step (a file path, a volume), may be empty.
     */
    void progress(const std::string& id, uint32_t done, uint32_t total = 0,
                  const std::string& detail = "");

    /** @brief Mark the operation as waiting for the operator to continue it. */
    void pause(const std::string& id, const std::string& detail = "");

    /**
     * @brief End the operation.
     *
     * The record disappears for a one-off operation (see the class comment) and
     * stays for one with a scheduled repeat, where `Done` reads as "last run
     * finished, the next one is coming".
     */
    void finish(const std::string& id, JobState state = JobState::Done,
                const std::string& detail = "");

    /**
     * @brief Ask the owning subsystem to stop an unfinished operation.
     *
     * Only records the request (the page shows it at once); the subsystem polls
     * @ref cancelRequested or is stopped by the REST layer that knows it.
     *
     * @return false when no such operation is recorded or it already ended.
     */
    bool requestCancel(const std::string& id);

    /** @brief True when @ref requestCancel was called for a running operation. */
    bool cancelRequested(const std::string& id) const;

    /** @brief True when an operation with that id is recorded (any state). */
    bool contains(const std::string& id) const;

    /** @brief Copy of the list, newest first, with the duration filled in. */
    std::vector<JobInfo> snapshot() const;

    /** @brief Forget everything (tests and the factory reset path). */
    void clear();

private:
    JobRegistry() = default;
    JobRegistry(const JobRegistry&) = delete;
    JobRegistry& operator=(const JobRegistry&) = delete;

    /** @brief Fresh time stamp of the monotonic clock used for durations. */
    static std::chrono::milliseconds now();

    /** @brief Free slot, a reusable finished one, or `kMaxJobs` when full. */
    size_t slotFor(const std::string& id);

    mutable std::mutex mutex_;
    JobInfo jobs_[kMaxJobs];
    bool used_[kMaxJobs] = {};
    /** @brief Start and end of each record (the monotonic clock, milliseconds). */
    std::chrono::milliseconds started_[kMaxJobs] = {};
    std::chrono::milliseconds finished_[kMaxJobs] = {};
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_JOBREGISTRY_H
