/**
 * @file IpText.h
 * @brief The text of an IP address, in one place (stage 209).
 *
 * Three call sites need to name an address the operator will read — the peer of a
 * connection, and every record the cache view prints — and a second copy of RFC
 * 5952 is how the same IPv6 address ends up spelled two ways on two pages. So the
 * spelling lives here, free of ESP-IDF so a host test can check it (there is no
 * `inet_ntop` on the host, and the board's one is not the thing being tested
 * anyway: the bytes are handed in deliberately).
 *
 * Two orderings are in play and they are not interchangeable: the bytes of an
 * address as a socket or a DNS answer reports them (wire order), and the 32-bit
 * value `core::Subnet` judges (host order). `ip4HostOrder()` is the one conversion
 * between them.
 */
#ifndef DHCP_CORE_IPTEXT_H
#define DHCP_CORE_IPTEXT_H

#include <cstdint>
#include <cstdio>
#include <string>

namespace dhcp {
namespace core {

/** @brief Four wire-order bytes as the host-order value `Subnet` works with. */
inline uint32_t ip4HostOrder(const uint8_t bytes[4])
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

/** @brief Four wire-order bytes as a dotted quad. */
inline std::string ip4Text(const uint8_t bytes[4])
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             static_cast<unsigned>(bytes[0]), static_cast<unsigned>(bytes[1]),
             static_cast<unsigned>(bytes[2]), static_cast<unsigned>(bytes[3]));
    return buf;
}

/**
 * @brief Sixteen wire-order bytes as IPv6 text in the shape RFC 5952 asks for.
 *
 * Lower case, no leading zeros in a group, and the **longest** run of zero groups
 * — two or more, the first one when two runs are equally long — written as `::`.
 * The text is what a browser accepts back as an address, so `::` for a run of one
 * group (which RFC 5952 forbids and a parser may refuse) and a second `::` are
 * both errors this deliberately cannot make.
 */
inline std::string ip6Text(const uint8_t bytes[16])
{
    uint16_t groups[8];
    for (int i = 0; i < 8; ++i) {
        groups[i] = static_cast<uint16_t>((static_cast<uint16_t>(bytes[2 * i]) << 8) |
                                          static_cast<uint16_t>(bytes[2 * i + 1]));
    }

    int bestStart = -1;
    int bestLen = 0;
    int runStart = -1;
    for (int i = 0; i <= 8; ++i) {
        const bool zero = (i < 8) && groups[i] == 0;
        if (zero) {
            if (runStart < 0) runStart = i;
            continue;
        }
        if (runStart >= 0) {
            const int len = i - runStart;
            if (len > bestLen) {
                bestLen = len;
                bestStart = runStart;
            }
            runStart = -1;
        }
    }
    if (bestLen < 2) bestStart = -1;   // one zero group is a zero, not a gap

    std::string out;
    for (int i = 0; i < 8; ++i) {
        if (i == bestStart) {
            out += "::";
            i += bestLen - 1;
            continue;
        }
        // The colon before a group is skipped when the group before it ended the
        // compression, which is what turns `::` and a leading group into one `::`.
        if (!out.empty() && out.back() != ':') out += ':';
        char buf[8];
        snprintf(buf, sizeof(buf), "%x", static_cast<unsigned>(groups[i]));
        out += buf;
    }
    return out;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_IPTEXT_H
