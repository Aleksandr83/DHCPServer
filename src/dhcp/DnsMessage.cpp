#include "DnsMessage.h"

using namespace std;

namespace dhcp {
namespace dhcp {

bool DnsMessage::readHeader(const uint8_t* buf, size_t len, uint16_t id,
                            uint16_t& qdOut, uint16_t& anOut)
{
    if (!buf || len < kHeaderBytes) return false;

    const uint16_t answerId = static_cast<uint16_t>((buf[kIdOffset] << 8) |
                                                    buf[kIdOffset + 1]);
    if (answerId != id) return false;                 // not our answer
    if ((buf[kFlags1Offset] & kFlagResponse) == 0) return false;   // QR: not a response
    if ((buf[kFlags2Offset] & kMaskRcode) != 0) return false;      // NXDOMAIN/SERVFAIL/…

    qdOut = static_cast<uint16_t>((buf[kQdCountOffset] << 8) |
                                  buf[kQdCountOffset + 1]);
    anOut = static_cast<uint16_t>((buf[kAnCountOffset] << 8) |
                                  buf[kAnCountOffset + 1]);
    return true;
}

size_t DnsMessage::skipName(const uint8_t* buf, size_t len, size_t off)
{
    size_t jumps = 0;
    while (off < len) {
        const uint8_t label = buf[off];
        if (label == 0) return off + 1;
        if ((label & kMaskLabelType) == kLabelPointer) {
            if (off + 1 >= len) return len + 1;
            if (++jumps > kMaxNameJumps) return len + 1;
            // A pointer must point backwards; a forward one means the message is
            // damaged, and skipping a name without checking it would let the
            // record walk start in the middle of something else.
            const size_t target = ((label & kMaskPointerOffset) << 8) | buf[off + 1];
            if (target >= off) return len + 1;
            return off + 2;
        }
        if (label > kMaxLabelBytes || off + 1 + label > len) return len + 1;
        off += 1 + label;
    }
    return len + 1;
}

string DnsMessage::decodeName(const uint8_t* buf, size_t len, size_t off)
{
    string out;
    size_t jumps = 0;
    while (off < len) {
        const uint8_t label = buf[off];
        if (label == 0) break;                       // end of the name
        if ((label & kMaskLabelType) == kLabelPointer) {   // compression pointer
            if (off + 1 >= len) return string();
            if (++jumps > kMaxNameJumps) return string();
            const size_t target = ((label & kMaskPointerOffset) << 8) | buf[off + 1];
            if (target >= off) return string(); // must point backwards
            off = target;
            continue;
        }
        if (label > kMaxLabelBytes || off + 1 + label > len) return string();
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(buf + off + 1), label);
        off += 1 + label;
    }
    return out;
}

} // namespace dhcp
} // namespace dhcp
