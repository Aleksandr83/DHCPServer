#ifndef DHCP_CORE_SSHLOGWRITER_H
#define DHCP_CORE_SSHLOGWRITER_H

#include <cstddef>
#include <string>

#include "LogFileWriter.h"

namespace dhcp {
namespace core {

/**
 * @brief The SSH journal as a file: `/fat/logs/SSH.log` (stage 223).
 *
 * Who opened a session and from where, how long it lasted, who was refused and
 * why — kept where the operator can read it without a serial console. The policy
 * is @ref LogFileWriter's, like every other file log here: a full file is copied
 * to the card as `log/Archive/Ssh/<first>-<last>/SSH.log`, and with no card the
 * newest ten lines are what remains.
 *
 * A failed archive says so in `Errors.log` under its own name, because that is the
 * file read when something is wrong — this one is the record of sessions, not of
 * faults.
 */
class SshLogWriter : public LogFileWriter {
public:
    SshLogWriter(std::string livePath, std::string cardMountPoint,
                 CardCheck cardMounted, Notice notice,
                 size_t maxBytes = kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SSHLOGWRITER_H
