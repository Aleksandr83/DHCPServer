#ifndef DHCP_CORE_ERRORSLOGWRITER_H
#define DHCP_CORE_ERRORSLOGWRITER_H

#include <cstddef>
#include <string>

#include "LogFileWriter.h"

namespace dhcp {
namespace core {

/**
 * @brief The device's error log as a file: `/fat/logs/Errors.log` (stage 206).
 *
 * The file the operator reads when something is wrong, and the file every other
 * log writes its own failures into. The policy - the cap, the rotation, the copy
 * to the card, the tail carried when there is no card, the ten attempts - is
 * @ref LogFileWriter's; this class answers the two questions that make it *this*
 * log:
 *
 *   * a full file is archived to `log/Archive/Errors/<first>-<last>/Errors.log` on
 *     the card;
 *   * a failed archive names itself `Errors log` in the line it leaves — in this
 *     very file, which is where the operator already looks (the one log that
 *     cannot report elsewhere without a second file to read).
 *
 * The lines themselves come from `ErrorLogCore`, and their shape has not changed.
 */
class ErrorsLogWriter : public LogFileWriter {
public:
    /** @brief The live file, the card to archive to, and the two questions. */
    ErrorsLogWriter(std::string livePath, std::string cardMountPoint,
                    CardCheck cardMounted, Notice notice,
                    size_t maxBytes = kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_ERRORSLOGWRITER_H
