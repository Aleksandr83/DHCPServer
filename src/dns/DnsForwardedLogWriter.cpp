#include "DnsForwardedLogWriter.h"

using namespace std;

namespace dhcp {
namespace dns {

DnsForwardedLogWriter::DnsForwardedLogWriter(string livePath, string cardMountPoint,
                                             CardCheck cardMounted, Notice notice,
                                             size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* DnsForwardedLogWriter::archiveFolder() const
{
    return kDnsForwardedLogArchiveFolder;
}

string DnsForwardedLogWriter::logTitle() const
{
    return "DNS forwarded log";
}

} // namespace dns
} // namespace dhcp
