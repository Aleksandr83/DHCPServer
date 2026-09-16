#ifndef DHCP_WEB_MULTIPARTEXTRACTOR_H
#define DHCP_WEB_MULTIPARTEXTRACTOR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace dhcp {
namespace web {

/**
 * @brief Streaming extractor for the payload of a `multipart/form-data` part.
 *
 * The HTTP body of an upload is an envelope: `--<boundary>\r\n`, the part
 * headers, a blank line, the payload, and finally `\r\n--<boundary>--` plus an
 * optional epilogue. Whoever needs the file (or, for OTA, the image) must hand
 * over **only** the payload — writing the envelope into the OTA partition
 * produces an image without the `0xE9` magic, which `esp_ota_end()` rejects.
 *
 * Feed @ref feed with the body bytes in any chunking: the class keeps the
 * state (part headers may span calls, and the closing delimiter is searched
 * across call boundaries thanks to a small carry-over buffer). Payload bytes
 * are delivered to the sink, everything else is dropped.
 *
 * Deliberately free of ESP-IDF dependencies so the logic — which silently
 * corrupted an OTA upload once — is unit-tested on the host
 * (`test/test_multipart.cpp`), like `core::Subnet` and `storage::PathUtil`.
 *
 * Binary safety: bytes are copied verbatim, no NUL termination or text
 * interpretation is involved, so an image payload is handled as-is.
 */
class MultipartExtractor {
public:
    /** Maximum length of the part headers (guards against a hostile body). */
    static constexpr size_t kMaxHeaderBytes = 8192;

    /**
     * @brief Payload sink.
     * @return false to abort the transfer (the caller's write failed).
     */
    using Sink = std::function<bool(const uint8_t* data, size_t len)>;

    /**
     * @param[in] boundary Boundary value from the `Content-Type` header
     *                     (without the leading `--`). An empty boundary marks
     *                     the extractor as failed — the caller must reject the
     *                     request instead of guessing what the body is.
     * @param[in] sink     Receiver of the payload bytes.
     */
    MultipartExtractor(std::string boundary, Sink sink);

    /**
     * @brief Feed the next chunk of the HTTP body.
     * @return false on a malformed body or when the sink refused the data.
     */
    bool feed(const uint8_t* data, size_t len);

    /**
     * @brief Validate that the body ended properly (closing delimiter seen).
     *
     * Call once after the last @ref feed. A body that ends in the middle of the
     * payload is reported as incomplete: appending the leftovers to the image
     * would corrupt it, so the caller must fail the transfer.
     *
     * @return true when the part was extracted completely.
     */
    bool finish();

    /** @brief True when the closing delimiter has been seen. */
    bool finished() const { return dataDone_; }

    /** @brief True on a malformed body (missing headers, bad sink, …). */
    bool failed() const { return failed_; }

    /** @brief Payload bytes handed to the sink so far. */
    uint64_t payloadBytes() const { return payloadBytes_; }

private:
    /** @brief Search the carry-over buffer for the delimiter and forward data. */
    bool consume(const char* data, size_t len);

    std::string delimiter_;    ///< `\r\n--<boundary>`
    std::string headerAcc_;    ///< Part headers until `\r\n\r\n` (multipart only)
    std::string pending_;      ///< Bytes not yet written (delimiter may start here)
    Sink sink_;
    uint64_t payloadBytes_ = 0;
    bool dataStarted_ = false;
    bool dataDone_ = false;
    bool failed_ = false;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_MULTIPARTEXTRACTOR_H
