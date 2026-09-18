/**
 * @file Md5.cpp
 * @brief MD5 (RFC 1321) — see the header for why it lives here.
 */

#include "Md5.h"

#include <cstdio>
#include <cstring>

namespace dhcp::core {

namespace {

// Per-round left-rotation amounts and the sine-derived constants, as in RFC 1321.
constexpr uint32_t kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

constexpr uint32_t kTable[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

inline uint32_t rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t getU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void putU32le(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace

void Md5::reset()
{
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    bitCount_ = 0;
    buffered_ = 0;
    memset(buffer_, 0, sizeof(buffer_));
}

void Md5::processBlock(const uint8_t* block)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = getU32(block + i * 4);

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];

    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + kTable[i] + m[g], kShift[i]);
        a = tmp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bitCount_ += static_cast<uint64_t>(len) * 8u;

    // Fill the partial block first, then work through whole blocks, then keep
    // the tail. A zero-length update is legal and does nothing.
    if (buffered_ != 0) {
        const size_t need = 64 - buffered_;
        const size_t take = (len < need) ? len : need;
        memcpy(buffer_ + buffered_, p, take);
        buffered_ += take;
        p += take;
        len -= take;
        if (buffered_ == 64) {
            processBlock(buffer_);
            buffered_ = 0;
        }
    }
    while (len >= 64) {
        processBlock(p);
        p += 64;
        len -= 64;
    }
    if (len != 0) {
        memcpy(buffer_, p, len);
        buffered_ = len;
    }
}

std::string Md5::hex()
{
    // Finish on a copy: hex() must not disturb this object — a second call has
    // to return the same digest, and the caller may still want to keep the
    // stream for something else (RFC 1321 §3.2).
    Md5 tail = *this;

    const uint64_t bits = tail.bitCount_;
    uint8_t pad[72];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    // 0x80, then zeros until 8 bytes are left in the block, then the length.
    const size_t padLen = (tail.buffered_ <= 55) ? (56 - tail.buffered_)
                                                 : (120 - tail.buffered_);
    tail.update(pad, padLen);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
    tail.update(lenBytes, 8);

    uint8_t out[16];
    for (int i = 0; i < 4; i++) putU32le(out + i * 4, tail.state_[i]);

    static const char* kHexDigits = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 16; i++) {
        s.push_back(kHexDigits[out[i] >> 4]);
        s.push_back(kHexDigits[out[i] & 0x0F]);
    }
    return s;
}

std::string Md5::file(const char* path, std::string* err)
{
    if (err) err->clear();
    if (!path || !*path) {
        if (err) *err = "no path";
        return "";
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err) *err = "cannot open the file";
        return "";
    }

    Md5 md5;
    uint8_t buf[4096];
    for (;;) {
        const size_t got = fread(buf, 1, sizeof(buf), f);
        if (got != 0) md5.update(buf, got);
        if (got < sizeof(buf)) {
            if (ferror(f)) {
                fclose(f);
                if (err) *err = "read error";
                return "";
            }
            break;   // EOF
        }
    }
    fclose(f);
    return md5.hex();
}

}  // namespace dhcp::core
