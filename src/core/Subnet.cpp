#include "Subnet.h"

#include <cstdio>

using namespace std;

namespace dhcp {
namespace core {

namespace {

// Rule 39: the shape of an IPv4 address, in text and in bits.
constexpr int kIpv4Octets = 4;       // its four dotted parts
constexpr int kOctetBits = 8;        // eight bits each
constexpr int kIpv4Bits = 32;        // and the address as a whole
constexpr int kOctetMax = 255;       // no part may be larger
constexpr int kMaxOctetDigits = 3;   // nor longer than three digits
constexpr int kDecimalBase = 10;
constexpr int kLowByteMask = 0xFF;   // one octet out of the 32 bits
constexpr int kIp4TextLen = 16;      // "255.255.255.255" plus the NUL

/** Index of the first space/tab, or npos-like size when there is none. */
bool isSpace(char c)
{
    return c == ' ' || c == '\t';
}

} // namespace

bool Subnet::parseIp4(const string& text, uint32_t& out)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    if (begin == end) return false;

    uint32_t addr = 0;
    int part = 0;
    size_t i = begin;

    while (i < end) {
        int value = 0;
        int digits = 0;
        while (i < end && text[i] >= '0' && text[i] <= '9') {
            value = value * kDecimalBase + (text[i] - '0');
            ++digits;
            if (digits > kMaxOctetDigits || value > kOctetMax) return false;
            ++i;
        }
        if (digits == 0) return false;               // empty or non-digit
        if (part >= kIpv4Octets) return false;       // too many octets
        addr |= static_cast<uint32_t>(value)
                << (kOctetBits * (kIpv4Octets - 1 - part));
        ++part;

        if (i == end) break;
        if (text[i] != '.') return false;            // garbage inside
        ++i;
        if (i == end) return false;                  // trailing dot
    }

    if (part != kIpv4Octets) return false;
    out = addr;
    return true;
}

bool Subnet::isValidMask(uint32_t mask)
{
    // A contiguous mask is (1…1)(0…0): inverting it must leave only low bits,
    // i.e. ~mask + 1 must be a power of two (0 and 0xFFFFFFFF included).
    const uint32_t inv = ~mask;
    return (inv & (inv + 1)) == 0;
}

uint32_t Subnet::network(uint32_t addr, uint32_t mask)
{
    return addr & mask;
}

bool Subnet::contains(uint32_t net, uint32_t mask, uint32_t addr)
{
    if (mask == 0 || !isValidMask(mask)) return false;
    return (addr & mask) == (net & mask);
}

int Subnet::hostBits(uint32_t mask)
{
    int bits = 0;
    uint32_t m = mask;
    while (m != 0 && (m & 1u) == 0) {
        ++bits;
        m >>= 1;
    }
    return bits;
}

uint32_t Subnet::maskFromPrefix(int prefixLen)
{
    if (prefixLen < 0 || prefixLen > kIpv4Bits) return 0;
    if (prefixLen == 0) return 0;
    return 0xFFFFFFFFu << (kIpv4Bits - prefixLen);
}

int Subnet::prefixLength(uint32_t mask)
{
    if (!isValidMask(mask)) return -1;
    return kIpv4Bits - hostBits(mask);
}

string Subnet::toString(uint32_t addr)
{
    char buf[kIp4TextLen];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  static_cast<unsigned>((addr >> (3 * kOctetBits)) & kLowByteMask),
                  static_cast<unsigned>((addr >> (2 * kOctetBits)) & kLowByteMask),
                  static_cast<unsigned>((addr >> kOctetBits) & kLowByteMask),
                  static_cast<unsigned>(addr & kLowByteMask));
    return string(buf);
}

} // namespace core
} // namespace dhcp
