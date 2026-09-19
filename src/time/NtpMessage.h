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

// ─── Bit fields ─────────────────────────────────────
// The first byte of a packet packs three fields (RFC 5905, section 7.3):
// a 2-bit leap indicator, a 3-bit version and a 3-bit mode.
constexpr uint8_t kNtpMaskLeap     = 0x03u;
constexpr uint8_t kNtpMaskMode     = 0x07u;
constexpr uint8_t kNtpShiftVersion = 3;
constexpr uint8_t kNtpShiftLeap    = 6;

// The reply of a server whose clock has never been synchronised is valid, but
// a compliant client has to be able to see that the time is unusable.
constexpr uint8_t  kNtpLeapNoWarning         = 0;   // LI = 0, the clock is fine
constexpr uint8_t  kNtpLeapNotSynchronized   = 3;   // LI = 3, it is not
constexpr uint8_t  kNtpStratumUnsynchronized = 16;  // stratum 16 means "unsynced"
constexpr int8_t   kNtpPrecisionUs           = -20; // ~1 us (log2 of the precision)
constexpr uint32_t kNtpOneSecond16_16        = 0x00010000UL; // 1 s as 16.16
constexpr uint32_t kNtpRefIdLocal            = 0x4C4F434CUL; // "LOCL"
constexpr uint8_t  kNtpVersionMin            = 3;   // older requests are bumped
constexpr uint8_t  kNtpVersionFallback       = 4;   // ...to this version

// ─── Byte order (NTP is always network/big-endian) ──
// One byte of a 32-bit word, and the distance it travels when the word is
// reversed: byte 0 ends up in the most significant position and vice versa.
constexpr uint32_t kMaskByte0 = 0x000000FFu;
constexpr uint32_t kMaskByte1 = 0x0000FF00u;
constexpr uint32_t kMaskByte2 = 0x00FF0000u;
constexpr uint32_t kMaskByte3 = 0xFF000000u;
constexpr int kByteSwapShiftHigh = 24;
constexpr int kByteSwapShiftLow  = 8;

inline uint32_t swap32(uint32_t v)
{
    return ((v & kMaskByte0) << kByteSwapShiftHigh) |
           ((v & kMaskByte1) << kByteSwapShiftLow)  |
           ((v & kMaskByte2) >> kByteSwapShiftLow)  |
           ((v & kMaskByte3) >> kByteSwapShiftHigh);
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

// The fraction of an NTP timestamp is a binary fraction of a second: the low
// kNtpFractionBits bits count 1/2^32 s each, and a second holds this many us.
constexpr size_t kNtpFractionBits = 32;
constexpr uint64_t kUsecPerSecond = 1000000ULL;

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
        (static_cast<uint64_t>(unixUsec) << kNtpFractionBits) / kUsecPerSecond);
}

/** @brief NTP seconds (without fraction) → Unix seconds. */
inline uint32_t ntpToUnixSec(uint32_t ntpSec)
{
    return ntpSec - kNtpUnixOffset;
}

/** @brief Extract the 3-bit mode field of an NTP packet. */
inline uint8_t ntpMode(const NtpPacket& p)
{
    return static_cast<uint8_t>(p.liVnMode & kNtpMaskMode);
}

/** @brief Extract the 3-bit version field of an NTP packet. */
inline uint8_t ntpVersion(const NtpPacket& p)
{
    return static_cast<uint8_t>((p.liVnMode >> kNtpShiftVersion) & kNtpMaskMode);
}

/** @brief Build the LI/VN/Mode byte. */
inline uint8_t ntpMakeLiVnMode(uint8_t li, uint8_t vn, uint8_t mode)
{
    return static_cast<uint8_t>(((li & kNtpMaskLeap) << kNtpShiftLeap) |
                                ((vn & kNtpMaskMode) << kNtpShiftVersion) |
                                (mode & kNtpMaskMode));
}

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_NTPMESSAGE_H
