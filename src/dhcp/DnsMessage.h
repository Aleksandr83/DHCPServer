#ifndef DHCP_DHCP_DNSMESSAGE_H
#define DHCP_DHCP_DNSMESSAGE_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace dhcp {
namespace dhcp {

/**
 * @brief The little bit of the DNS wire format both name probes need.
 *
 * The reverse-DNS probe and the NetBIOS node-status probe are different
 * protocols that happen to share the same message envelope: a header, a question
 * whose name is written as length-prefixed labels, and answers whose names are
 * usually **compressed** — a pointer back into text that already appeared. That
 * envelope is what lives here, once, because two copies of a name decoder is
 * how the two copies start to disagree about a malformed packet.
 *
 * Everything is a pure function on bytes: no sockets, no ESP-IDF, so a damaged
 * message can be built by hand in a test and handed in.
 */
class DnsMessage {
public:
    /** @brief Longest header the probes expect (id, flags, 4 counters). */
    static constexpr size_t kHeaderBytes = 12;

    // ─── Header layout (RFC 1035, section 4.1.1) ─────────
    // Offsets and masks are named because a bare `buf[4] << 8 | buf[5]` says
    // nothing about *which* counter it reads.
    static constexpr size_t kIdOffset = 0;           // 16-bit transaction id
    static constexpr size_t kFlags1Offset = 2;       // QR, opcode, AA, TC, RD
    static constexpr size_t kFlags2Offset = 3;       // RA, Z, RCODE
    static constexpr size_t kQdCountOffset = 4;      // question count
    static constexpr size_t kAnCountOffset = 6;      // answer count
    static constexpr uint8_t kFlagResponse = 0x80;   // QR bit: this is an answer
    static constexpr uint8_t kMaskRcode = 0x0F;      // low nibble of flags 2

    // ─── Name and record layout ──────────────────────────
    static constexpr uint8_t kMaskLabelType = 0xC0;  // top two bits of a length byte
    static constexpr uint8_t kLabelPointer = 0xC0;   // 11: the rest is an offset
    static constexpr uint8_t kMaskPointerOffset = 0x3F;  // low 14 bits of a pointer
    static constexpr size_t kMaxLabelBytes = 63;     // RFC 1035: 2^6 - 1
    static constexpr size_t kQuestionTailBytes = 4;  // QTYPE + QCLASS
    static constexpr size_t kRecordFixedBytes = 10;  // TYPE, CLASS, TTL, RDLENGTH
    static constexpr size_t kRdLengthOffset = 8;     // RDLENGTH inside the fixed part
    static constexpr size_t kRdataOffset = kRecordFixedBytes;  // where RDATA starts

    /**
     * @brief Compression-pointer jumps allowed before a name is refused.
     *
     * A pointer chain that never reaches a label is a malformed message; the
     * budget is what turns "walk forever" into "no name".
     */
    static constexpr size_t kMaxNameJumps = 8;

    /**
     * @brief Check the header and read the question/answer counts.
     *
     * @param buf   Datagram.
     * @param len   Its length.
     * @param id    Transaction id the query was sent with.
     * @param qdOut Questions in the message.
     * @param anOut Answers in the message.
     * @return false when the message is too short, answers another query
     *         (QR clear), or reports an error (rcode != 0).
     */
    static bool readHeader(const uint8_t* buf, size_t len, uint16_t id,
                           uint16_t& qdOut, uint16_t& anOut);

    /**
     * @brief Offset just past the name at `off` (a pointer ends it).
     * @return `len` + 1 when the message is damaged or a pointer points forward.
     */
    static size_t skipName(const uint8_t* buf, size_t len, size_t off);

    /**
     * @brief Read the name at `off`, following compression pointers.
     * @return The dotted name, or "" when the message is damaged, a label runs
     *         past the buffer, a pointer points forward, or the pointers loop.
     */
    static std::string decodeName(const uint8_t* buf, size_t len, size_t off);
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DNSMESSAGE_H
