#ifndef DHCP_DHCP_DHCPLOGWRITER_H
#define DHCP_DHCP_DHCPLOGWRITER_H

#include <cstddef>
#include <string>

#include "../core/LogFileWriter.h"

namespace dhcp {
namespace dhcp {

/**
 * @brief The DHCP log as a file: `/fat/logs/Dhcp.log` (stage 212).
 *
 * What the DHCP server says on the terminal, kept where the operator can read it
 * without a serial console and next to the other logs in the file explorer: every
 * packet it answered, every address it handed out, every client it refused and the
 * probes that decided it. The policy is @ref core::LogFileWriter's, as it is for the
 * other five logs: a full file is copied to the card as
 * `log/Archive/Dhcp/<first>-<last>/Dhcp.log`, and with no card the newest ten lines
 * are what remains.
 *
 * A failed archive names itself `DHCP log` in the line it leaves in `Errors.log` —
 * which is the file the operator reads when something is wrong, and is not the same
 * as this one.
 *
 * The line shape (`[2026-10-16 21:30:00] dhcp: …`) belongs to `DhcpFileLog`, which
 * owns the timestamp; this class only decides where the file lives and what it is
 * called when it cannot be archived.
 */
class DhcpLogWriter : public core::LogFileWriter {
public:
    /** @brief The live file, the card to archive to, and the two questions. */
    DhcpLogWriter(std::string livePath, std::string cardMountPoint,
                  CardCheck cardMounted, Notice notice,
                  size_t maxBytes = core::kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DHCPLOGWRITER_H
