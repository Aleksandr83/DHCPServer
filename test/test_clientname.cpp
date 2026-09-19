/**
 * @file test_clientname.cpp
 * @brief Unit tests for the client name a DHCP packet carries (option 12 host
 *        name, option 81 client FQDN) and for the filtering of that text.
 *
 * The module is plain C++ on purpose (no ESP-IDF, no sockets, no lwIP), so this
 * test runs on a **host**: the input is a byte array shaped like the option area
 * of a DHCP message, which is exactly what the device hands in.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -I. \
 *       test/test_clientname.cpp src/dhcp/DhcpClientName.cpp host_main.cpp \
 *       -o test_clientname
 */

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "../src/dhcp/DhcpClientName.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, std::string(a).c_str(), std::string(b).c_str()); return 1; } } while(0)

using dhcp::dhcp::DhcpClientName;

namespace {

/** An option area, built the way a client builds it: code, length, payload. */
class Options {
public:
    Options& add(uint8_t code, const std::string& payload)
    {
        buf_.push_back(code);
        buf_.push_back(static_cast<uint8_t>(payload.size()));
        buf_.insert(buf_.end(), payload.begin(), payload.end());
        return *this;
    }
    Options& pad()
    {
        buf_.push_back(0);
        return *this;
    }
    Options& end()
    {
        buf_.push_back(255);
        return *this;
    }
    /** Two bytes that claim more than the buffer holds. */
    Options& truncated(uint8_t code, uint8_t claimed)
    {
        buf_.push_back(code);
        buf_.push_back(claimed);
        buf_.push_back('X');
        return *this;
    }

    std::string name() const { return DhcpClientName::fromOptions(buf_.data(), buf_.size()); }
    std::string host() const { return DhcpClientName::hostNameOption(buf_.data(), buf_.size()); }
    std::string fqdn() const { return DhcpClientName::fqdnOption(buf_.data(), buf_.size()); }
    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

/** Option 81 payload: flags, rcode1, rcode2, then the (DNS-encoded) name. */
std::string fqdnPayload(uint8_t flags, const std::string& dnsName)
{
    std::string p;
    p.push_back(static_cast<char>(flags));
    p.push_back(0);
    p.push_back(0);
    p += dnsName;
    return p;
}

/** DNS-encode a dotted name: <len>label<len>label<0>. */
std::string dnsEncode(const std::string& dotted)
{
    std::string out;
    size_t pos = 0;
    while (pos <= dotted.size()) {
        const size_t dot = dotted.find('.', pos);
        const std::string label = dotted.substr(pos, (dot == std::string::npos)
                                                         ? std::string::npos : dot - pos);
        if (!label.empty()) {
            out.push_back(static_cast<char>(label.size()));
            out += label;
        }
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    out.push_back('\0');
    return out;
}

} // namespace

extern "C" {

/** Option 12 is the name; NUL padding is not part of it. */
static int test_option12()
{
    Options o;
    o.add(12, std::string("office-pc", 9) + std::string(1, '\0') + std::string("junk"));
    o.end();
    TEST_ASSERT_STR_EQ(o.host(), "office-pc");
    TEST_ASSERT_STR_EQ(o.name(), "office-pc");

    // The same option without a NUL terminator (legal: the length is the end).
    Options plain;
    plain.add(12, "tablet").pad().end();
    TEST_ASSERT_STR_EQ(plain.name(), "tablet");

    // Nothing at all, and an empty option: no name, no crash.
    Options empty;
    empty.end();
    TEST_ASSERT_STR_EQ(empty.name(), "");
    Options zeroLen;
    zeroLen.add(12, "").end();
    TEST_ASSERT_STR_EQ(zeroLen.host(), "");

    // Options in front of it, and a PAD between them, must not confuse the walk.
    Options padded;
    padded.add(53, std::string(1, '\x01')).pad().add(50, std::string(4, '\x00')).add(12, "nas-1").end();
    TEST_ASSERT_STR_EQ(padded.name(), "nas-1");
    return 0;
}

/** A malformed option list is refused, not read past. */
static int test_malformed_options()
{
    Options truncated;
    truncated.truncated(12, 200);   // claims 200 bytes, holds 1
    TEST_ASSERT_STR_EQ(truncated.host(), "");

    Options noEnd;                  // runs to the end of the buffer without 0xFF
    noEnd.add(12, "abc");
    TEST_ASSERT_STR_EQ(noEnd.host(), "abc");

    // No options at all (nullptr) — the caller may pass an empty area.
    TEST_ASSERT_STR_EQ(DhcpClientName::fromOptions(nullptr, 0), "");

    // Length byte missing at the very end.
    const uint8_t lonely[] = { 12 };
    TEST_ASSERT_STR_EQ(DhcpClientName::fromOptions(lonely, sizeof(lonely)), "");
    return 0;
}

/** Option 81: DNS-encoded and plain text, with and without the host part. */
static int test_option81()
{
    Options encoded;
    encoded.add(81, fqdnPayload(0x04, dnsEncode("pc1.lan"))).end();
    TEST_ASSERT_STR_EQ(encoded.fqdn(), "pc1.lan");
    TEST_ASSERT_STR_EQ(encoded.name(), "pc1");     // the host part is the name

    // E flag clear: the name is plain text (the RFC 4702 deprecation path), and
    // a trailing dot means nothing.
    Options plain;
    plain.add(81, fqdnPayload(0x00, std::string("pc2.lan.") + std::string(1, '\0'))).end();
    TEST_ASSERT_STR_EQ(plain.fqdn(), "pc2.lan");
    TEST_ASSERT_STR_EQ(plain.name(), "pc2");

    // Option 12 wins when both are present: it IS the computer name.
    Options both;
    both.add(12, "chosen").add(81, fqdnPayload(0x04, dnsEncode("other.lan"))).end();
    TEST_ASSERT_STR_EQ(both.name(), "chosen");
    TEST_ASSERT_STR_EQ(both.fqdn(), "other.lan");

    // A client that sent only the domain (allowed by RFC 4702) leaves us with
    // what it sent: the page shows a suggestion, and the operator can fix it.
    Options domainOnly;
    domainOnly.add(81, fqdnPayload(0x04, dnsEncode("lan"))).end();
    TEST_ASSERT_STR_EQ(domainOnly.name(), "lan");

    // Too short to hold flags + rcodes: refused instead of read past the end.
    Options short81;
    short81.add(81, std::string(2, '\x04')).end();
    TEST_ASSERT_STR_EQ(short81.fqdn(), "");

    // A label length that runs past the option: refused.
    Options badLabel;
    badLabel.add(81, fqdnPayload(0x04, std::string(1, static_cast<char>(60)) + "abc")).end();
    TEST_ASSERT_STR_EQ(badLabel.fqdn(), "");
    return 0;
}

/** The text is filtered before anybody else sees it. */
static int test_sanitize()
{
    // Separators and control bytes are dropped: the name is copied into the
    // allow-list blob ("mac|name|enabled") and printed into JSON.
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize("a|b"), "ab");
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize(std::string("a\nb\rc")), "abc");
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize(std::string("ok\x01\x02")), "ok");
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize("  spaced  "), "spaced");
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize(""), "");

    // A quote or a backslash would end the JSON string early if it were copied
    // raw — they are printable and harmless here, but the page inserts them into
    // an HTML attribute, so they must survive as they are (the page escapes).
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize("a\"b"), "a\"b");

    // Valid UTF-8 (a name typed in Cyrillic) is kept: dropping it would lose a
    // correct name, and it is valid JSON once the bytes are valid UTF-8.
    const std::string cyrillic = "\xD0\x9F\xD0\x9A";   // "ПК"
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize(cyrillic), cyrillic);

    // Invalid UTF-8 is dropped byte by byte, so the result can be encoded.
    const std::string bad = std::string("a\xD0") + "b";      // lone lead byte
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize(bad), "ab");
    const std::string cont = std::string("a\x80") + "b";     // stray continuation
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize(cont), "ab");
    const std::string truncatedSeq = std::string("a\xE2\x82");  // cut 3-byte form
    TEST_ASSERT_STR_EQ(DhcpClientName::sanitize(truncatedSeq), "a");
    return 0;
}

/** Truncation happens on a character boundary, never inside a sequence. */
static int test_truncate_limit()
{
    const std::string longName(100, 'x');
    TEST_ASSERT_EQ(DhcpClientName::sanitize(longName).size(), DhcpClientName::kMaxLen);

    // The budget is bytes: 20 Cyrillic characters are 40 bytes, so 16 of them
    // fit, and the last one is not cut in half.
    std::string many;
    for (int i = 0; i < 20; i++) many += "\xD0\x9F";   // "П" x20
    const std::string out = DhcpClientName::sanitize(many);
    TEST_ASSERT_EQ(out.size(), DhcpClientName::kMaxLen);         // 32 bytes
    std::string expected;
    for (int i = 0; i < 16; i++) expected += "\xD0\x9F";
    TEST_ASSERT_STR_EQ(out, expected);

    // Where a multi-byte character would be cut, it is dropped as a whole: 31
    // 'a' bytes leave exactly one byte, which cannot hold a 3-byte sign.
    std::string mixed;
    for (int i = 0; i < 31; i++) mixed += 'a';
    mixed += "\xE2\x82\xAC";   // euro sign, 3 bytes
    const std::string mixedOut = DhcpClientName::sanitize(mixed);
    TEST_ASSERT_EQ(mixedOut.size(), 31u);
    TEST_ASSERT_STR_EQ(mixedOut, std::string(31, 'a'));
    return 0;
}

/** shortLabel() is the host part of a dotted name. */
static int test_short_label()
{
    TEST_ASSERT_STR_EQ(DhcpClientName::shortLabel("pc1.lan"), "pc1");
    TEST_ASSERT_STR_EQ(DhcpClientName::shortLabel("pc1"), "pc1");
    TEST_ASSERT_STR_EQ(DhcpClientName::shortLabel("pc1."), "pc1");
    TEST_ASSERT_STR_EQ(DhcpClientName::shortLabel(""), "");
    TEST_ASSERT_STR_EQ(DhcpClientName::shortLabel(".lan"), "");
    return 0;
}

void app_main()
{
    printf("Running DhcpClientName tests...\n");
    int failures = 0;

    failures += test_option12();
    failures += test_malformed_options();
    failures += test_option81();
    failures += test_sanitize();
    failures += test_truncate_limit();
    failures += test_short_label();

    if (failures == 0) {
        printf("All DhcpClientName tests PASSED!\n");
    } else {
        printf("Some DhcpClientName tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#else   // !DHCP_TEST_HOST

#include <cstdio>

extern "C" void app_main()
{
    printf("test_clientname is a host-only test (build with -DDHCP_TEST_HOST)"
           " - see the file header.\n");
}

#endif  // DHCP_TEST_HOST
