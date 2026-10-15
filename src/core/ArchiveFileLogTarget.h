#ifndef DHCP_CORE_ARCHIVEFILELOGTARGET_H
#define DHCP_CORE_ARCHIVEFILELOGTARGET_H

#include <functional>
#include <memory>
#include <string>

#include "IErrorLogTarget.h"
#include "LogFileWriter.h"

namespace dhcp {
namespace core {

/**
 * @brief The error-log seam over a file writer (stage 206).
 *
 * `Errors.log` and `Jobs.log` are not written by a logger of their own: a producer
 * hands a message to `ErrorLogCore`, which formats it and gives the finished line
 * to an @ref IErrorLogTarget, and one low-priority task drains the queue. That seam
 * is where the two logs get the policy every other file log in the project already
 * has — the cap the operator chooses, an archive on the card when the file is
 * full, and the newest lines kept when there is no card to archive to.
 *
 * The pattern, per rule 44, is an **Adapter**: what `IErrorLogTarget` speaks is "a
 * formatted line", what @ref LogFileWriter speaks is "a line and the stamp of the
 * moment it was written", and this class is the translation between the two. It
 * also decides nothing: the cap, the rotation, the archive folder and the failure
 * text belong to the writer it was handed, and a log that needs a different place
 * on the card is a different writer, not a change here.
 *
 * Two details are the whole of the class:
 *
 *   * **the stamp is read here**, at the moment of the write, because the line
 *     arrives without one and a rotation needs the date of what is inside the file
 *     (the range folder is named from it). The device's local time is the default,
 *     and a host test injects a clock to know what that folder will be.
 *   * **a line that cannot be stored comes back as `false`**, so `ErrorLogCore`
 *     counts it as a drop and says so — a target that answered `true` to a write
 *     that never happened would make a log with holes look complete.
 */
class ArchiveFileLogTarget : public IErrorLogTarget {
public:
    /** @brief The stamp of the line being written (the device clock by default). */
    using Clock = std::function<LogStamp()>;

    /**
     * @param writer the log this target stores into (its live path is what
     *               @ref description returns, and its max bytes is the cap)
     * @param clock  where the stamp of a line comes from; empty means the
     *               device's own local time
     */
    ArchiveFileLogTarget(std::unique_ptr<LogFileWriter> writer, Clock clock = Clock());

    bool append(LogLevel level, const std::string& line) override;
    const std::string& description() const override;

    /** @brief The writer itself, for the cap and the counters. */
    LogFileWriter& writer() { return *writer_; }

private:
    std::unique_ptr<LogFileWriter> writer_;
    Clock clock_;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_ARCHIVEFILELOGTARGET_H
