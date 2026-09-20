/**
 * @file test_ptrprobe.cpp
 * @brief Unit tests for the reverse-DNS (PTR) probe: the query it builds and the
 *        answers it accepts.
 *
 * The socket is not tested here — the bytes are. Compression pointers are the
 * part of DNS that goes wrong quietly, so they get the most cases: a pointer
 * into the question (what a real resolver sends), a forward pointer, a loop,
 * and a label that runs past the end. None of them may hang or read past the
 * datagram.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -I. \
 *       test/test_ptrprobe.cpp src/dhcp/PtrProbe.cpp host_main.cpp \
 *       -o test_ptrprobe
 *
 * Without `DHCP_TEST_HOST` the file compiles to an `app_main` that does nothing:
 * the interesting cases are bytes in and bytes out, which needs no device.
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../src/dhcp/PtrProbe.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (string(a) != string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, string(a).c_str(), string(b).c_str()); return 1; } } while(0)

using dhcp::dhcp::PtrProbe;

namespace {

/** An IPv4 address as it is kept in the DHCP code: in network byte order. */
uint32_t ipNet(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t bytes[4] = { a, b, c, d };
    uint32_t out = 0;
    memcpy(&out, bytes, 4);
    return out;
}

/** Build a DNS answer the way a resolver does: header, question, one answer. */
class Answer {
public:
    Answer(uint16_t id, bool response = true, uint8_t rcode = 0, uint16_t answers = 1)
    {
        add(id >> 8); add(id & 0xFF);
        add(response ? 0x81 : 0x01);   // QR + RD (or a query back)
        add(rcode);
        add(0); add(1);                // QDCOUNT
        add(answers >> 8); add(answers & 0xFF);
        add(0); add(0);                // NSCOUNT
        add(0); add(0);                // ARCOUNT
    }

    Answer& add(int byte)
    {
        buf_.push_back(static_cast<uint8_t>(byte));
        return *this;
    }

    /** The standard `42.1.168.192.in-addr.arpa PTR` question. */
    Answer& question()
    {
        const char* labels[] = { "42", "1", "168", "192", "in-addr", "arpa" };
        for (const char* label : labels) {
            const size_t n = strlen(label);
            add(static_cast<int>(n));
            for (size_t i = 0; i < n; i++) add(static_cast<unsigned char>(label[i]));
        }
        add(0);
        add(0); add(12);   // QTYPE = PTR
        add(0); add(1);    // QCLASS = IN
        return *this;
    }

    /** Answer owner as a pointer back to the question (offset 12). */
    Answer& ownerPointer(size_t at = 12)
    {
        add(0xC0);
        add(static_cast<int>(at));
        return *this;
    }

    /** Answer owner spelled out in full. */
    Answer& ownerFull()
    {
        const char* labels[] = { "42", "1", "168", "192", "in-addr", "arpa" };
        for (const char* label : labels) {
            const size_t n = strlen(label);
            add(static_cast<int>(n));
            for (size_t i = 0; i < n; i++) add(static_cast<unsigned char>(label[i]));
        }
        add(0);
        return *this;
    }

    /** Type/class/TTL, then a PTR value built by `value`. */
    Answer& ptrRecord(uint16_t type)
    {
        add(type >> 8); add(type & 0xFF);
        add(0); add(1);            // CLASS = IN
        add(0); add(0); add(0); add(60);  // TTL
        rdataPos_ = buf_.size();
        add(0); add(0);            // RDLENGTH placeholder
        return *this;
    }

    /** Append the RDATA of a PTR: a dotted name written out in full. */
    Answer& ptrValue(const string& dotted)
    {
        const size_t start = buf_.size();
        size_t pos = 0;
        while (pos <= dotted.size()) {
            const size_t dot = dotted.find('.', pos);
            const string label = dotted.substr(pos, (dot == string::npos)
                                                             ? string::npos : dot - pos);
            if (!label.empty()) {
                buf_.push_back(static_cast<uint8_t>(label.size()));
                buf_.insert(buf_.end(), label.begin(), label.end());
            }
            if (dot == string::npos) break;
            pos = dot + 1;
        }
        buf_.push_back(0);
        const size_t len = buf_.size() - start;
        buf_[rdataPos_] = static_cast<uint8_t>(len >> 8);
        buf_[rdataPos_ + 1] = static_cast<uint8_t>(len & 0xFF);
        return *this;
    }

    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }

private:
    vector<uint8_t> buf_;
    size_t rdataPos_ = 0;
};

} // namespace

extern "C" {

/** The query names the address backwards, as RFC 1035 requires. */
static int test_query_bytes()
{
    const uint32_t ip = ipNet(192, 168, 1, 42);
    const auto q = PtrProbe::buildQuery(ip, 0x1234);
    TEST_ASSERT_FALSE(q.empty());

    const string bytes(reinterpret_cast<const char*>(q.data()), q.size());
    TEST_ASSERT_EQ(q[0], 0x12);            // id, big endian
    TEST_ASSERT_EQ(q[1], 0x34);
    TEST_ASSERT_EQ(q[2], 0x01);            // RD
    TEST_ASSERT_EQ(q[5], 0x01);            // QDCOUNT = 1
    TEST_ASSERT_EQ(q[6], 0x00);            // ANCOUNT = 0
    TEST_ASSERT_TRUE(bytes.find(string("\x02""42\x01""1\x03""168\x03""192")) !=
                     string::npos);
    TEST_ASSERT_TRUE(bytes.find("in-addr") != string::npos);
    TEST_ASSERT_TRUE(bytes.find("arpa") != string::npos);
    TEST_ASSERT_EQ(q[q.size() - 5], 0x00); // root label ends the name
    TEST_ASSERT_EQ(q[q.size() - 4], 0x00);
    TEST_ASSERT_EQ(q[q.size() - 3], 0x0C); // QTYPE = PTR
    TEST_ASSERT_EQ(q[q.size() - 2], 0x00);
    TEST_ASSERT_EQ(q[q.size() - 1], 0x01); // QCLASS = IN
    TEST_ASSERT_TRUE(q.size() <= PtrProbe::kMaxQueryBytes);

    // Addresses nobody can answer for are refused instead of sent as noise.
    TEST_ASSERT_TRUE(PtrProbe::buildQuery(0, 7).empty());
    TEST_ASSERT_TRUE(PtrProbe::buildQuery(0xFFFFFFFFu, 7).empty());

    // Every octet is decimal-encoded, including 0 and 255.
    const auto edge = PtrProbe::buildQuery(ipNet(0, 255, 10, 1), 7);
    const string edgeBytes(reinterpret_cast<const char*>(edge.data()), edge.size());
    TEST_ASSERT_TRUE(edgeBytes.find(string("\x01""1\x02""10\x03""255\x01""0")) !=
                     string::npos);
    return 0;
}

/** A normal answer: the PTR value points back into the question. */
static int test_answer_with_pointer()
{
    Answer a(0x1234);
    a.question();
    a.ownerPointer();            // "42.1.168.192.in-addr.arpa" via 0xC00C
    a.ptrRecord(12);
    a.ptrValue("office-pc.lan");
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(a.data(), a.size(), 0x1234),
                       "office-pc.lan");

    // The owner spelled out instead of a pointer, and the value compressed too.
    Answer b(0x0001);
    b.question();
    b.ownerFull();
    b.ptrRecord(12);
    b.ptrValue("nas.lan");
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(b.data(), b.size(), 0x0001), "nas.lan");
    return 0;
}

/** Answers that are not answers to this question are ignored. */
static int test_wrong_answers()
{
    Answer good(0x1234);
    good.question();
    good.ownerPointer();
    good.ptrRecord(12);
    good.ptrValue("pc.lan");
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(good.data(), good.size(), 0x1234), "pc.lan");
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(good.data(), good.size(), 0x9999), "");

    // A query coming back (QR clear), an error rcode, and no answers at all.
    Answer queryBack(0x1234, false);
    queryBack.question();
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(queryBack.data(), queryBack.size(), 0x1234), "");

    Answer nxdomain(0x1234, true, 3);
    nxdomain.question();
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(nxdomain.data(), nxdomain.size(), 0x1234), "");

    Answer empty(0x1234, true, 0, 0);
    empty.question();
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(empty.data(), empty.size(), 0x1234), "");

    // An answer of another type (A for the same name) is not a name.
    Answer anA(0x1234);
    anA.question();
    anA.ownerPointer();
    anA.ptrRecord(1);            // TYPE = A
    anA.ptrValue("1.2.3.4");
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(anA.data(), anA.size(), 0x1234), "");
    return 0;
}

/** Damaged datagrams are refused, never walked off the end. */
static int test_damage()
{
    Answer good(0x1234);
    good.question();
    good.ownerPointer();
    good.ptrRecord(12);
    good.ptrValue("pc.lan");

    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(nullptr, 0, 0x1234), "");
    // Shorter than a DNS header.
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(good.data(), 5, 0x1234), "");
    // Truncated in the middle of the answer: the RDATA claims more than is there.
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(good.data(), good.size() - 3, 0x1234), "");
    // Truncated right after the owner pointer.
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(good.data(), 14, 0x1234), "");

    // A pointer that points forward (into data that is not there yet) is refused.
    Answer forward(0x1234);
    forward.question();
    forward.add(0xC0); forward.add(0x60);   // -> offset 96, past the end
    forward.ptrRecord(12);
    forward.ptrValue("pc.lan");
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(forward.data(), forward.size(), 0x1234), "");
    return 0;
}

/** A pointer loop must terminate with an empty answer, not hang. */
static int test_pointer_loop()
{
    Answer a(0x1234);
    a.question();
    // Owner name points at offset 12 (the question) — fine. The PTR value then
    // points at itself: following it would loop forever if the jump budget did
    // not stop it.
    a.ownerPointer(12);
    a.ptrRecord(12);
    const size_t rdata = a.size() + 2;      // first byte of the RDATA
    a.add(0xC0); a.add(static_cast<int>(rdata));   // points at itself

    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(a.data(), a.size(), 0x1234), "");

    // A chain of pointers that never reaches a label: also refused.
    Answer chain(0x1234);
    chain.question();
    chain.ownerPointer(12);
    chain.ptrRecord(12);
    const size_t first = chain.size() + 2;
    chain.add(0xC0); chain.add(static_cast<int>(first + 2));   // -> second
    chain.add(0xC0); chain.add(static_cast<int>(first));       // -> first (loop)
    TEST_ASSERT_STR_EQ(PtrProbe::parseResponse(chain.data(), chain.size(), 0x1234), "");
    return 0;
}

void app_main()
{
    printf("Running PtrProbe tests...\n");
    int failures = 0;

    failures += test_query_bytes();
    failures += test_answer_with_pointer();
    failures += test_wrong_answers();
    failures += test_damage();
    failures += test_pointer_loop();

    if (failures == 0) {
        printf("All PtrProbe tests PASSED!\n");
    } else {
        printf("Some PtrProbe tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#else   // !DHCP_TEST_HOST

#include <cstdio>

extern "C" void app_main()
{
    printf("test_ptrprobe is a host-only test (build with -DDHCP_TEST_HOST)"
           " - see the file header.\n");
}

#endif  // DHCP_TEST_HOST
