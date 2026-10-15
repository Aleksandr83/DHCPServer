#include "ArchiveFileLogTarget.h"

#include <ctime>

#include "ErrorLogCore.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

/**
 * @brief When the line being written happened, as the device's local time.
 *
 * The same reading the NTP and DNS file logs take: the C library is pointed at the
 * operator's zone when the time settings are applied, and a clock that was never
 * set is reported as such instead of printing a date that never happened.
 */
LogStamp deviceStamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    const bool clockSet = ErrorLogCore::clockIsSet(static_cast<uint64_t>(now)) &&
                          ErrorLogCore::localTime(now, local);
    return logStampFromLocal(local, clockSet);
}

} // namespace

ArchiveFileLogTarget::ArchiveFileLogTarget(unique_ptr<LogFileWriter> writer, Clock clock)
    : writer_(move(writer))
    , clock_(move(clock))
{
}

bool ArchiveFileLogTarget::append(LogLevel /*level*/, const string& line)
{
    if (!writer_) return false;

    // The level is not used: the line arrives already formatted (the error log's
    // own shape, or the journal's) and nothing in a file log depends on it. It is
    // kept in the signature because `IErrorLogTarget` carries it.
    const LogStamp stamp = clock_ ? clock_() : deviceStamp();
    return writer_->append(line, stamp);
}

const string& ArchiveFileLogTarget::description() const
{
    return writer_->path();
}

} // namespace core
} // namespace dhcp
