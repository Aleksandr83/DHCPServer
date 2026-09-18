// Host test of the MD5 implementation used for the cache-file checksum.
//
// The checksum decides whether a cache file is loaded or refused, so "the
// digest is wrong on some input length" is a defect that must be provable
// without a board. The expected values come from RFC 1321 and, for the block
// boundaries, from an independent implementation (Python's hashlib) over a
// deterministic byte pattern — the boundaries (55/56/57, 63/64/65, 119/120/121)
// are exactly where a hand-written MD5 goes wrong.
//
// Build (MinGW g++ 13, PATH must contain C:\Qt\Tools\mingw1310_64\bin):
//   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -Itest/stubs -I.
//       test/test_md5.cpp src/core/Md5.cpp -o t_md5.exe
//
// The harness always exits with 0 — the proof is the printed text.

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "src/core/Md5.h"

using dhcp::core::Md5;

static int g_fail = 0;

static void check(bool ok, const std::string& what)
{
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

static void checkEq(const std::string& got, const std::string& want,
                    const std::string& what)
{
    if (got == want) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n        got  %s\n        want %s\n",
                    what.c_str(), got.c_str(), want.c_str());
        ++g_fail;
    }
}

static std::string hashOf(const std::string& s)
{
    Md5 md5;
    md5.update(s.data(), s.size());
    return md5.hex();
}

// The deterministic payload the boundary table below was generated from.
static std::string pattern(size_t n)
{
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) {
        s[i] = static_cast<char>((i * 37 + 11) & 0xFF);
    }
    return s;
}

static std::string tempBase()
{
    const char* tmp = std::getenv("TEMP");
    if (!tmp || !*tmp) tmp = std::getenv("TMP");
    if (!tmp || !*tmp) tmp = ".";
    return std::string(tmp);
}

// ─── RFC 1321 test suite (appendix A.5) ─────────────────────────────

static void test_rfc1321_vectors()
{
    std::printf("test_rfc1321_vectors\n");
    struct Vec { const char* text; const char* md5; };
    const Vec vectors[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
         "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"123456789012345678901234567890123456789012345678901234567890123456789"
         "01234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (const Vec& v : vectors) {
        const std::string name = std::string("RFC 1321: \"") +
                                 (std::strlen(v.text) > 20 ? "<long>" : v.text) + "\"";
        checkEq(hashOf(v.text), v.md5, name);
    }

    // A million 'a' is the one vector that exercises the bit counter rather
    // than the block loop, and it is fed one byte at a time here.
    Md5 md5;
    const char a = 'a';
    for (int i = 0; i < 1000000; i++) md5.update(&a, 1);
    checkEq(md5.hex(), "7707d6ae4e027c70eea2a935c2296f21",
            "RFC 1321: one million 'a', fed byte by byte");
}

// ─── Block boundaries, block counts and the bit-counter split ────────

static void test_block_boundaries()
{
    std::printf("test_block_boundaries\n");
    struct Vec { size_t len; const char* md5; };
    const Vec vectors[] = {
        {0, "d41d8cd98f00b204e9800998ecf8427e"},
        {1, "13c8ffd977013703a701cf8e11deac65"},
        {54, "5e5534f470ef1d65657807ed96610693"},
        {55, "d872aa0473a24da995ce4ac518ade767"},
        {56, "e23567645846677c205de80f9779081b"},
        {57, "1fe5e4a27e12484d7203d0b60c21c51f"},
        {63, "4775b66278a8fc132ff80923378216cd"},
        {64, "71e123b70c7aa64826fcfe472694cd1c"},
        {65, "5949948f26e35203661075214faa3966"},
        {119, "87f72c7241c5fe218bb4df8c0aba3232"},
        {120, "bf240b8b7407fba3805adeec63a03b77"},
        {121, "c51004ed1ef4271c82106468a13fe8cf"},
        {127, "520bdc2dbab7c25d64263ffb242d9e98"},
        {128, "3e93b378458b77da96b2357c3bda8cc2"},
        {1000, "c3cb8ae118768782aa1896d7fb2827ef"},
        {4096, "efc404fa609a799c7801273de6d65e84"},
        {4109, "c09a1ed7a42dba1a1d9969f666ebf661"},
    };
    for (const Vec& v : vectors) {
        checkEq(hashOf(pattern(v.len)), v.md5,
                "length " + std::to_string(v.len) + " (one shot)");
    }
}

static void test_chunking_does_not_change_the_digest()
{
    std::printf("test_chunking_does_not_change_the_digest\n");
    const std::string data = pattern(4109);
    const std::string want = "c09a1ed7a42dba1a1d9969f666ebf661";

    const size_t chunks[] = {1, 3, 7, 63, 64, 65, 127, 128, 4096};
    for (size_t c : chunks) {
        Md5 md5;
        size_t off = 0;
        while (off < data.size()) {
            const size_t take = (data.size() - off < c) ? (data.size() - off) : c;
            md5.update(data.data() + off, take);
            off += take;
        }
        checkEq(md5.hex(), want, "4109 bytes in " + std::to_string(c) + "-byte chunks");
    }

    // A zero-length update in the middle must not disturb the stream (the file
    // helper can hand over a short read, and MQTT-style callers do this).
    Md5 md5;
    md5.update(data.data(), 100);
    md5.update(data.data() + 100, 0);
    md5.update(data.data() + 100, data.size() - 100);
    checkEq(md5.hex(), want, "a zero-length update in the middle changes nothing");

    // hex() must be repeatable and must not end the stream: the same object can
    // still be fed and produce the digest of the longer message.
    Md5 again;
    again.update(data.data(), 200);
    const std::string twice1 = again.hex();
    checkEq(again.hex(), twice1, "hex() is idempotent");
    again.update(data.data() + 200, data.size() - 200);
    checkEq(again.hex(), want, "the object still works after hex()");
}

// ─── The file helper (what the firmware actually calls) ──────────────

static void test_file_helper()
{
    std::printf("test_file_helper\n");
    const std::string path = tempBase() + "/dhcpserver_md5_test.bin";
    const std::string data = pattern(4109);
    const std::string want = "c09a1ed7a42dba1a1d9969f666ebf661";

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        check(false, "could not create " + path);
        return;
    }
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);

    std::string err;
    checkEq(Md5::file(path.c_str(), &err), want, "the digest of a file equals the digest of its bytes");
    check(err.empty(), "no error for a readable file");

    // The firmware must be able to tell "no file" from "damaged file": both
    // give an empty digest, and the reason is what separates them.
    checkEq(Md5::file((tempBase() + "/dhcpserver_md5_missing.bin").c_str(), &err), "",
            "a missing file has no digest");
    check(!err.empty(), "and says why: " + err);

    checkEq(Md5::file("", &err), "", "an empty path has no digest");
    check(!err.empty(), "and says why: " + err);

    // An empty file is a legal file, and its digest is the one of the empty
    // message — not the same thing as "unreadable".
    const std::string empty = tempBase() + "/dhcpserver_md5_empty.bin";
    FILE* g = std::fopen(empty.c_str(), "wb");
    if (g) std::fclose(g);
    err.clear();
    checkEq(Md5::file(empty.c_str(), &err), "d41d8cd98f00b204e9800998ecf8427e",
            "an empty file hashes as the empty message");
    check(err.empty(), "and is not an error");

    // A file big enough to need several read chunks, hashed against the same
    // bytes fed in one go.
    const std::string big = tempBase() + "/dhcpserver_md5_big.bin";
    const std::string bigData = pattern(20000);
    FILE* h = std::fopen(big.c_str(), "wb");
    if (h) {
        std::fwrite(bigData.data(), 1, bigData.size(), h);
        std::fclose(h);
    }
    checkEq(Md5::file(big.c_str()), hashOf(bigData),
            "20000 bytes spanning several 4 KB reads");

    std::remove(path.c_str());
    std::remove(empty.c_str());
    std::remove(big.c_str());
}

int main()
{
    test_rfc1321_vectors();
    test_block_boundaries();
    test_chunking_does_not_change_the_digest();
    test_file_helper();

    if (g_fail != 0) {
        std::printf("FAILED (%d checks)\n", g_fail);
    } else {
        std::printf("PASSED!\n");
    }
    return 0;
}

#endif // DHCP_TEST_HOST
