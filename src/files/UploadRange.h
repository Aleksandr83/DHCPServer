#ifndef DHCP_FILES_UPLOADRANGE_H
#define DHCP_FILES_UPLOADRANGE_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace files {

/**
 * @brief Numbers of one upload chunk: where it starts and where the file ends.
 *
 * The upload endpoint takes a file in pieces, so a transfer can be paused and
 * continued (`?offset=N&total=M`) instead of starting over. Everything that
 * follows from those numbers is arithmetic — a chunk that does not line up with
 * the temporary file already on the device, a body that runs past the end of the
 * file, the bytes the volume still has to accept, whether this chunk completes
 * the file — and it lives here, away from the HTTP layer and the filesystem, so
 * the host tests cover it without a device (`test/test_uploadrange.cpp`).
 *
 * A request without `total` keeps the original meaning: one request carries the
 * whole file and replaces whatever is on the device.
 */
struct UploadRange {
    /** @brief Bytes already stored in `<name>.part` on the device. */
    uint64_t partSize = 0;
    /** @brief Where this chunk starts writing. */
    uint64_t offset = 0;
    /** @brief Bytes in this chunk (`Content-Length`). */
    uint64_t chunk = 0;
    /** @brief Size of the finished file. */
    uint64_t total = 0;
    /** @brief True when the client sent `total`, i.e. asked to be able to resume. */
    bool resumable = false;

    /** @brief Bytes on the device once this chunk is written. */
    uint64_t endOffset() const { return offset + chunk; }

    /** @brief True when this chunk leaves a complete file behind. */
    bool completes() const { return endOffset() >= total; }

    /** @brief Free space the volume must have for this chunk. */
    uint64_t neededBytes() const { return total > offset ? total - offset : 0; }

    /**
     * @brief Check the numbers of a request against the state on the device.
     *
     * @param[in]  partSize Bytes currently in `<name>.part` (0 when absent).
     * @param[in]  offset   `offset` parameter of the request.
     * @param[in]  chunk    `Content-Length` of the request body.
     * @param[in]  hasTotal True when the request carried `total`.
     * @param[in]  total    `total` parameter (ignored when @p hasTotal is false).
     * @param[out] out      Filled in when the check passes; untouched otherwise.
     *
     * @return "" when the chunk may be written, otherwise the reason for
     *         refusing it (English, like the other `detail` strings the REST
     *         layer sends next to a status code).
     */
    static std::string check(uint64_t partSize, uint64_t offset, uint64_t chunk,
                             bool hasTotal, uint64_t total, UploadRange& out);
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_UPLOADRANGE_H
