#ifndef DHCP_CORE_SWEEPDETAILTARGET_H
#define DHCP_CORE_SWEEPDETAILTARGET_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "IErrorLogTarget.h"
#include "SweepFileLogTarget.h"

namespace dhcp {
namespace core {

/**
 * @file SweepDetailTarget.h
 * @brief The detail log of the cache auto-update as a target that is there even
 *        when no sweep is writing (stage 186).
 *
 * The per-record log of a sweep is one file per sweep now, on the card, and that
 * file exists only while a sweep runs. The log itself, however, is wired once at
 * boot: `ErrorLogCore` holds a **reference** to its target, and the writing task
 * drains a queue on another thread — so swapping the target under it, or the
 * target pointer, would be a data race on a `unique_ptr` the task is using.
 *
 * This class is the fixed point that makes the swap safe: it never changes, it
 * holds the sink behind a mutex, and it answers a line with `false` when no sweep
 * is writing — which is a true statement, not an error: outside a sweep the
 * per-record log has nothing to say.
 *
 * The current file is read under the same lock (`currentFile()`), and the one
 * string that is handed out by reference — @ref description — is fixed at
 * construction, so no reader can hold a reference to something being replaced.
 */
class SweepDetailTarget : public IErrorLogTarget {
public:
    /**
     * @param description Where sweeps are logged, for the log's own startup line
     *                    (e.g. `/sdcard/log/AutoUpdate`); never changed after.
     */
    explicit SweepDetailTarget(std::string description)
        : description_(std::move(description))
    {
    }

    /**
     * @brief Open the file of the sweep that is starting and make it the sink.
     *
     * A sink left over from an earlier sweep (its close was missed) is closed by
     * this call rather than written to.
     *
     * @return false when the volume refused the file; the caller says so in the
     *         journal instead of writing into nothing.
     */
    bool begin(const std::string& mountPoint, const std::string& fileName,
               size_t keep);

    /** @brief Close the current file. Lines are refused until @ref begin again. */
    void end();

    bool append(LogLevel level, const std::string& line) override;
    const std::string& description() const override { return description_; }

    /** @brief True while a sweep's file is open and can be written to. */
    bool active() const;
    /** @brief The file of the sweep being logged, or "" between sweeps. */
    std::string currentFile() const;
    /** @brief Lines written into the current file. */
    uint64_t lines() const;

private:
    mutable std::mutex mutex_;
    std::unique_ptr<SweepFileLogTarget> sink_;
    const std::string description_;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SWEEPDETAILTARGET_H
