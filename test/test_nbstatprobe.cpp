/**
 * @file test_nbstatprobe.cpp
 * @brief Unit tests for the NetBIOS node-status probe: the wildcard query and
 *        the name table it answers with.
 *
 * The name table is where the interesting cases live: a group entry must never
 * be mistaken for a computer, the workstation name wins over the server name,
 * names are padded to 15 characters with blanks, and a table that claims more
 * entries than the datagram holds must be refused instead of read past its end.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -I. \
 *       test/test_nbstatprobe.cpp src/dhcp/NbstatProbe.cpp src/dhcp/DnsMessage.cpp \
 *       host_main.cpp -o test_nbstatprobe
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../src/dhcp/NbstatProbe.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (string(a) != string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, string(a).c_str(), string(b).c_str()); return 1; } } while(0)

using dhcp::dhcp::NbstatProbe;

namespace {

/** One name-table entry: 15 blank-padded bytes, a suffix, two flag bytes. */
struct Entry {
    string name;
    uint8_t suffix = 0x00;
    uint16_t flags = 0x0000;
};

/** A node-status answer, built the way a Windows host builds it. */
class Answer {
public:
    explicit Answer(uint16_t id, bool response = true, uint8_t rcode = 0)
    {
        add(id >> 8); add(id & 0xFF);
        add(response ? 0x84 : 0x00);   // QR + AA (or a query back)
        add(rcode);
        add(0); add(1);                // QDCOUNT
        add(0); add(1);                // ANCOUNT
        add(0); add(0);                // NSCOUNT
        add(0); add(0);                // ARCOUNT
    }

    Answer& add(int byte)
    {
        buf_.push_back(static_cast<uint8_t>(byte));
        return *this;
    }

    /** The question, as the device sends it: label 0x20 + 32 letters + root. */
    Answer& question()
    {
        add(0x20);
        add('C'); add('K');                     // '*' = 0x2A
        for (int i = 0; i < 15; i++) { add('A'); add('A'); }
        add(0x00);
        add(0x00); add(0x21);                   // QTYPE = NBSTAT
        add(0x00); add(0x01);                   // QCLASS = IN
        return *this;
    }

    /** The answer record: owner pointer, type, class, TTL, RDLENGTH, table. */
    Answer& record(const vector<Entry>& entries, uint16_t rdLengthOverride = 0xFFFF)
    {
        add(0xC0); add(0x0C);                   // owner = the question name
        add(0x00); add(0x21);                   // TYPE = NBSTAT
        add(0x00); add(0x01);                   // CLASS = IN
        add(0x00); add(0x00); add(0x03); add(0xE8);   // TTL = 1000 s
        const size_t rdata = static_cast<size_t>(entries.size()) * NbstatProbe::kEntryBytes + 1;
        const uint16_t declared = (rdLengthOverride == 0xFFFF)
                                      ? static_cast<uint16_t>(rdata) : rdLengthOverride;
        add(declared >> 8); add(declared & 0xFF);
        add(static_cast<int>(entries.size()));
        for (const Entry& e : entries) {
            for (size_t i = 0; i < 15; i++) {
                add(i < e.name.size() ? static_cast<unsigned char>(e.name[i]) : ' ');
            }
            add(e.suffix);
            add(e.flags >> 8); add(e.flags & 0xFF);
        }
        return *this;
    }

    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }

private:
    vector<uint8_t> buf_;
};

} // namespace

extern "C" {

/** The query is the wildcard name in NetBIOS encoding, NBSTAT, class IN. */
static int test_query_bytes()
{
    const auto q = NbstatProbe::buildQuery(0x2B1C);
    TEST_ASSERT_FALSE(q.empty());
    TEST_ASSERT_EQ(q.size(), 50u);
    TEST_ASSERT_EQ(q[0], 0x2B);              // id
    TEST_ASSERT_EQ(q[1], 0x1C);
    TEST_ASSERT_EQ(q[2], 0x00);              // flags: a plain question
    TEST_ASSERT_EQ(q[5], 0x01);              // QDCOUNT = 1
    TEST_ASSERT_EQ(q[11], 0x00);             // ARCOUNT = 0

    TEST_ASSERT_EQ(q[12], 0x20);             // label length = 32 encoded bytes
    TEST_ASSERT_EQ(q[13], 'C');              // '*' → high nibble 2
    TEST_ASSERT_EQ(q[14], 'K');              //        low nibble A
    for (int i = 0; i < 15; i++) {           // the rest is NUL → "AA"
        TEST_ASSERT_EQ(q[15 + i * 2], 'A');
        TEST_ASSERT_EQ(q[16 + i * 2], 'A');
    }
    TEST_ASSERT_EQ(q[45], 0x00);             // root label ends the name
    TEST_ASSERT_EQ(q[46], 0x00);             // QTYPE = NBSTAT
    TEST_ASSERT_EQ(q[47], 0x21);
    TEST_ASSERT_EQ(q[48], 0x00);             // QCLASS = IN
    TEST_ASSERT_EQ(q[49], 0x01);

    // A different id lands only in the first two bytes.
    const auto other = NbstatProbe::buildQuery(1);
    TEST_ASSERT_EQ(other[0], 0x00);
    TEST_ASSERT_EQ(other[1], 0x01);
    TEST_ASSERT_EQ(other.size(), q.size());
    return 0;
}

/** A normal answer: the workstation name (suffix 0x00) wins. */
static int test_normal_answer()
{
    Answer a(0x2B1C);
    a.question();
    a.record({ { "OFFICE-PC", 0x00, 0x0000 },
               { "OFFICE-PC", 0x20, 0x0000 },     // server service, same name
               { "WORKGROUP", 0x00, 0x8000 } });  // a group
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(a.data(), a.size(), 0x2B1C), "OFFICE-PC");

    // The workstation entry listed second, and the name padded with blanks.
    Answer b(0x0001);
    b.question();
    b.record({ { "WORKGROUP", 0x00, 0x8000 },
               { "__MSBROWSE__", 0x01, 0x8000 },
               { "PC-2", 0x03, 0x0000 },          // messenger service only
               { "PC-2", 0x20, 0x0000 } });
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(b.data(), b.size(), 0x0001), "PC-2");
    return 0;
}

/** Group entries are never a computer name, and the case is left alone. */
static int test_groups_and_case()
{
    Answer onlyGroups(0x0001);
    onlyGroups.question();
    onlyGroups.record({ { "WORKGROUP", 0x00, 0x8000 },
                        { "__MSBROWSE__", 0x01, 0x8000 } });
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(onlyGroups.data(), onlyGroups.size(), 0x0001), "");

    // NetBIOS names are upper case by protocol: the probe reports what the
    // computer said and does not invent a lower-case spelling.
    Answer upper(0x0002);
    upper.question();
    upper.record({ { "OFFICE-PC", 0x00, 0x0000 } });
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(upper.data(), upper.size(), 0x0002), "OFFICE-PC");

    // An empty table (count = 0) is an answer without a name.
    Answer empty(0x0003);
    empty.question();
    empty.record({});
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(empty.data(), empty.size(), 0x0003), "");
    return 0;
}

/** Answers that are not answers to this question are ignored. */
static int test_wrong_answers()
{
    Answer good(0x0100);
    good.question();
    good.record({ { "PC-1", 0x00, 0x0000 } });
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(good.data(), good.size(), 0x0100), "PC-1");
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(good.data(), good.size(), 0x0101), "");
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(nullptr, 0, 0x0100), "");
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(good.data(), 8, 0x0100), "");   // short header

    Answer queryBack(0x0100, false);
    queryBack.question();
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(queryBack.data(), queryBack.size(), 0x0100), "");

    Answer error(0x0100, true, 3);        // NXDOMAIN
    error.question();
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(error.data(), error.size(), 0x0100), "");

    Answer noAnswers(0x0100);
    noAnswers.question();
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(noAnswers.data(), noAnswers.size(), 0x0100), "");
    return 0;
}

/** A table that claims more than the datagram holds is refused, not walked. */
static int test_damage()
{
    Answer good(0x0100);
    good.question();
    good.record({ { "PC-1", 0x00, 0x0000 } });

    // Cut in the middle of the only entry.
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(good.data(), good.size() - 5, 0x0100), "");

    // The record declares more data than there is.
    Answer shortRecord(0x0100);
    shortRecord.question();
    shortRecord.record({ { "PC-1", 0x00, 0x0000 } }, 200);
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(shortRecord.data(), shortRecord.size(), 0x0100), "");

    // RDLENGTH = 0 means no count byte at all.
    Answer noCount(0x0100);
    noCount.question();
    noCount.record({ { "PC-1", 0x00, 0x0000 } }, 0);
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(noCount.data(), noCount.size(), 0x0100), "");
    return 0;
}

/** A name of exactly 15 characters is not truncated. */
static int test_full_length_name()
{
    Answer a(0x0100);
    a.question();
    a.record({ { "ABCDEFGHIJKLMNO", 0x00, 0x0000 } });   // 15 characters
    TEST_ASSERT_STR_EQ(NbstatProbe::parseResponse(a.data(), a.size(), 0x0100), "ABCDEFGHIJKLMNO");
    return 0;
}

void app_main()
{
    printf("Running NbstatProbe tests...\n");
    int failures = 0;

    failures += test_query_bytes();
    failures += test_normal_answer();
    failures += test_groups_and_case();
    failures += test_wrong_answers();
    failures += test_damage();
    failures += test_full_length_name();

    if (failures == 0) {
        printf("All NbstatProbe tests PASSED!\n");
    } else {
        printf("Some NbstatProbe tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#else   // !DHCP_TEST_HOST

#include <cstdio>

extern "C" void app_main()
{
    printf("test_nbstatprobe is a host-only test (build with -DDHCP_TEST_HOST)"
           " - see the file header.\n");
}

#endif  // DHCP_TEST_HOST
