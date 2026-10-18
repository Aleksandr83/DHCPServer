#include "SshLogWriter.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

/** @brief Folder of this log's archives on the card (rule 39: named once). */
constexpr const char* kSshLogArchiveFolder = "log/Archive/Ssh";

} // namespace

SshLogWriter::SshLogWriter(string livePath, string cardMountPoint,
                           CardCheck cardMounted, Notice notice, size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* SshLogWriter::archiveFolder() const
{
    return kSshLogArchiveFolder;
}

string SshLogWriter::logTitle() const
{
    return "SSH log";
}

} // namespace core
} // namespace dhcp
