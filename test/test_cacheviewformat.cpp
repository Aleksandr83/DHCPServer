/**
 * @file test_cacheviewformat.cpp
 * @brief Unit tests for the cache view: which record the filter asks for, and
 *        what the page receives (stage 209).
 *
 * The operator asked for a look into the PSRAM cache — a page of its own, opened
 * from the Internal Cache page, with a name filter, a type filter and a pager that
 * can walk to any page. Two halves of that are decided **before** a byte leaves
 * the device, and both are here: which records a query matches (`CacheEntryView.h`,
 * no ESP-IDF, so a host can hand it the names it wants) and the answer the page
 * parses (`CacheViewJson.h`).
 *
 * Three decisions are worth spelling out, because the tests pin them:
 *  - an empty name filter matches every record, and the filter is a **substring**
 *    of a lower-case name (the cache stores names lower-cased, so one lowering of
 *    the filter is enough and the walk over thousands of records copies nothing);
 *  - a limit of `0` means "the page did not say" and becomes the default page
 *    size, while an absurd one is capped — a page is what keeps a 59 000-record
 *    cache out of one JSON answer;
 *  - the envelope carries the page number and the page count, because the pager
 *    the operator asked for ("1,2,3 … 99") cannot be drawn from the records alone.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_cacheviewformat.cpp -o test_cacheviewformat
 */
#include "src/dns/CacheEntryView.h"
#include "src/web/CacheViewJson.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace std;

using dhcp::dns::cacheEntryMatches;
using dhcp::dns::CacheEntryPage;
using dhcp::dns::CacheEntryQuery;
using dhcp::dns::CacheEntryView;
using dhcp::dns::cacheEntryTypeFromText;
using dhcp::dns::cacheEntryTypeText;
using dhcp::dns::cacheViewClampLimit;
using dhcp::dns::cacheViewLowerName;
using dhcp::dns::cacheViewPageCount;
using dhcp::dns::cacheViewPageNumber;
using dhcp::dns::kCacheTypeA;
using dhcp::dns::kCacheTypeAAAA;
using dhcp::web::cacheEntryPageJson;

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

static void checkEq(const string& got, const string& want, const string& what)
{
    ++g_checks;
    if (got == want) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n    got:  %s\n    want: %s\n", what.c_str(),
               got.c_str(), want.c_str());
        ++g_failed;
    }
}

namespace {

CacheEntryQuery query(const string& name, uint16_t qtype, size_t offset = 0,
                      size_t limit = dhcp::dns::kCacheViewPageSize)
{
    CacheEntryQuery q;
    q.name = cacheViewLowerName(name);
    q.qtype = qtype;
    q.offset = offset;
    q.limit = limit;
    return q;
}

/** A wire-order IPv4 address as the cache holds it: the four bytes in memory order. */
uint32_t wire4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint32_t value = 0;
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&value);
    bytes[0] = a;
    bytes[1] = b;
    bytes[2] = c;
    bytes[3] = d;
    return value;
}

CacheEntryView aRecord(const string& name, uint16_t qtype, uint32_t ttl,
                       uint32_t remaining, uint32_t age, uint32_t uses,
                       bool expired = false)
{
    CacheEntryView e;
    e.name = name;
    e.qtype = qtype;
    e.ttl = ttl;
    e.remaining = remaining;
    e.age = age;
    e.uses = uses;
    e.expired = expired;
    return e;
}

} // namespace

static void test_the_type_filter_of_a_query()
{
    uint16_t type = 0;

    check(cacheEntryTypeFromText("A", type) && type == kCacheTypeA,
          "`A` is the type the cache stores for an IPv4 answer");
    check(cacheEntryTypeFromText("a", type) && type == kCacheTypeA,
          "the case of the filter is not the operator's business");
    check(cacheEntryTypeFromText("AAAA", type) && type == kCacheTypeAAAA,
          "and `AAAA` is the other one");
    check(cacheEntryTypeFromText(" A ", type) && type == kCacheTypeA,
          "a filter typed with spaces around it still means the type");
    check(!cacheEntryTypeFromText("", type),
          "an empty filter is not a type — it means both, and the caller must see "
          "the difference instead of a zero that looks like a type");
    check(!cacheEntryTypeFromText("MX", type),
          "a type the cache cannot hold is not silently read as one of the two");
    check(!cacheEntryTypeFromText("A A", type), "and neither is a word that starts like one");

    checkEq(cacheEntryTypeText(kCacheTypeA), "A", "a stored type is named for the page");
    checkEq(cacheEntryTypeText(kCacheTypeAAAA), "AAAA", "both of them");
    checkEq(cacheEntryTypeText(33), "type 33",
            "and a type the cache should not hold is still printed as a number "
            "rather than as one of the two names");
}

static void test_which_record_the_filter_asks_for()
{
    check(cacheEntryMatches("example.com", kCacheTypeA, query("", 0)),
          "an empty filter matches a record — the page without a filter is the cache");
    check(cacheEntryMatches("example.com", kCacheTypeA, query("example", 0)),
          "a filter matches the beginning of a name");
    check(cacheEntryMatches("www.example.com", kCacheTypeA, query("ample.co", 0)),
          "and the middle of one, which is what a substring search means");
    check(cacheEntryMatches("example.com", kCacheTypeA, query("EXAMPLE.COM", 0)),
          "the filter is lower-cased like the names the cache stores");
    check(!cacheEntryMatches("example.com", kCacheTypeA, query("example.org", 0)),
          "a name that does not hold the filter is not returned");
    check(!cacheEntryMatches("example.com", kCacheTypeA, query("example.net", 0)),
          "nor is one that differs at the end");

    check(cacheEntryMatches("example.com", kCacheTypeA, query("", kCacheTypeA)),
          "the type filter A selects an A record");
    check(!cacheEntryMatches("example.com", kCacheTypeAAAA, query("", kCacheTypeA)),
          "and leaves an AAAA record of the same name out — they are two records");
    check(cacheEntryMatches("example.com", kCacheTypeAAAA, query("", kCacheTypeAAAA)),
          "the type filter AAAA selects the other one");
    check(cacheEntryMatches("example.com", kCacheTypeA, query("exam", kCacheTypeA)),
          "both filters together have to hold");
    check(!cacheEntryMatches("example.com", kCacheTypeA, query("exam", kCacheTypeAAAA)),
          "and disagreeing about the type returns nothing");
}

static void test_lowering_a_filter_once()
{
    checkEq(cacheViewLowerName("Example.COM"), "example.com",
            "a filter is lower-cased once, before the walk, not per record");
    checkEq(cacheViewLowerName(""), "", "an empty filter stays empty");
    checkEq(cacheViewLowerName("xn--80ak6aa92e.com"), "xn--80ak6aa92e.com",
            "a punycode name has nothing to lower");
    checkEq(cacheViewLowerName("a\xC3\xA9.com"), "a\xC3\xA9.com",
            "a byte above ASCII is left alone rather than mangled");
}

static void test_a_page_size()
{
    check(cacheViewClampLimit(0) == dhcp::dns::kCacheViewPageSize,
          "a limit nobody set (0) is the default page, not an empty page");
    check(cacheViewClampLimit(1) == 1, "the smallest page is one record");
    check(cacheViewClampLimit(dhcp::dns::kCacheViewPageSize) == dhcp::dns::kCacheViewPageSize,
          "the default page is a page the cache will serve as asked");
    check(cacheViewClampLimit(dhcp::dns::kCacheViewMaxPageSize + 1) ==
              dhcp::dns::kCacheViewMaxPageSize,
          "a page above the ceiling is capped rather than refused");
    check(cacheViewClampLimit(dhcp::dns::kCacheViewPageSize + 1) ==
              dhcp::dns::kCacheViewPageSize + 1,
          "a page between the default and the ceiling is served as asked");
    check(cacheViewClampLimit(10000) == dhcp::dns::kCacheViewMaxPageSize,
          "however absurd the request, the cache answers with a bounded page");
    check(cacheViewClampLimit(static_cast<size_t>(-1)) == dhcp::dns::kCacheViewMaxPageSize,
          "a value that came out of a negative number cannot ask for the world");
}

static void test_the_numbers_a_pager_needs()
{
    check(cacheViewPageCount(0, 100) == 1, "an empty cache is one empty page, not zero pages");
    check(cacheViewPageCount(1, 100) == 1, "a single record is the first page");
    check(cacheViewPageCount(100, 100) == 1, "a full page is still one page");
    check(cacheViewPageCount(101, 100) == 2, "one record more starts a second page");
    check(cacheViewPageCount(9900, 100) == 99, "and the page count is what the pager lists");
    check(cacheViewPageCount(10, 0) == 1,
          "a limit of nothing cannot divide the count — the answer is one page");

    check(cacheViewPageNumber(0, 100) == 1, "the first record is on the first page");
    check(cacheViewPageNumber(100, 100) == 2, "the record after a full page is on the second");
    check(cacheViewPageNumber(250, 100) == 3,
          "an offset that is not a multiple of the page lands on the page that holds it");
    check(cacheViewPageNumber(0, 0) == 1, "and a limit of nothing cannot divide the offset");
}

static void test_the_answer_the_page_parses()
{
    CacheEntryPage page;
    page.available = true;
    page.total = 101;
    page.offset = 100;
    page.limit = 100;
    page.scanned = 3421;

    CacheEntryView a = aRecord("example.com", kCacheTypeA, 300, 120, 180, 7);
    a.a4Count = 2;
    // The cache stores what `inet_pton` wrote: the address's four bytes in memory
    // order, which is why the expected JSON below holds the order that was set.
    a.a4[0] = wire4(1, 2, 3, 4);
    a.a4[1] = wire4(192, 168, 1, 100);
    page.items.push_back(a);

    CacheEntryView aaaa = aRecord("example.com", kCacheTypeAAAA, 60, 0, 60, 1, true);
    aaaa.a6Count = 1;
    const uint8_t v6[16] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) aaaa.a6[0][i] = v6[i];
    page.items.push_back(aaaa);

    checkEq(cacheEntryPageJson(page),
            "{\"available\":true,\"total\":101,\"offset\":100,\"limit\":100,"
            "\"page\":2,\"pages\":2,\"scanned\":3421,\"items\":["
            "{\"name\":\"example.com\",\"type\":\"A\",\"a4\":[\"1.2.3.4\",\"192.168.1.100\"],"
            "\"a6\":[],\"ttl\":300,\"remaining\":120,\"age\":180,\"uses\":7,"
            "\"expired\":false},"
            "{\"name\":\"example.com\",\"type\":\"AAAA\",\"a4\":[],"
            "\"a6\":[\"fe80::1\"],\"ttl\":60,\"remaining\":0,\"age\":60,\"uses\":1,"
            "\"expired\":true}]}",
            "the page receives the records of this page, both families of address, "
            "the age, the time left, the use count and the page's own numbers");
}

static void test_an_unavailable_cache_and_an_empty_one()
{
    CacheEntryPage gone;
    checkEq(cacheEntryPageJson(gone),
            "{\"available\":false,\"total\":0,\"offset\":0,\"limit\":0,\"page\":1,"
            "\"pages\":1,\"scanned\":0,\"items\":[]}",
            "a device without PSRAM says so in one field instead of answering a "
            "page of nothing, which the page would read as an empty cache");

    CacheEntryPage empty;
    empty.available = true;
    empty.limit = dhcp::dns::kCacheViewPageSize;
    checkEq(cacheEntryPageJson(empty),
            "{\"available\":true,\"total\":0,\"offset\":0,\"limit\":100,\"page\":1,"
            "\"pages\":1,\"scanned\":0,\"items\":[]}",
            "and an enabled but empty cache is one empty page the page can draw");
}

static void test_a_name_that_would_break_the_json()
{
    CacheEntryPage page;
    page.available = true;
    page.total = 1;
    page.limit = 100;
    page.items.push_back(aRecord("we\"ird\\name.com", kCacheTypeA, 1, 1, 0, 0));

    const string json = cacheEntryPageJson(page);
    check(json.find("\"name\":\"we\\\"ird\\\\name.com\"") != string::npos,
          "a quote or a backslash in a name is escaped — an unescaped one makes "
          "the whole answer invalid JSON and the page shows nothing at all");
}

int main()
{
    printf("Running CacheViewFormat tests...\n");
    test_the_type_filter_of_a_query();
    test_which_record_the_filter_asks_for();
    test_lowering_a_filter_once();
    test_a_page_size();
    test_the_numbers_a_pager_needs();
    test_the_answer_the_page_parses();
    test_an_unavailable_cache_and_an_empty_one();
    test_a_name_that_would_break_the_json();

    if (g_failed == 0) {
        printf("All %d CacheViewFormat checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d CacheViewFormat checks FAILED!\n", g_failed, g_checks);
    return 1;
}
