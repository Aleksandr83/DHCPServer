#include "NbstatProbe.h"
#include "DnsMessage.h"

#include <cstring>

using namespace std;

namespace dhcp {
namespace dhcp {

namespace {

constexpr uint16_t kTypeNbstat = 0x0021;           // QTYPE of a node-status query
constexpr uint16_t kClassIn = 0x0001;              // QCLASS IN (the Internet)
constexpr size_t kMaxResponseBytes = 1024;         // UDP answer we are willing to read
constexpr uint16_t kPortNetbiosNameService = 137;  // UDP port NBSTAT lives on
constexpr uint32_t kNoAddress = 0;                 // "don't ask"
constexpr uint32_t kBroadcastAddress = 0xFFFFFFFFu;  // nobody owns this name

// Layout of one name-table entry (RFC 1002, section 4.2.18).
constexpr size_t kEntryNameBytes = 15;      // the name itself, blank padded
constexpr size_t kEntrySuffixOffset = 15;   // service suffix byte
constexpr size_t kEntryFlagsOffset = 16;    // group/unique flag word
constexpr size_t kTableCountBytes = 1;      // the count byte before the table

// First-level encoding: every byte becomes two letters, high nibble first.
constexpr int kNibbleShift = 4;
constexpr uint8_t kNibbleMask = 0x0F;       // the low nibble, the second letter
constexpr char kEncodingBase = 'A';         // nibble 0 is 'A', nibble 15 is 'P'
constexpr uint8_t kWildcardNameByte = '*';  // the name every NetBIOS host answers

/** One name-table entry, read out of the answer. */
struct NameEntry {
    string name;    // trailing blanks/NULs already removed
    uint8_t suffix;
    uint16_t flags;
};

/** @brief Read `count` entries at `off`; false when the answer is too short. */
bool readTable(const uint8_t* buf, size_t len, size_t off, uint8_t count,
               vector<NameEntry>& out)
{
    if (off + static_cast<size_t>(count) * NbstatProbe::kEntryBytes > len) return false;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t* entry = buf + off + static_cast<size_t>(i) * NbstatProbe::kEntryBytes;
        NameEntry e;
        // The name is padded with spaces (or NULs) to its 15 bytes; the 16th
        // byte of the entry is the service suffix.
        size_t nameLen = kEntryNameBytes;
        while (nameLen > 0 && (entry[nameLen - 1] == ' ' || entry[nameLen - 1] == 0)) {
            nameLen--;
        }
        e.name.assign(reinterpret_cast<const char*>(entry), nameLen);
        e.suffix = entry[kEntrySuffixOffset];
        e.flags = static_cast<uint16_t>((entry[kEntryFlagsOffset] << 8) |
                                        entry[kEntryFlagsOffset + 1]);
        out.push_back(e);
    }
    return true;
}

} // namespace

vector<uint8_t> NbstatProbe::buildQuery(uint16_t id)
{
    vector<uint8_t> q;
    q.reserve(kMaxQueryBytes);

    q.push_back(static_cast<uint8_t>(id >> 8));
    q.push_back(static_cast<uint8_t>(id & 0xFF));
    q.push_back(0x00); q.push_back(0x00);   // flags: a unicast question, no RD
    q.push_back(0x00); q.push_back(0x01);   // QDCOUNT = 1
    q.push_back(0x00); q.push_back(0x00);   // ANCOUNT
    q.push_back(0x00); q.push_back(0x00);   // NSCOUNT
    q.push_back(0x00); q.push_back(0x00);   // ARCOUNT

    // The wildcard name "*" padded with NULs to 16 bytes, in NetBIOS
    // "first-level encoding": every byte becomes two letters A..P (high nibble
    // first), and the whole thing is a 32-byte label.
    uint8_t raw[kNameBytes];
    memset(raw, 0, sizeof(raw));
    raw[0] = kWildcardNameByte;
    q.push_back(static_cast<uint8_t>(kNameBytes * 2));   // label length = 0x20
    for (size_t i = 0; i < kNameBytes; i++) {
        q.push_back(static_cast<uint8_t>(kEncodingBase + (raw[i] >> kNibbleShift)));
        q.push_back(static_cast<uint8_t>(kEncodingBase + (raw[i] & kNibbleMask)));
    }
    q.push_back(0x00);                                   // end of the name

    q.push_back(static_cast<uint8_t>(kTypeNbstat >> 8)); // QTYPE = NBSTAT
    q.push_back(static_cast<uint8_t>(kTypeNbstat & 0xFF));
    q.push_back(static_cast<uint8_t>(kClassIn >> 8));    // QCLASS = IN
    q.push_back(static_cast<uint8_t>(kClassIn & 0xFF));
    return q;
}

string NbstatProbe::parseResponse(const uint8_t* buf, size_t len, uint16_t id)
{
    uint16_t questions = 0;
    uint16_t answers = 0;
    if (!DnsMessage::readHeader(buf, len, id, questions, answers)) return string();
    if (answers == 0) return string();

    size_t off = DnsMessage::kHeaderBytes;
    for (uint16_t i = 0; i < questions; i++) {
        off = DnsMessage::skipName(buf, len, off);
        if (off + DnsMessage::kQuestionTailBytes > len) return string();
        off += DnsMessage::kQuestionTailBytes;           // QTYPE + QCLASS
    }

    for (uint16_t i = 0; i < answers; i++) {
        off = DnsMessage::skipName(buf, len, off);       // owner name
        if (off + DnsMessage::kRecordFixedBytes > len) return string();
        const uint16_t type = static_cast<uint16_t>((buf[off] << 8) | buf[off + 1]);
        const uint16_t rdLength = static_cast<uint16_t>((buf[off + DnsMessage::kRdLengthOffset] << 8) |
                                                        buf[off + DnsMessage::kRdLengthOffset + 1]);
        if (off + DnsMessage::kRecordFixedBytes + rdLength > len) return string();
        if (type != kTypeNbstat) {                       // another record: skip it
            off += DnsMessage::kRecordFixedBytes + rdLength;
            continue;
        }
        // The record data of a node status answer IS the name table: one count
        // byte, then that many entries.
        const size_t table = off + DnsMessage::kRdataOffset;
        if (rdLength < kTableCountBytes || table + rdLength > len) return string();
        const uint8_t count = buf[table];
        vector<NameEntry> entries;
        if (!readTable(buf, table + rdLength, table + kTableCountBytes, count, entries))
            return string();

        string firstUnique;
        for (const NameEntry& e : entries) {
            if ((e.flags & kFlagGroup) != 0) continue;   // a group is not a computer
            if (e.name.empty()) continue;
            if (e.suffix == kSuffixWorkstation) return e.name;
            if (firstUnique.empty()) firstUnique = e.name;
        }
        return firstUnique;
    }
    return string();
}

#ifndef DHCP_TEST_HOST

// ─── Socket wrapper (target only) ───────────────────
// Same shape as the PTR probe: open, send, wait with a timeout, close.

#include "lwip/sockets.h"
#include "esp_random.h"

uint16_t NbstatProbe::nextId()
{
    constexpr uint16_t kMaskId = 0xFFFF;
    constexpr uint16_t kFirstUsableId = 1;   // 0 is legal, but a fixed 0 is a poor pattern
    const uint16_t id = static_cast<uint16_t>(esp_random() & kMaskId);
    return (id == kNoId) ? kFirstUsableId : id;
}

string NbstatProbe::query(uint32_t ipNet)
{
    if (ipNet == kNoAddress || ipNet == kBroadcastAddress) return string();

    const vector<uint8_t> request = buildQuery(nextId());
    if (request.empty()) return string();
    const uint16_t id = static_cast<uint16_t>((request[DnsMessage::kIdOffset] << 8) |
                                              request[DnsMessage::kIdOffset + 1]);

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return string();

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(kPortNetbiosNameService);
    to.sin_addr.s_addr = ipNet;

    string name;
    // A machine that does not speak NetBIOS often answers the datagram with an
    // ICMP port-unreachable, which this socket reports as an error: that is not
    // a failure of the probe, it is the answer "no name here".
    if (sendto(fd, request.data(), request.size(), 0,
               reinterpret_cast<struct sockaddr*>(&to), sizeof(to)) > 0) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        struct timeval timeout;
        timeout.tv_sec = kTimeoutMs / 1000;
        timeout.tv_usec = static_cast<long>(kTimeoutMs % 1000) * 1000;
        if (select(fd + 1, &readSet, nullptr, nullptr, &timeout) > 0) {
            uint8_t answer[kMaxResponseBytes];
            struct sockaddr_in from;
            socklen_t fromLen = sizeof(from);
            const int received = recvfrom(fd, answer, sizeof(answer), 0,
                                          reinterpret_cast<struct sockaddr*>(&from),
                                          &fromLen);
            if (received > 0 && from.sin_addr.s_addr == ipNet) {
                name = parseResponse(answer, static_cast<size_t>(received), id);
            }
        }
    }

    close(fd);
    return name;
}

#endif  // !DHCP_TEST_HOST

} // namespace dhcp
} // namespace dhcp
