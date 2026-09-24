/**
 * @file test_cachefilereader.cpp
 * @brief Unit tests for the reader of `cache.dat` and for the structural
 *        read-back check a planned restart runs (stage 169).
 *
 * The check the operator asked for is "read the file back and tell me whether it
 * holds what was written". `CacheFileReader` is that check, and it is also the
 * reader the boot-time restore uses — one parser, so a file the check accepts is
 * a file the restore can read. It is free of ESP-IDF and of lwIP (addresses
 * travel as raw bytes), which is what lets the format be tested here on files
 * assembled **by hand**: a builder shared with the reader would agree with it
 * about a wrong layout.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_cachefilereader.cpp src/dns/CacheFileReader.cpp \
 *       -o test_cachefilereader
 */
#include "src/dns/CacheFileReader.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace std;

using dhcp::dns::CacheFileCheck;
using dhcp::dns::CacheFileRead;
using dhcp::dns::CacheFileReader;
using dhcp::dns::CacheFileRecord;
using dhcp::dns::CacheFileStatus;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const string& what)
{
    ++g_checks;
    if (ok) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        ++g_failed;
    }
}

// ─── Building the bytes of a cache file by hand ───────────────────────────────

static void putU16(string& s, uint16_t v)
{
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}

static void putU32(string& s, uint32_t v)
{
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

static void putU64(string& s, uint64_t v)
{
    for (int i = 0; i < 8; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

static string ipv4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    string s;
    s.push_back(static_cast<char>(a));
    s.push_back(static_cast<char>(b));
    s.push_back(static_cast<char>(c));
    s.push_back(static_cast<char>(d));
    return s;
}

static string ipv6(unsigned last)
{
    string s(16, '\0');
    s[15] = static_cast<char>(last);
    return s;
}

static string header(uint32_t version, uint32_t count)
{
    string h;
    h.append("DCC1", 4);
    putU32(h, version);
    putU32(h, count);
    putU32(h, 0);
    return h;
}

/** One record, laid out the way the writer lays it out for @p version. */
static string record(const string& name, uint16_t qtype, uint32_t ttl, uint64_t uses,
                     const vector<string>& addrs, uint32_t version)
{
    string r;
    r.push_back(static_cast<char>(name.size()));
    r.append(name);
    putU16(r, qtype);
    const bool isA = (qtype == CacheFileReader::kTypeA);
    r.push_back(static_cast<char>(isA ? addrs.size() : 0));
    r.push_back(static_cast<char>(isA ? 0 : addrs.size()));
    putU32(r, ttl);
    if (version >= CacheFileReader::kVersionUses64) {
        if (version == CacheFileReader::kVersionUses64) {
            putU64(r, uses);
        } else {
            putU32(r, static_cast<uint32_t>(uses));
        }
    }
    for (const string& a : addrs) r.append(a);
    return r;
}

static const char* kPath = "test_cachefile.dat";
static const char* kOther = "test_cachefile_other.dat";

static void writeFile(const char* path, const string& bytes)
{
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        check(false, string("the test file can be created: ") + path);
        return;
    }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

/** Collects what the reader hands over, so the records themselves are checked. */
struct Collected {
    vector<CacheFileRecord> records;
    vector<uint32_t> indexes;
    vector<uint32_t> counts;

    CacheFileReader::RecordHandler handler()
    {
        return [this](const CacheFileRecord& rec, uint32_t index, uint32_t count) {
            records.push_back(rec);
            indexes.push_back(index);
            counts.push_back(count);
        };
    }
};

/** A file of two whole records is read to the end: records, indexes, sizes. */
static void test_reads_a_good_file()
{
    printf("a whole file is read to the end\n");
    string bytes = header(CacheFileReader::kVersion, 2);
    bytes += record("example.com", CacheFileReader::kTypeA, 60, 7,
                    {ipv4(93, 184, 216, 34)}, CacheFileReader::kVersion);
    bytes += record("example.net", CacheFileReader::kTypeAaaa, 300, 9,
                    {ipv6(1), ipv6(2)}, CacheFileReader::kVersion);
    writeFile(kPath, bytes);

    Collected got;
    const CacheFileRead read = CacheFileReader::read(kPath, got.handler());
    check(read.status == CacheFileStatus::Ok,
          string("the file reads: ") + CacheFileReader::statusName(read.status) +
              " (" + read.why + ")");
    check(read.version == CacheFileReader::kVersion, "the version comes from the header");
    check(read.headerCount == 2 && read.records == 2, "two records were promised and two read");
    check(read.sizeBytes == bytes.size(),
          "and the size is the file's (" + to_string(read.sizeBytes) + ")");

    check(got.records.size() == 2, "the handler saw both records");
    if (got.records.size() == 2) {
        check(got.records[0].name == "example.com" && got.records[1].name == "example.net",
              "with their names");
        check(got.records[0].qtype == 1 && got.records[1].qtype == 28, "and query types");
        check(got.records[0].ttlRemaining == 60 && got.records[1].ttlRemaining == 300,
              "and the remaining TTLs");
        check(got.records[0].uses == 7 && got.records[1].uses == 9, "and usage counters");
        check(got.records[0].addrs.size() == 1 && got.records[0].addrs[0] == ipv4(93, 184, 216, 34),
              "the IPv4 address is the four bytes the file holds");
        check(got.records[1].addrs.size() == 2 && got.records[1].addrs[1] == ipv6(2),
              "and an IPv6 record carries its 16-byte addresses");
    }
    check(got.indexes.size() == 2 && got.indexes[0] == 0 && got.indexes[1] == 1,
          "the handler is told the position of each record");
    check(got.counts.size() == 2 && got.counts[0] == 2, "and how many there are in all");

    const CacheFileCheck pass = CacheFileReader::check(kPath, 2);
    check(pass.ok && pass.records == 2, "the restart check passes on it");

    const CacheFileCheck fewer = CacheFileReader::check(kPath, 3);
    check(!fewer.ok, "and fails when the save reported one record more");
    check(fewer.why.find("2 records") != string::npos &&
              fewer.why.find("3 were written") != string::npos,
          "naming both numbers for the operator: " + fewer.why);

    const CacheFileCheck more = CacheFileReader::check(kPath, 1);
    check(!more.ok, "and when it reported one fewer");

    remove(kPath);
}

/** A file that ends inside a record is a torn write, not a small cache. */
static void test_a_file_cut_short()
{
    printf("a file that ends inside a record\n");
    string bytes = header(CacheFileReader::kVersion, 2);
    bytes += record("example.com", CacheFileReader::kTypeA, 60, 1,
                    {ipv4(10, 0, 0, 1)}, CacheFileReader::kVersion);
    const string second = record("example.net", CacheFileReader::kTypeA, 60, 1,
                                 {ipv4(10, 0, 0, 2)}, CacheFileReader::kVersion);
    bytes += second.substr(0, second.size() - 2);   // the addresses are cut off
    writeFile(kPath, bytes);

    Collected got;
    const CacheFileRead read = CacheFileReader::read(kPath, got.handler());
    check(read.status == CacheFileStatus::Truncated,
          string("the reader says truncated: ") + CacheFileReader::statusName(read.status));
    check(read.records == 1, "the whole record before the cut is still reported");
    check(got.records.size() == 1, "and it is the only one handed over");
    check(read.why.find("record 2 of 2") != string::npos,
          "the reason names the record that was cut: " + read.why);

    const CacheFileCheck fail = CacheFileReader::check(kPath, 2);
    check(!fail.ok, "the restart check refuses it");
    check(fail.why == read.why, "with the same words the reader gave");

    remove(kPath);
}

/** Bytes after the last promised record mean the header and the body disagree. */
static void test_trailing_bytes()
{
    printf("bytes after the last record\n");
    string bytes = header(CacheFileReader::kVersion, 1);
    bytes += record("example.com", CacheFileReader::kTypeA, 60, 1,
                    {ipv4(10, 0, 0, 1)}, CacheFileReader::kVersion);
    bytes += "xyz";
    writeFile(kPath, bytes);

    const CacheFileRead read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::TrailingBytes,
          string("the reader says trailing bytes: ") + CacheFileReader::statusName(read.status));
    check(read.records == 1, "after reading the one record the header promised");
    check(!CacheFileReader::check(kPath, 1).ok, "and the restart check does not pass it");

    remove(kPath);
}

/** The header is checked before a single record is read. */
static void test_header_is_checked_first()
{
    printf("the header\n");

    writeFile(kPath, "not a cache file at all, not even close to sixteen");
    CacheFileRead read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::BadHeader,
          string("a foreign file is refused: ") + CacheFileReader::statusName(read.status));
    check(!read.why.empty(), "with a reason: " + read.why);

    writeFile(kPath, string("DCC1") + string(4, '\0'));   // eight bytes, no room for a header
    read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::BadHeader, "so is a file shorter than the header");
    check(read.why.find("8 bytes") != string::npos,
          "and the reason names what is there: " + read.why);

    string version = header(9, 0);
    writeFile(kPath, version);
    read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::UnsupportedVersion,
          string("a version from the future is refused: ") +
              CacheFileReader::statusName(read.status));
    check(read.why.find("9") != string::npos, "naming it: " + read.why);

    string huge = header(CacheFileReader::kVersion, CacheFileReader::kMaxPlausibleRecords + 1);
    writeFile(kPath, huge);
    read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::ImplausibleCount,
          string("an impossible record count is refused: ") +
              CacheFileReader::statusName(read.status));

    remove(kPath);
}

/** Fields that cannot be true are refused instead of being handed over. */
static void test_impossible_records()
{
    printf("records that cannot be true\n");

    // A record without a name.
    string noName = header(CacheFileReader::kVersion, 1);
    noName.push_back('\0');
    noName += record("example.com", CacheFileReader::kTypeA, 60, 1,
                     {ipv4(10, 0, 0, 1)}, CacheFileReader::kVersion);
    writeFile(kPath, noName);
    CacheFileRead read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::BadRecord,
          string("a zero-length name is refused: ") + CacheFileReader::statusName(read.status));

    // A query type that is neither A nor AAAA.
    string wrongType = header(CacheFileReader::kVersion, 1);
    wrongType += record("example.com", 15, 60, 1, {ipv4(10, 0, 0, 1)},
                        CacheFileReader::kVersion);
    writeFile(kPath, wrongType);
    read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::BadRecord, "so is a record of type 15");
    check(read.why.find("15") != string::npos, "with the type in the reason: " + read.why);

    // An A record with no address at all: the restore would have nothing to
    // insert, and the writer never produces one.
    string noAddrs = header(CacheFileReader::kVersion, 1);
    noAddrs += record("example.com", CacheFileReader::kTypeA, 60, 1, {},
                      CacheFileReader::kVersion);
    writeFile(kPath, noAddrs);
    read = CacheFileReader::read(kPath, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::BadRecord, "and a record without an address");

    // One record of the file is wrong: the check must not pass on the file.
    string second = header(CacheFileReader::kVersion, 2);
    second += record("example.com", CacheFileReader::kTypeA, 60, 1,
                     {ipv4(10, 0, 0, 1)}, CacheFileReader::kVersion);
    second.push_back('\0');   // a record that starts with a zero name length
    second += record("example.net", CacheFileReader::kTypeA, 60, 1,
                     {ipv4(10, 0, 0, 2)}, CacheFileReader::kVersion);
    writeFile(kPath, second);
    const CacheFileCheck fail = CacheFileReader::check(kPath, 2);
    check(!fail.ok, "a file with one impossible record does not pass the check");
    check(fail.records == 1, "and only the whole records are counted");

    remove(kPath);
}

/** The two older versions still read: no counter, and an eight-byte one. */
static void test_older_versions()
{
    printf("version 1 and version 2 records\n");

    string v1 = header(CacheFileReader::kVersionMin, 1);
    v1 += record("old.example", CacheFileReader::kTypeA, 30, 0,
                 {ipv4(10, 1, 1, 1)}, CacheFileReader::kVersionMin);
    writeFile(kPath, v1);
    Collected got;
    CacheFileRead read = CacheFileReader::read(kPath, got.handler());
    check(read.status == CacheFileStatus::Ok && read.version == CacheFileReader::kVersionMin,
          "a version 1 file reads");
    check(got.records.size() == 1 && got.records[0].uses == 0,
          "and its records come back with no usage counter");

    const uint64_t big = 0x1FFFFFFFFull;   // larger than the field has today
    string v2 = header(CacheFileReader::kVersionUses64, 1);
    v2 += record("old.example", CacheFileReader::kTypeA, 30, big,
                 {ipv4(10, 1, 1, 1)}, CacheFileReader::kVersionUses64);
    writeFile(kPath, v2);
    got = Collected();
    read = CacheFileReader::read(kPath, got.handler());
    check(read.status == CacheFileStatus::Ok && read.version == CacheFileReader::kVersionUses64,
          "a version 2 file reads");
    check(got.records.size() == 1 && got.records[0].uses == big,
          "and its eight-byte counter arrives whole");
    check(CacheFileReader::check(kPath, 1).ok, "both pass the restart check");

    remove(kPath);
}

/** A file that is not there is the normal state of a device that never saved. */
static void test_missing_file()
{
    printf("a file that is not there\n");
    remove(kOther);
    const CacheFileRead read = CacheFileReader::read(kOther, CacheFileReader::RecordHandler());
    check(read.status == CacheFileStatus::NotFound,
          string("the reader says not found: ") + CacheFileReader::statusName(read.status));
    check(read.records == 0, "with nothing read");
    check(read.why == "the file is not there", "and a plain reason: " + read.why);

    const CacheFileCheck fail = CacheFileReader::check(kOther, 4);
    check(!fail.ok, "the restart check does not pass a missing file");
    check(fail.why == "the file is not there", "with the same reason the reader gave");

    const CacheFileCheck empty = CacheFileReader::check("", 1);
    check(!empty.ok, "and an empty path is refused as well");
}

/** The read-back pass reports where it is (stage 169), so the page can say
 *  "checking" with the read's own percentage instead of freezing at the save's
 *  last one. */
static void test_the_read_reports_its_progress()
{
    printf("the read reports its progress\n");
    const uint32_t count = 150;
    string bytes = header(CacheFileReader::kVersion, count);
    for (uint32_t i = 0; i < count; i++) {
        bytes += record("h" + to_string(i) + ".example", CacheFileReader::kTypeA, 60, 1,
                        {ipv4(10, 0, static_cast<unsigned>(i / 256),
                              static_cast<unsigned>(i % 256))},
                        CacheFileReader::kVersion);
    }
    writeFile(kPath, bytes);

    vector<uint32_t> done;
    vector<uint32_t> total;
    const CacheFileCheck pass = CacheFileReader::check(
        kPath, count, [&](uint32_t d, uint32_t t) { done.push_back(d); total.push_back(t); });

    check(pass.ok, "the file passes the check");
    check(!done.empty(), "and the pass reported progress");
    if (!done.empty()) {
        check(done.front() >= 1 && done.front() < count,
              "the first report comes before the end (" + to_string(done.front()) + ")");
        check(done.back() == count && total.back() == count,
              "and the last one is the whole file");
        bool increasing = true;
        for (size_t i = 1; i < done.size(); i++) {
            if (done[i] <= done[i - 1]) increasing = false;
        }
        check(increasing, "the reports only move forward");
    }

    remove(kPath);
}

int main()
{
    test_reads_a_good_file();
    test_a_file_cut_short();
    test_trailing_bytes();
    test_header_is_checked_first();
    test_impossible_records();
    test_older_versions();
    test_missing_file();
    test_the_read_reports_its_progress();

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
