#ifndef DHCP_FILES_IFILESOURCE_H
#define DHCP_FILES_IFILESOURCE_H

#include <cstddef>
#include <cstdint>

namespace dhcp {
namespace files {

/**
 * @brief Byte source for a streamed download (see `IFileManager::openRead`).
 *
 * Deliberately pull-based (the caller owns the buffer): the REST layer can
 * hand its shared PSRAM transfer window in, so a download never needs a buffer
 * sized like the file.
 */
class IFileSource {
public:
    virtual ~IFileSource() = default;

    /** @brief Total size in bytes (0 for an empty file). */
    virtual uint64_t size() const = 0;

    /**
     * @brief Read up to @p maxLen bytes into @p buf.
     * @return Bytes read; `0` means end-of-file **or** an error — check
     *         @ref error to tell the two apart.
     */
    virtual size_t read(uint8_t* buf, size_t maxLen) = 0;

    /** @brief True when the last @ref read failed (as opposed to EOF). */
    virtual bool error() const = 0;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_IFILESOURCE_H
