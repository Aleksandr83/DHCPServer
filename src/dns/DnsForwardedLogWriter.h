#ifndef DHCP_DNS_DNSFORWARDEDLOGWRITER_H
#define DHCP_DNS_DNSFORWARDEDLOGWRITER_H

#include <cstddef>
#include <string>

#include "DnsLogFormat.h"
#include "core/LogFileWriter.h"

namespace dhcp {
namespace dns {

/**
 * @brief The DNS server's file log: `/fat/logs/Forwarded.log` (stage 204).
 *
 * The whole policy - the cap, the rotation, the archive copy, the tail carried
 * when there is no card, the ten attempts and the line in `Errors.log` - is
 * `core::LogFileWriter`'s, and it is shared with the NTP request log; this class
 * answers the two questions that make it *this* log:
 *
 *   * a full file is archived to `log/Archive/Dns/<first>-<last>/Forwarded.log`
 *     on the card;
 *   * a failed archive names itself `DNS forwarded log` in `Errors.log`.
 *
 * The lines are built by `DnsLogFormat.h` and handed in, so this class knows
 * nothing about DNS records - it writes what it is given wherever the operator
 * asked for it.
 */
class DnsForwardedLogWriter : public core::LogFileWriter {
public:
    /** @brief The live file, the card to archive to, and the two questions. */
    DnsForwardedLogWriter(std::string livePath, std::string cardMountPoint,
                          CardCheck cardMounted, Notice notice,
                          size_t maxBytes = core::kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSFORWARDEDLOGWRITER_H
