#include "DhcpLogWriter.h"

using namespace std;

namespace dhcp {
namespace dhcp {

namespace {

/** @brief Folder of this log's archives on the card (rule 39: named once). */
constexpr const char* kDhcpLogArchiveFolder = "log/Archive/Dhcp";

} // namespace

DhcpLogWriter::DhcpLogWriter(string livePath, string cardMountPoint,
                             CardCheck cardMounted, Notice notice, size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* DhcpLogWriter::archiveFolder() const
{
    return kDhcpLogArchiveFolder;
}

string DhcpLogWriter::logTitle() const
{
    return "DHCP log";
}

} // namespace dhcp
} // namespace dhcp
