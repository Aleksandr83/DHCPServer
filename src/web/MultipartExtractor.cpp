#include "MultipartExtractor.h"

#include <utility>

namespace dhcp {
namespace web {

MultipartExtractor::MultipartExtractor(std::string boundary, Sink sink)
    : delimiter_("\r\n--" + boundary)
    , sink_(std::move(sink))
{
    // An empty boundary would make the delimiter just CRLF CRLF and would eat
    // every blank line inside the payload — refuse to guess instead. (Checked
    // AFTER building the delimiter: moving `boundary` first would leave it
    // empty and mark every extractor as failed.)
    if (boundary.empty()) failed_ = true;
}

bool MultipartExtractor::feed(const uint8_t* data, size_t len)
{
    if (failed_ || dataDone_) return !failed_;
    if (data == nullptr || len == 0) return true;

    if (!dataStarted_) {
        // Part headers: `--boundary\r\n` + headers + `\r\n\r\n`.
        headerAcc_.append(reinterpret_cast<const char*>(data), len);

        const size_t end = headerAcc_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (headerAcc_.size() > kMaxHeaderBytes) {
                failed_ = true;
                return false;
            }
            return true;   // headers not complete yet
        }

        dataStarted_ = true;
        const std::string payload = headerAcc_.substr(end + 4);
        headerAcc_.clear();

        if (!payload.empty()) {
            return consume(payload.data(), payload.size());
        }
        return true;
    }

    return consume(reinterpret_cast<const char*>(data), len);
}

bool MultipartExtractor::consume(const char* data, size_t len)
{
    pending_.append(data, len);

    const size_t pos = pending_.find(delimiter_);
    if (pos != std::string::npos) {
        // Everything before the delimiter is payload; the delimiter itself, a
        // possible `--` marker and the epilogue are envelope.
        if (pos > 0) {
            if (sink_ && !sink_(reinterpret_cast<const uint8_t*>(pending_.data()), pos)) {
                failed_ = true;
                return false;
            }
            payloadBytes_ += pos;
        }
        pending_.clear();
        dataDone_ = true;
        return true;
    }

    // No delimiter: hand over everything except the longest possible prefix of
    // the delimiter, which may only complete once the next chunk arrives.
    const size_t keep = delimiter_.size() - 1;
    if (pending_.size() > keep) {
        const size_t take = pending_.size() - keep;
        if (sink_ && !sink_(reinterpret_cast<const uint8_t*>(pending_.data()), take)) {
            failed_ = true;
            return false;
        }
        payloadBytes_ += take;
        pending_.erase(0, take);
    }
    return true;
}

bool MultipartExtractor::finish()
{
    if (failed_) return false;

    // A body that ends while bytes are still held back was cut in the middle of
    // the payload (or the closing delimiter is missing): the leftover could be
    // part of the image, but it could just as well be envelope, so the transfer
    // is reported as incomplete rather than writing a possibly corrupt file.
    return dataDone_;
}

} // namespace web
} // namespace dhcp
