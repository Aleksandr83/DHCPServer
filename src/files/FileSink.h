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
 * an interrupted upload (power loss, reboot) obvious instead of looking like a
 * complete file — and it is reserved, so `PathUtil` refuses to create such a
 * name through the API and the explorer does not list it.
 */
class FileSink : public IFileSink {
public:
    /**
     * @param[in] finalPath   Absolute VFS path of the destination file.
     * @param[in] initialSize Bytes already in `<finalPath>.part` that this sink
     *                        continues (0 = start a new temporary file). The
     *                        caller has checked that the number matches the file
     *                        on the device (see `FileManager::openWrite`).
     */
    explicit FileSink(std::string finalPath, uint64_t initialSize = 0);
    ~FileSink() override;

    /** @brief True when the temporary file was created successfully. */
    bool isOpen() const { return file_ != nullptr; }

    // IFileSink
    bool write(const uint8_t* data, size_t len) override;
    uint64_t written() const override { return written_; }
    bool commit() override;
    void abort() override;
    void keep() override;

private:
    /** @brief Close the temporary file without deleting it. */
    void closeFile();

    std::string finalPath_;
    std::string partPath_;
    std::FILE* file_ = nullptr;
    uint64_t written_ = 0;
    bool committed_ = false;
    bool kept_ = false;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_FILESINK_H
