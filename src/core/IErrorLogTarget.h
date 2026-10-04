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
 * A seam on purpose. Today it is a file on the internal FAT
 * (`/fat/logs/Errors.log`); the plan is that the operator picks the volume in
 * the settings later (the microSD card is the obvious candidate), and the host
 * tests use an in-memory list. The log itself knows none of that: it formats a
 * line and hands it over.
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
