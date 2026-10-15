#include "JobsLogWriter.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

/** @brief Folder of this log's archives on the card (rule 39: named once). */
constexpr const char* kJobsLogArchiveFolder = "log/Archive/Jobs";

} // namespace

JobsLogWriter::JobsLogWriter(string livePath, string cardMountPoint,
                             CardCheck cardMounted, Notice notice, size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* JobsLogWriter::archiveFolder() const
{
    return kJobsLogArchiveFolder;
}

string JobsLogWriter::logTitle() const
{
    return "Jobs log";
}

} // namespace core
} // namespace dhcp
