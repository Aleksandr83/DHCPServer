#pragma once

/**
 * @file Md5.h
 * @brief MD5 (RFC 1321), enough of it to checksum a file we wrote ourselves.
 *
 * Why a local implementation instead of mbedTLS: the value of the checksum is
 * that it can be *tested on the host*, next to the code that uses it. mbedTLS
 * only exists in the ESP-IDF build, so a firmware that hashed with it could not
 * be checked by the host tests at all — and the two places that need this (the
 * cache file and its checksum in NVS) are exactly the places where a silent
 * mismatch costs the operator his working set.
 *
 * MD5 is used here as a **corruption detector**, not as a security primitive:
 * the file lives on the device's own FAT partition and the value is compared
 * with the one the device itself stored in the same device's NVS. Nothing
 * secret is protected by it. Against a deliberate forgery it would be the wrong
 * choice; against a half-written file, a bad block or a file replaced behind the
 * firmware's back it is exactly right, and the whole implementation is a few
 * dozen lines that the host tests can check against the RFC 1321 vectors.
 *
 * Deliberately free of ESP-IDF (plain `fopen` in the file helper), like
 * `DnsStatStore` — that is what lets `test/test_md5.cpp` check it against the
 * RFC 1321 vectors on the host.
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace dhcp::core {

class Md5 {
public:
    Md5() { reset(); }

    /** @brief Start a new digest (the constructor calls it). */
    void reset();

    /** @brief Feed the next @p len bytes of the stream. */
    void update(const void* data, size_t len);

    /**
     * @brief Finish the digest and return it as 32 lowercase hex characters.
     *
     * Does not disturb this object: calling it twice returns the same digest.
     */
    std::string hex();

    /**
     * @brief Digest of a whole file.
     *
     * Reads in 4 KB chunks, so the cost is one pass over the file and no
     * buffer the size of the cache. Returns an empty string when the file
     * cannot be opened or read; @p err (when given) then says why — a missing
     * file and a damaged read are different things for the caller.
     */
    static std::string file(const char* path, std::string* err = nullptr);

private:
    void processBlock(const uint8_t* block);

    uint32_t state_[4];   // A, B, C, D
    uint64_t bitCount_;   // total message length in bits
    uint8_t  buffer_[64]; // partial block
    size_t   buffered_;   // bytes currently in buffer_
};

}  // namespace dhcp::core
