#include "PtrProbe.h"
#include "DnsMessage.h"

#include <cstring>

namespace dhcp {
namespace dhcp {

namespace {

constexpr uint16_t kTypePtr = 12;          // QTYPE of a reverse lookup
constexpr uint16_t kClassIn = 1;           // QCLASS IN (the Internet)
constexpr size_t kMaxResponseBytes = 512;  // UDP answer we are willing to read
constexpr uint32_t kNoAddress = 0;         // "don't ask"
constexpr uint32_t kBroadcastAddress = 0xFFFFFFFFu;  // nobody owns this name
constexpr size_t kOctets = 4;              // IPv4
constexpr size_t kMaxOctetDigits = 3;      // 255 is the longest decimal octet
constexpr uint8_t kFlagRecursionDesired = 0x01;   // RD bit of the query flags

} // namespace

std::vector<uint8_t> PtrProbe::buildQuery(uint32_t ipNet, uint16_t id)
{
    // A PTR question for "no address" or for the broadcast address is not a
    // question anybody can answer: refuse instead of sending noise.
    if (ipNet == kNoAddress || ipNet == kBroadcastAddress) return std::vector<uint8_t>();

    // The address is kept in network byte order, so its bytes in memory are the
    // wire order (the same assumption every IP_FMT_ARGS in this project makes).
    const uint8_t* octets = reinterpret_cast<const uint8_t*>(&ipNet);

    std::vector<uint8_t> q;
    q.reserve(kMaxQueryBytes);
    q.push_back(static_cast<uint8_t>(id >> 8));
    q.push_back(static_cast<uint8_t>(id & 0xFF));
    q.push_back(kFlagRecursionDesired);     // flags: RD, and nothing else
    q.push_back(0x00);                      // no error, no answer expected yet
    q.push_back(0x00); q.push_back(0x01);   // QDCOUNT = 1
    q.push_back(0x00); q.push_back(0x00);   // ANCOUNT
    q.push_back(0x00); q.push_back(0x00);   // NSCOUNT
    q.push_back(0x00); q.push_back(0x00);   // ARCOUNT

    // Reversed address, e.g. 42.1.168.192.in-addr.arpa — least significant
    // octet first, which is why this walks the bytes backwards.
    for (size_t i = kOctets; i-- > 0; ) {
        char label[kMaxOctetDigits + 1];
        const int n = std::snprintf(label, sizeof(label), "%u", octets[i]);
        if (n <= 0 || static_cast<size_t>(n) > kMaxOctetDigits ||
            q.size() + 1 + n >= kMaxQueryBytes) {
            return std::vector<uint8_t>();
        }
        q.push_back(static_cast<uint8_t>(n));
        q.insert(q.end(), label, label + n);
    }
    for (const char* part : { "in-addr", "arpa" }) {
        const size_t n = std::strlen(part);
        if (q.size() + 1 + n >= kMaxQueryBytes) return std::vector<uint8_t>();
        q.push_back(static_cast<uint8_t>(n));
        q.insert(q.end(), part, part + n);
    }
    q.push_back(0x00);                                   // root label
    q.push_back(static_cast<uint8_t>(kTypePtr >> 8));    // QTYPE = PTR
    q.push_back(static_cast<uint8_t>(kTypePtr & 0xFF));
    q.push_back(static_cast<uint8_t>(kClassIn >> 8));    // QCLASS = IN
    q.push_back(static_cast<uint8_t>(kClassIn & 0xFF));
    return q;
}

std::string PtrProbe::parseResponse(const uint8_t* buf, size_t len, uint16_t id)
{
    uint16_t questions = 0;
    uint16_t answers = 0;
    // The envelope (id, QR, rcode, counts) is DNS itself and belongs to
    // DnsMessage, which the NetBIOS probe uses as well.
    if (!DnsMessage::readHeader(buf, len, id, questions, answers)) return std::string();
    if (answers == 0) return std::string();

    size_t off = DnsMessage::kHeaderBytes;
    for (uint16_t i = 0; i < questions; i++) {
        off = DnsMessage::skipName(buf, len, off);
        if (off + DnsMessage::kQuestionTailBytes > len) return std::string();
        off += DnsMessage::kQuestionTailBytes;           // QTYPE + QCLASS
    }

    for (uint16_t i = 0; i < answers; i++) {
        off = DnsMessage::skipName(buf, len, off);       // owner name
        if (off + DnsMessage::kRecordFixedBytes > len) return std::string();
        const uint16_t type = static_cast<uint16_t>((buf[off] << 8) | buf[off + 1]);
        const uint16_t rdLength = static_cast<uint16_t>((buf[off + DnsMessage::kRdLengthOffset] << 8) |
                                                        buf[off + DnsMessage::kRdLengthOffset + 1]);
        if (off + DnsMessage::kRecordFixedBytes + rdLength > len) return std::string();
        if (type == kTypePtr) return DnsMessage::decodeName(buf, len, off + DnsMessage::kRdataOffset);
        off += DnsMessage::kRecordFixedBytes + rdLength;  // CNAME, A, … : skip
    }
    return std::string();
}

#ifndef DHCP_TEST_HOST

// ─── Socket wrapper (target only) ───────────────────
// The pure parts above are what the host test covers; this is the same thin
// "open, send, wait with a timeout, close" shape the DNS and NTP servers use.

#include "lwip/sockets.h"
#include "esp_random.h"

uint16_t PtrProbe::nextId()
{
    constexpr uint16_t kMaskId = 0xFFFF;
    constexpr uint16_t kFirstUsableId = 1;   // 0 is legal, but a fixed 0 is a poor pattern
    const uint16_t id = static_cast<uint16_t>(esp_random() & kMaskId);
    return (id == kNoId) ? kFirstUsableId : id;
}

std::string PtrProbe::query(uint32_t ipNet, uint32_t serverNet)
{
    constexpr uint16_t kPortDns = 53;        // the name server we ask (UDP)
    if (serverNet == kNoAddress) return std::string();

    const std::vector<uint8_t> request = buildQuery(ipNet, nextId());
    if (request.empty()) return std::string();
    const uint16_t id = static_cast<uint16_t>((request[DnsMessage::kIdOffset] << 8) |
                                              request[DnsMessage::kIdOffset + 1]);

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return std::string();

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(kPortDns);
    to.sin_addr.s_addr = serverNet;

    std::string name;
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
            // An answer from a different server is not our answer: drop it
            // instead of believing a stranger.
            if (received > 0 && from.sin_addr.s_addr == serverNet) {
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
