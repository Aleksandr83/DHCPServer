#include "NtpLogWriter.h"

using namespace std;

namespace dhcp {
namespace core {

NtpLogWriter::NtpLogWriter(string livePath, string cardMountPoint,
                           CardCheck cardMounted, Notice notice, size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* NtpLogWriter::archiveFolder() const
{
    return kNtpLogArchiveFolder;
}

string NtpLogWriter::logTitle() const
{
    return "NTP log";
}

} // namespace core
} // namespace dhcp
