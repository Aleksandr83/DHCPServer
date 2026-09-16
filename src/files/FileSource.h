#ifndef DHCP_FILES_FILESOURCE_H
#define DHCP_FILES_FILESOURCE_H

#include <cstdint>
#include <cstdio>
#include <string>

#include "IFileSource.h"

namespace dhcp {
namespace files {

/**
 * @brief @ref IFileSource reading a file from a FAT volume.
 *
 * Thin `stdio` wrapper: it only reports the size and hands out byte windows,
 * so a download of a 21 MB file needs no buffer of that size. The file is
 * closed in the destructor, which makes the lifetime of the stream — and of
 * the HTTP response that uses it — identical.
 */
class FileSource : public IFileSource {
public:
    /** @param[in] path Absolute VFS path of the file to read. */
    explicit FileSource(std::string path);
    ~FileSource() override;

    /** @brief True when the file was opened successfully. */
    bool isOpen() const { return file_ != nullptr; }

    // IFileSource
    uint64_t size() const override { return size_; }
    size_t read(uint8_t* buf, size_t maxLen) override;
    bool error() const override { return error_; }

private:
    std::string path_;
    std::FILE* file_ = nullptr;
    uint64_t size_ = 0;
    bool error_ = false;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_FILESOURCE_H
