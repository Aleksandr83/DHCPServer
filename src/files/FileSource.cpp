#include "FileSource.h"

#include <sys/stat.h>

#include "esp_log.h"

using namespace std;

namespace dhcp {
namespace files {

namespace {
const char* TAG = "FileSource";
}

FileSource::FileSource(string path)
    : path_(move(path))
{
    struct stat st = {};
    if (stat(path_.c_str(), &st) == 0) {
        size_ = static_cast<uint64_t>(st.st_size);
    }

    file_ = fopen(path_.c_str(), "rb");
    if (file_ == nullptr) {
        error_ = true;
        ESP_LOGE(TAG, "cannot open %s for reading", path_.c_str());
    }
}

FileSource::~FileSource()
{
    if (file_ != nullptr) fclose(file_);
}

size_t FileSource::read(uint8_t* buf, size_t maxLen)
{
    if (file_ == nullptr || maxLen == 0) return 0;

    const size_t n = fread(buf, 1, maxLen, file_);
    if (n == 0 && ferror(file_) != 0) {
        error_ = true;
        ESP_LOGE(TAG, "read failed for %s", path_.c_str());
    }
    return n;
}

} // namespace files
} // namespace dhcp
