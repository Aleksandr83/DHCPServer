#ifndef DHCP_CORE_AUTHLOGWRITER_H
#define DHCP_CORE_AUTHLOGWRITER_H

#include <cstddef>
#include <string>

#include "LogFileWriter.h"

namespace dhcp {
namespace core {

/**
 * @brief The authorisation log as a file: `/fat/logs/Auth.log` (stage 205).
 *
 * Who tried to log in, who was refused, who was locked out and who got in after
 * failing — kept where the operator can read it without a serial console, and next
 * to the other logs in the file explorer. The policy is @ref LogFileWriter's, as
 * it is for the other four logs: a full file is copied to the card as
 * `log/Archive/Auth/<first>-<last>/Auth.log`, and with no card the newest ten lines
 * are what remains.
 *
 * A failed archive names itself `Auth log` in the line it leaves in `Errors.log` —
 * which is the file the operator reads when something is wrong, and is not the same
 * as this one.
 *
 * The sentences come from `AuthLogFormat.h`; the timestamp is added by
 * `ErrorLogCore`, the same component the journal uses.
 */
class AuthLogWriter : public LogFileWriter {
public:
    /** @brief The live file, the card to archive to, and the two questions. */
    AuthLogWriter(std::string livePath, std::string cardMountPoint,
                  CardCheck cardMounted, Notice notice,
                  size_t maxBytes = kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTHLOGWRITER_H
