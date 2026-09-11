#include "Subnet.h"

#include <cstdio>

namespace dhcp {
namespace core {

namespace {

/** Index of the first space/tab, or npos-like size when there is none. */
bool isSpace(char c)
{
    return c == ' ' || c == '\t';
}

} // namespace

bool Subnet::parseIp4(const std::string& text, uint32_t& out)
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
            value = value * 10 + (text[i] - '0');
            ++digits;
            if (digits > 3 || value > 255) return false;
            ++i;
        }
        if (digits == 0) return false;               // empty or non-digit
        if (part > 3) return false;                  // too many octets
        addr |= static_cast<uint32_t>(value) << (24 - 8 * part);
        ++part;

        if (i == end) break;
        if (text[i] != '.') return false;            // garbage inside
        ++i;
        if (i == end) return false;                  // trailing dot
    }

    if (part != 4) return false;
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
    if (prefixLen < 0 || prefixLen > 32) return 0;
    if (prefixLen == 0) return 0;
    return 0xFFFFFFFFu << (32 - prefixLen);
}

int Subnet::prefixLength(uint32_t mask)
{
    if (!isValidMask(mask)) return -1;
    return 32 - hostBits(mask);
}

std::string Subnet::toString(uint32_t addr)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  static_cast<unsigned>((addr >> 24) & 0xFF),
                  static_cast<unsigned>((addr >> 16) & 0xFF),
                  static_cast<unsigned>((addr >> 8) & 0xFF),
                  static_cast<unsigned>(addr & 0xFF));
    return std::string(buf);
}

} // namespace core
} // namespace dhcp
