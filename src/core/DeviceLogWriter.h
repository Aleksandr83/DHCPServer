#ifndef DHCP_CORE_DEVICELOGWRITER_H
#define DHCP_CORE_DEVICELOGWRITER_H

#include <cstddef>
#include <string>

#include "LogFileWriter.h"

namespace dhcp {
namespace core {

/**
 * @brief The device events journal as a file: `/fat/logs/Device.log`.
 *
 * What the box itself did — started with which version, why the previous run
 * ended, who asked for a restart, when the settings were replaced or erased —
 * kept where the operator can read it without a serial console. The policy is
 * @ref LogFileWriter's, like every other file log here: a full file is copied to
 * the card as `log/Archive/Device/<first>-<last>/Device.log`, and with no card
 * the newest ten lines are what remains.
 *
 * A failed archive says so in `Errors.log` under its own name: this file is the
 * record of what happened, not of what went wrong.
 */
class DeviceLogWriter : public LogFileWriter {
public:
    DeviceLogWriter(std::string livePath, std::string cardMountPoint,
                    CardCheck cardMounted, Notice notice,
                    size_t maxBytes = kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_DEVICELOGWRITER_H
