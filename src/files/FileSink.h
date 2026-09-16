#ifndef DHCP_FILES_FILESINK_H
#define DHCP_FILES_FILESINK_H

#include <cstdint>
#include <cstdio>
#include <string>

#include "IFileSink.h"

namespace dhcp {
namespace files {

/**
 * @brief @ref IFileSink writing to `<destination>.part` until @ref commit.
 *
 * Used for uploads; see the interface for the atomicity contract. The
 * temporary name is the destination plus the `.part` suffix, which also makes
 * an interrupted upload (power loss, reboot) obvious in the explorer instead
 * of looking like a complete file.
 */
class FileSink : public IFileSink {
public:
    /** @param[in] finalPath Absolute VFS path of the destination file. */
    explicit FileSink(std::string finalPath);
    ~FileSink() override;

    /** @brief True when the temporary file was created successfully. */
    bool isOpen() const { return file_ != nullptr; }

    // IFileSink
    bool write(const uint8_t* data, size_t len) override;
    uint64_t written() const override { return written_; }
    bool commit() override;
    void abort() override;

private:
    /** @brief Close the temporary file without deleting it. */
    void closeFile();

    std::string finalPath_;
    std::string partPath_;
    std::FILE* file_ = nullptr;
    uint64_t written_ = 0;
    bool committed_ = false;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_FILESINK_H
