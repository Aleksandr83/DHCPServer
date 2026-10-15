#ifndef DHCP_CORE_IERRORLOGTARGET_H
#define DHCP_CORE_IERRORLOGTARGET_H

#include <string>

namespace dhcp {
namespace core {

/**
 * @brief Severity of a logged line.
 *
 * `Error` and `Warn` belong to the error log: something went wrong, or the log
 * itself has something to admit (a full queue, a lost line). `Info` is what the
 * job log (stage 173) marks its own lines with — a long-running operation
 * starting or finishing is news, not a fault, and calling it an error would make
 * the file harder to read than it has to be.
 */
enum class LogLevel {
    Error,
    Warn,
    Info,
};

/**
 * @brief Where the error log lines end up.
 *
 * A seam on purpose: the log itself formats a line and hands it over, and does not
 * know what is on the other side. Today that is `ArchiveFileLogTarget` over a
 * `LogFileWriter` — a file on the internal volume whose full generations are
 * copied to the card (stage 206) — while the host tests hand the same core an
 * in-memory list.
 */
class IErrorLogTarget {
public:
    virtual ~IErrorLogTarget() = default;

    /**
     * @brief Append one already formatted line (no trailing newline).
     * @return false when the line could not be stored (volume gone, no space).
     *         The caller counts that as a drop rather than retrying forever.
     */
    virtual bool append(LogLevel level, const std::string& line) = 0;

    /** @brief Human-readable target ("/fat/logs/Errors.log"), for the log itself. */
    virtual const std::string& description() const = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_IERRORLOGTARGET_H
