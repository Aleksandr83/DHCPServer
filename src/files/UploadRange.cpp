#include "UploadRange.h"

namespace dhcp {
namespace files {

std::string UploadRange::check(uint64_t partSize, uint64_t offset, uint64_t chunk,
                               bool hasTotal, uint64_t total, UploadRange& out)
{
    UploadRange range;
    range.partSize = partSize;
    range.chunk = chunk;
    range.resumable = hasTotal;

    if (!hasTotal) {
        // The original single-request upload: the body is the whole file and the
        // temporary file starts from zero, so a leftover `.part` with a different
        // size is simply replaced (that is what the client is asking for by not
        // sending `total`). An empty body is a valid, empty file here — that is
        // how the text editor truncates one.
        range.offset = 0;
        range.total = chunk;
        out = range;
        return "";
    }

    // In resumable mode an empty chunk achieves nothing: the client would tell
    // itself it had sent something.
    if (chunk == 0) return "empty body";

    if (total == 0) return "total must be greater than zero";

    if (offset != partSize) {
        return "offset " + std::to_string(offset) +
               " does not match the partial file (" + std::to_string(partSize) +
               " bytes on the device)";
    }

    if (offset >= total) {
        return "offset " + std::to_string(offset) + " is at or past the end of " +
               std::to_string(total) + " bytes";
    }

    range.offset = offset;
    range.total = total;

    if (range.endOffset() > total) {
        return "body of " + std::to_string(chunk) + " bytes at offset " +
               std::to_string(offset) + " runs past the end of " +
               std::to_string(total) + " bytes";
    }

    out = range;
    return "";
}

} // namespace files
} // namespace dhcp
