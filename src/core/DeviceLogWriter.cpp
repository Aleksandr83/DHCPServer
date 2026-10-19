#include "DeviceLogWriter.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

/** @brief Folder of this log's archives on the card (rule 39: named once). */
constexpr const char* kDeviceLogArchiveFolder = "log/Archive/Device";

} // namespace

DeviceLogWriter::DeviceLogWriter(string livePath, string cardMountPoint,
                                 CardCheck cardMounted, Notice notice,
                                 size_t maxBytes)
    : LogFileWriter(move(livePath), move(cardMountPoint), move(cardMounted),
                    move(notice), maxBytes)
{
}

const char* DeviceLogWriter::archiveFolder() const
{
    return kDeviceLogArchiveFolder;
}

string DeviceLogWriter::logTitle() const
{
    return "Device log";
}

} // namespace core
} // namespace dhcp
