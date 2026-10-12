#ifndef DHCP_CORE_SWEEPFILELOGTARGET_H
#define DHCP_CORE_SWEEPFILELOGTARGET_H

#include <cstdint>
#include <cstdio>
#include <string>

#include "IErrorLogTarget.h"

namespace dhcp {
namespace core {

/**
 * @file SweepFileLogTarget.h
 * @brief One file per cache-auto-update sweep, on a volume, kept open for the
 *        whole sweep (stage 186).
 *
 * The other log files are written a line at a time — open, write, flush, close —
 * because a line that survives the restart it describes is worth the cost. This
 * file is different in both directions:
 *
 *   * it is one file **per sweep** (`log/AutoUpdate/2026.10.11_01.34.log`), so it
 *     says how a whole sweep went instead of the last few hundred lines, and the
 *     64 KB cap with its single rotation is deliberately not applied here;
 *   * a sweep of 59520 records is ~5 MB, and 59520 open/flush/close cycles on a
 *     card would cost minutes on their own — so the handle is kept open and the
 *     data is flushed every @ref kFlushEveryLines lines and at close.
 *
 * The trade is stated where it is made: a restart in the middle of a sweep loses
 * the last few lines of **that** log. The file is a description of an operation
 * that was interrupted anyway, and the journal (`Jobs.log`, which does flush per
 * line) still holds the two ends of it.
 *
 * The class owns the file and nothing else: the folder chain, the name and the
 * retention rule come from `SweepLogFormat.h`, so the file layout is decided in
 * one place that has tests.
 */
class SweepFileLogTarget : public IErrorLogTarget {
public:
    /** @brief Lines written before the file is flushed to the volume. */
    static constexpr uint32_t kFlushEveryLines = 32;
    /** @brief How many attempts `mkdir` gets when a level appears twice. */
    static constexpr int kMkdirAttempts = 2;

    ~SweepFileLogTarget() override { close(); }

    SweepFileLogTarget() = default;
    SweepFileLogTarget(const SweepFileLogTarget&) = delete;
    SweepFileLogTarget& operator=(const SweepFileLogTarget&) = delete;

    /**
     * @brief Create the folders, drop the files over the limit, open the file.
     *
     * @param mountPoint Volume the log lives on (`/sdcard`).
     * @param fileName   Name from @ref sweepLogName; an existing name is
     *                   appended to, never overwritten — two sweeps in the same
     *                   minute are two sweeps, and both are worth having.
     * @param keep       How many of the newest files to keep (the older go now).
     * @return false when the volume refused the folders or the file; the caller
     *         reports that instead of writing into nothing.
     */
    bool open(const std::string& mountPoint, const std::string& fileName,
              size_t keep);

    /** @brief Flush and close; harmless when the file is already closed. */
    void close();

    bool append(LogLevel level, const std::string& line) override;
    const std::string& description() const override { return path_; }

    /** @brief Lines written since @ref open (0 before the first sweep). */
    uint64_t lines() const { return lines_; }
    /** @brief True while the file is open and can be written to. */
    bool isOpen() const { return file_ != nullptr; }

private:
    /** @brief Create every level of @p chain; false on the first real failure. */
    static bool createFolders(const std::string& folder);
    /** @brief Remove the sweep logs that are over @p keep. */
    static void prune(const std::string& folder, size_t keep);

    FILE* file_ = nullptr;
    std::string path_;
    uint32_t sinceFlush_ = 0;
    uint64_t lines_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SWEEPFILELOGTARGET_H
