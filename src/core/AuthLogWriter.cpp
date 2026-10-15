#include "AuthLogWriter.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

/** @brief Folder of this log's archives on the card (rule 39: named once). */
constexpr const char* kAuthLogArchiveFolder = "log/Archive/Auth";

} // namespace

AuthLogWriter::AuthLogWriter(string livePath, string cardMountPoint,
                             CardCheck cardMounted, Notice notice, size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* AuthLogWriter::archiveFolder() const
{
    return kAuthLogArchiveFolder;
}

string AuthLogWriter::logTitle() const
{
    return "Auth log";
}

} // namespace core
} // namespace dhcp
