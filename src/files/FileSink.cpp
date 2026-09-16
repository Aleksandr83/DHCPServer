#include "FileSink.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "esp_log.h"

namespace dhcp {
namespace files {

namespace {
const char* TAG = "FileSink";
}

FileSink::FileSink(std::string finalPath, uint64_t initialSize)
    : finalPath_(std::move(finalPath))
    , partPath_(finalPath_ + ".part")
    , written_(initialSize)
{
    // Appending is what a continued upload needs: the sink is opened with the
    // size the caller checked against the file, so "ab" writes exactly where the
    // previous chunk ended. A fresh upload truncates whatever was left behind.
    const char* mode = (initialSize > 0) ? "ab" : "wb";
    file_ = fopen(partPath_.c_str(), mode);
    if (file_ == nullptr) {
        ESP_LOGE(TAG, "cannot open %s (%s)", partPath_.c_str(), mode);
    }
}

FileSink::~FileSink()
{
    // Safety net: an object destroyed without commit() must not leave a
    // half-written file behind (the destination may not exist yet, but the
    // .part file must go) — unless the upload was explicitly kept for a later
    // continuation.
    if (!committed_ && !kept_) abort();
}

bool FileSink::write(const uint8_t* data, size_t len)
{
    if (file_ == nullptr || len == 0) return file_ != nullptr;

    if (fwrite(data, 1, len, file_) != len) {
        ESP_LOGE(TAG, "write failed at %llu bytes (%s)",
                 (unsigned long long)written_, partPath_.c_str());
        return false;
    }
    written_ += len;
    return true;
}

void FileSink::closeFile()
{
    if (file_ == nullptr) return;
    fclose(file_);
    file_ = nullptr;
}

bool FileSink::commit()
{
    if (committed_) return true;
    if (file_ == nullptr) return false;

    // Flush and close before touching the destination: on FAT the flush is
    // where a "disk full" error is reported, so it must be checked.
    if (fflush(file_) != 0 || ferror(file_) != 0) {
        ESP_LOGE(TAG, "flush failed for %s", partPath_.c_str());
        closeFile();
        abort();
        return false;
    }
    closeFile();

    // FatFs' f_rename() refuses an existing destination, so the old file is
    // removed first (the UI already confirmed the replacement).
    if (unlink(finalPath_.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "cannot remove the old %s (%s)", finalPath_.c_str(),
                 strerror(errno));
    }

    if (::rename(partPath_.c_str(), finalPath_.c_str()) != 0) {
        ESP_LOGE(TAG, "cannot publish %s (%s)", finalPath_.c_str(),
                 strerror(errno));
        abort();
        return false;
    }

    committed_ = true;
    ESP_LOGI(TAG, "uploaded %s (%llu bytes)", finalPath_.c_str(),
             (unsigned long long)written_);
    return true;
}

void FileSink::keep()
{
    if (committed_ || kept_) return;

    closeFile();
    kept_ = true;
    ESP_LOGI(TAG, "kept %s (%llu bytes) for a later continuation",
             partPath_.c_str(), (unsigned long long)written_);
}

void FileSink::abort()
{
    closeFile();

    if (!committed_ && unlink(partPath_.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "cannot remove the partial %s (%s)", partPath_.c_str(),
                 strerror(errno));
    }
    written_ = 0;
}

} // namespace files
} // namespace dhcp
