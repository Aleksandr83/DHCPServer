#ifndef DHCP_FILES_IFILESINK_H
#define DHCP_FILES_IFILESINK_H

#include <cstddef>
#include <cstdint>

namespace dhcp {
namespace files {

/**
 * @brief Byte sink for a streamed upload (see `IFileManager::openWrite`).
 *
 * The implementation writes into a temporary file and only publishes it on
 * @ref commit, so an upload interrupted halfway (client disconnect, out of
 * space, HTTP error) can never leave a truncated file in place under the
 * destination name; @ref abort discards the temporary file.
 *
 * The REST layer stays transport-only: it reads the HTTP body in windows and
 * pushes them here, it never opens the filesystem itself.
 */
class IFileSink {
public:
    virtual ~IFileSink() = default;

    /**
     * @brief Append @p len bytes.
     * @return false on a filesystem error (the caller must @ref abort).
     */
    virtual bool write(const uint8_t* data, size_t len) = 0;

    /** @brief Bytes accepted so far (progress/logging). */
    virtual uint64_t written() const = 0;

    /**
     * @brief Publish the uploaded file.
     *
     * Flushes and closes the temporary file, replaces the destination (an
     * existing file with that name is overwritten — the UI asks for
     * confirmation before the upload starts) and keeps the temporary name
     * invisible to the explorer until this point.
     *
     * @return false when the file could not be published.
     */
    virtual bool commit() = 0;

    /** @brief Discard the upload (removes the temporary file). */
    virtual void abort() = 0;
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_IFILESINK_H
