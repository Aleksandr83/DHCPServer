#ifndef DHCP_TIME_NTPMESSAGE_H
#define DHCP_TIME_NTPMESSAGE_H

#include <cstdint>
#include <cstring>

namespace dhcp {
namespace time {

/**
 * @brief NTPv4 packet (RFC 5905) — 48 bytes, all multi-byte fields big-endian.
 */
#pragma pack(push, 1)
struct NtpPacket {
    uint8_t  liVnMode;       // LI (2 bits) | VN (3 bits) | Mode (3 bits)
    uint8_t  stratum;
    int8_t   poll;
    int8_t   precision;
    uint32_t rootDelay;      // 16.16 fixed point
    uint32_t rootDispersion; // 16.16 fixed point
    uint32_t refId;
    uint32_t refTsSec;       // Reference Timestamp
    uint32_t refTsFrac;
    uint32_t origTsSec;      // Originate Timestamp
    uint32_t origTsFrac;
    uint32_t recvTsSec;      // Receive Timestamp
    uint32_t recvTsFrac;
    uint32_t txTsSec;        // Transmit Timestamp
    uint32_t txTsFrac;
};
#pragma pack(pop)

static_assert(sizeof(NtpPacket) == 48, "NTP packet must be exactly 48 bytes");

// ─── Constants ──────────────────────────────────────
// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
constexpr uint32_t kNtpUnixOffset = 2208988800UL;

// NTP mode values (RFC 5905, section 7.3).
constexpr uint8_t kNtpModeSymmetricActive = 1;
constexpr uint8_t kNtpModeClient          = 3;
constexpr uint8_t kNtpModeServer          = 4;

// ─── Byte order (NTP is always network/big-endian) ──
inline uint32_t swap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

/** @brief Host → NTP (big-endian) 32-bit value. */
inline uint32_t toBe32(uint32_t v)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return swap32(v);
#else
    return v;
#endif
}

/** @brief NTP (big-endian) → host 32-bit value. */
inline uint32_t fromBe32(uint32_t v)
{
    return toBe32(v);
}

/**
 * @brief Convert a Unix timestamp (seconds + microseconds) to NTP seconds + fraction.
 *
 * The fraction is a 32-bit binary fraction of a second (1/2^32 s per LSB).
 */
inline void unixToNtp(uint32_t unixSec, uint32_t unixUsec,
                      uint32_t& ntpSec, uint32_t& ntpFrac)
{
    ntpSec  = unixSec + kNtpUnixOffset;
    ntpFrac = static_cast<uint32_t>(
        (static_cast<uint64_t>(unixUsec) << 32) / 1000000ULL);
}

/** @brief NTP seconds (without fraction) → Unix seconds. */
inline uint32_t ntpToUnixSec(uint32_t ntpSec)
{
    return ntpSec - kNtpUnixOffset;
}

/** @brief Extract the 3-bit mode field of an NTP packet. */
inline uint8_t ntpMode(const NtpPacket& p)
{
    return static_cast<uint8_t>(p.liVnMode & 0x07u);
}

/** @brief Extract the 3-bit version field of an NTP packet. */
inline uint8_t ntpVersion(const NtpPacket& p)
{
    return static_cast<uint8_t>((p.liVnMode >> 3) & 0x07u);
}

/** @brief Build the LI/VN/Mode byte. */
inline uint8_t ntpMakeLiVnMode(uint8_t li, uint8_t vn, uint8_t mode)
{
    return static_cast<uint8_t>(((li & 0x03u) << 6) |
                                ((vn & 0x07u) << 3) |
                                (mode & 0x07u));
}

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_NTPMESSAGE_H
