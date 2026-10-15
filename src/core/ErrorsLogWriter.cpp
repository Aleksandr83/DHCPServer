#include "ErrorsLogWriter.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

/** @brief Folder of this log's archives on the card (rule 39: named once). */
constexpr const char* kErrorsLogArchiveFolder = "log/Archive/Errors";

} // namespace

ErrorsLogWriter::ErrorsLogWriter(string livePath, string cardMountPoint,
                                 CardCheck cardMounted, Notice notice, size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* ErrorsLogWriter::archiveFolder() const
{
    return kErrorsLogArchiveFolder;
}

string ErrorsLogWriter::logTitle() const
{
    return "Errors log";
}

} // namespace core
} // namespace dhcp
