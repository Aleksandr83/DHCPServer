#ifndef DHCP_CORE_NTPLOGWRITER_H
#define DHCP_CORE_NTPLOGWRITER_H

#include <string>

#include "LogFileWriter.h"
#include "NtpLogFormat.h"

namespace dhcp {
namespace core {

/**
 * @brief The NTP request log as a file: `/fat/logs/Ntp.log` (stage 191).
 *
 * The writing, the cap, the rotation and the carried tail are the operator's
 * policy and live in @ref LogFileWriter — one copy of them, shared with the DNS
 * forwarded log (stage 203). What this class adds is the two answers that make it
 * the *NTP* log and not some other file:
 *
 *   * a full file is archived to `log/Archive/Ntp/<first>-<last>/Ntp.log` on the
 *     card;
 *   * a failed archive names itself `NTP log` in the line it leaves in
 *     `Errors.log`.
 *
 * The lines themselves are built in `NtpLogFormat.h`, by the caller: this class
 * writes what it is handed.
 */
class NtpLogWriter : public LogFileWriter {
public:
    /**
     * @brief The live file, the card to archive to, and the two questions.
     *
     * The arguments are @ref LogFileWriter's: this class adds nothing to them and
     * only names itself.
     */
    NtpLogWriter(std::string livePath, std::string cardMountPoint,
                 CardCheck cardMounted, Notice notice,
                 size_t maxBytes = kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_NTPLOGWRITER_H
