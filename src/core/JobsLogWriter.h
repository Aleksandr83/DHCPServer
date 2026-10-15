#ifndef DHCP_CORE_JOBSLOGWRITER_H
#define DHCP_CORE_JOBSLOGWRITER_H

#include <cstddef>
#include <string>

#include "LogFileWriter.h"

namespace dhcp {
namespace core {

/**
 * @brief The long-running-operations journal as a file: `/fat/logs/Jobs.log`
 *        (stage 206).
 *
 * "What has the device been doing": one line when an operation starts and one when
 * it ends, with its result and how long it took. Where the error log answers "what
 * went wrong", this file answers "what happened", and since stage 206 a full one
 * is treated like every other file log in the project: copied to the card as
 * `log/Archive/Jobs/<first>-<last>/Jobs.log`, and kept with its newest ten lines
 * when there is no card to copy to. The lines themselves are built by `JobLog`
 * (`[2026-10-04 02:06:43] job: …`), which is why this class has nothing to say
 * about their shape.
 *
 * The policy is @ref LogFileWriter's; what this class adds is the two answers that
 * make it the journal: the archive folder above, and the name `Jobs log` in the
 * line a failed archive leaves in `Errors.log`.
 */
class JobsLogWriter : public LogFileWriter {
public:
    /** @brief The live file, the card to archive to, and the two questions. */
    JobsLogWriter(std::string livePath, std::string cardMountPoint,
                  CardCheck cardMounted, Notice notice,
                  size_t maxBytes = kLogFileDefaultMaxBytes);

protected:
    const char* archiveFolder() const override;
    std::string logTitle() const override;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_JOBSLOGWRITER_H
