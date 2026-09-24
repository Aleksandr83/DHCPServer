/**
 * @file test_internalcache.cpp
 * @brief Unit tests for the built-in PSRAM DNS cache (usage counter, eviction,
 *        persistence).
 *
 * The cache talks to ESP-IDF (capability allocator, microsecond timer, a
 * FreeRTOS mutex) and to lwIP (`inet_pton` / `inet_ntop`), so this test runs on
 * a **host** with the stand-ins from `test/stubs` — which is also what makes TTL
 * expiry testable without sleeping (the stub clock is a variable).
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -I test/stubs -I. \
 *       test/test_internalcache.cpp src/dns/InternalDnsCache.cpp \
 *       src/dns/CacheFileReader.cpp host_main.cpp \
 *       -o test_internalcache -lws2_32
 *
 * `host_main.cpp` must return 0 from `main()` for this file: its `app_main` is
 * `void`, so an `int`-returning shim would read an undefined value (the harness
 * keeps two shims for exactly that reason).
 *
 * Without `DHCP_TEST_HOST` the file compiles to an `app_main` that does nothing:
 * an on-device test build cannot drive the fake clock or ask for a PSRAM arena
 * of a chosen size, so running it there would test something other than what is
 * written here.
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_timer.h>   // host stub: testClockUs() moves the clock

#include "../src/dns/InternalDnsCache.h"
#include "../src/dns/CacheFileReader.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (string(a) != string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, string(a).c_str(), string(b).c_str()); return 1; } } while(0)

using dhcp::dns::InternalDnsCache;
using dhcp::dns::CacheFileReader;
using dhcp::dns::CacheFileCheck;

namespace {

void resetClock() { testClockUs() = 0; }
void advanceMs(uint64_t ms) { testClockUs() += static_cast<int64_t>(ms) * 1000; }

vector<string> ipA(const char* ip) { return vector<string>{ip}; }

} // namespace

extern "C" {

/** A stored record answers a query, and keeps the TTL it was stored with. */
static int test_basic_answer()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    TEST_ASSERT_TRUE(c.available());
    TEST_ASSERT_TRUE(c.stats().capacity > 0);

    c.store("example.com", 1, ipA("93.184.216.34"), 60);

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_TRUE(c.lookup("EXAMPLE.com", 1, out, ttl));   // case-insensitive
    TEST_ASSERT_EQ(out.size(), 1u);
    TEST_ASSERT_STR_EQ(out[0], "93.184.216.34");
    TEST_ASSERT_EQ(ttl, 60u);
    TEST_ASSERT_EQ(c.stats().entries, 1u);

    // AAAA is a different key, so it is not answered by the A record.
    TEST_ASSERT_FALSE(c.lookup("example.com", 28, out, ttl));
    return 0;
}

/** The counter grows by one per use: a store and a hit, not a miss, and a
 *  refresh keeps the old count instead of starting over. */
static int test_usage_counter()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));

    c.store("example.com", 1, ipA("93.184.216.34"), 60);
    TEST_ASSERT_EQ(c.stats().usesTotal, 1);   // the store that created it
    TEST_ASSERT_EQ(c.stats().usesMax, 1);

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_TRUE(c.lookup("example.com", 1, out, ttl));
    TEST_ASSERT_EQ(c.stats().usesTotal, 2);
    TEST_ASSERT_EQ(c.stats().usesMax, 2);

    // A miss involves no record, so there is nothing to count.
    TEST_ASSERT_FALSE(c.lookup("absent.test", 1, out, ttl));
    TEST_ASSERT_EQ(c.stats().usesTotal, 2);

    // A fresh upstream answer for the same name is one more use of that name.
    c.store("example.com", 1, ipA("93.184.216.35"), 300);
    const auto s = c.stats();
    TEST_ASSERT_EQ(s.entries, 1u);
    TEST_ASSERT_EQ(s.usesTotal, 3);
    TEST_ASSERT_EQ(s.usesMax, 3);
    TEST_ASSERT_STR_EQ(s.topName, "example.com");
    TEST_ASSERT_EQ(s.topQtype, 1);

    TEST_ASSERT_TRUE(c.lookup("example.com", 1, out, ttl));
    TEST_ASSERT_STR_EQ(out[0], "93.184.216.35");   // the refresh won
    return 0;
}

/** An entry past its TTL is a miss and a purge — never a use. */
static int test_expired_is_not_a_use()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    c.store("short.test", 1, ipA("10.3.3.3"), 10);
    TEST_ASSERT_EQ(c.stats().usesTotal, 1);

    advanceMs(11000);   // 11 s later, the 10 s TTL is over

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_FALSE(c.lookup("short.test", 1, out, ttl));

    const auto s = c.stats();
    TEST_ASSERT_EQ(s.usesTotal, 1);      // the expiry did not count
    TEST_ASSERT_EQ(s.entries, 0u);
    TEST_ASSERT_EQ(s.evicted, 1u);
    // The "hottest name" is a high-water mark: the record it names may be gone.
    TEST_ASSERT_EQ(s.usesMax, 1);
    TEST_ASSERT_STR_EQ(s.topName, "short.test");
    return 0;
}

/** Overflow takes the least used record, not the one stored earliest. */
static int test_least_used_is_evicted()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    const size_t cap = c.stats().capacity;

    // The hot name goes in FIRST on purpose: under the old "oldest record
    // first" rule it would be the very first victim.
    c.store("hot.test", 1, ipA("10.2.2.2"), 3600);
    for (size_t i = 0; i + 1 < cap; i++) {
        c.store("g" + to_string(i) + ".test", 1, ipA("10.9.9.9"), 3600);
    }
    TEST_ASSERT_EQ(c.stats().entries, cap);

    vector<string> out;
    uint32_t ttl = 0;
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(c.lookup("hot.test", 1, out, ttl));
    }
    const auto before = c.stats();
    TEST_ASSERT_EQ(before.usesMax, 11);              // 1 store + 10 hits
    TEST_ASSERT_STR_EQ(before.topName, "hot.test");

    c.store("new.test", 1, ipA("10.8.8.8"), 3600);   // the pool is full → evict

    TEST_ASSERT_TRUE(c.lookup("hot.test", 1, out, ttl));   // survived
    TEST_ASSERT_EQ(out.size(), 1u);
    TEST_ASSERT_STR_EQ(out[0], "10.2.2.2");
    const auto after = c.stats();
    TEST_ASSERT_EQ(after.entries, cap);
    TEST_ASSERT_EQ(after.evicted, 1u);
    TEST_ASSERT_EQ(after.usesMax, 12);               // the check above counted
    return 0;
}

/** A recycled node starts from zero: it must not inherit the count of the
 *  record it replaced (a name would then look used before it ever was). */
static int test_recycled_node_starts_from_zero()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    const size_t cap = c.stats().capacity;

    for (size_t i = 0; i < cap; i++) {
        c.store("f" + to_string(i) + ".test", 1, ipA("10.1.1.1"), 3600);
    }
    TEST_ASSERT_EQ(c.stats().entries, cap);
    TEST_ASSERT_EQ(c.stats().usesMax, 1);   // every record has one use exactly

    c.store("overflow.test", 1, ipA("10.7.7.7"), 3600);   // forces the eviction

    const auto s = c.stats();
    TEST_ASSERT_EQ(s.entries, cap);
    TEST_ASSERT_EQ(s.evicted, 1u);
    // With an inherited counter this would read 2 (the replaced record's use
    // plus this store) and the new name would falsely lead the statistics.
    TEST_ASSERT_EQ(s.usesMax, 1);
    return 0;
}

/**
 * "Nothing to write" is not a failed write.
 *
 * The reboot flow tells the operator what happened to the cache, and a device
 * that has just started (or whose entries have all expired) has nothing to
 * write. Telling that apart from a real failure is what the out-parameter is
 * for: reporting "the cache could not be saved" on a fresh device is a lie the
 * operator would chase for nothing.
 */
static int test_save_reports_nothing_to_save()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));

    // 1. Nothing stored at all.
    bool nothing = false;
    size_t written = 99;
    TEST_ASSERT_FALSE(c.saveToFile("test_cache_none.dat", &written, nullptr, nullptr, &nothing));
    TEST_ASSERT_TRUE(nothing);
    TEST_ASSERT_EQ(written, 0u);

    // 2. Entries stored, but every one of them expired: still nothing to write.
    c.store("gone.test", 1, ipA("10.1.1.1"), 10);
    advanceMs(11000);
    nothing = false;
    TEST_ASSERT_FALSE(c.saveToFile("test_cache_none.dat", &written, nullptr, nullptr, &nothing));
    TEST_ASSERT_TRUE(nothing);

    // 3. One live entry: the file is written and the flag stays down.
    c.store("live.test", 1, ipA("10.2.2.2"), 60);
    nothing = true;
    TEST_ASSERT_TRUE(c.saveToFile("test_cache_one.dat", &written, nullptr, nullptr, &nothing));
    TEST_ASSERT_FALSE(nothing);
    TEST_ASSERT_EQ(written, 1u);

    // 4. Without the out-parameter the call behaves exactly as before (it is
    // optional, and the older call sites still pass four arguments).
    nothing = true;
    TEST_ASSERT_TRUE(c.saveToFile("test_cache_one.dat", &written));
    TEST_ASSERT_EQ(written, 1u);

    // 5. The distinction survives the "ignore TTL" mode: with TTL expiry off the
    // expired entry above is live again, so there IS something to write.
    InternalDnsCache forever;
    TEST_ASSERT_TRUE(forever.enable(1));
    forever.setIgnoreTtl(true);
    forever.store("forever.test", 1, ipA("10.3.3.3"), 10);
    advanceMs(60000);
    nothing = true;
    TEST_ASSERT_TRUE(forever.saveToFile("test_cache_ignore.dat", &written, nullptr, nullptr, &nothing));
    TEST_ASSERT_FALSE(nothing);
    TEST_ASSERT_EQ(written, 1u);

    remove("test_cache_none.dat");
    remove("test_cache_one.dat");
    remove("test_cache_ignore.dat");
    c.disable();
    forever.disable();
    return 0;
}

/** Save/load keeps the counters: the file carries them (format version 2). */
static int test_save_load_keeps_counters()
{
    resetClock();
    const char* path = "test_cache_v2.dat";
    remove(path);

    uint64_t saved = 0;
    {
        InternalDnsCache c;
        TEST_ASSERT_TRUE(c.enable(1));
        c.store("keep.test", 1, ipA("1.2.3.4"), 3600);
        vector<string> out;
        uint32_t ttl = 0;
        for (int i = 0; i < 4; i++) {
            TEST_ASSERT_TRUE(c.lookup("keep.test", 1, out, ttl));
        }
        saved = c.stats().usesMax;
        TEST_ASSERT_EQ(saved, 5);

        size_t written = 0;
        TEST_ASSERT_TRUE(c.saveToFile(path, &written));
        TEST_ASSERT_EQ(written, 1u);
        const auto info = c.fileInfo(path);
        TEST_ASSERT_TRUE(info.exists);
        TEST_ASSERT_EQ(info.version, 3u);
        TEST_ASSERT_EQ(info.entries, 1u);
        // 16 B header + 1 + 9 ("keep.test") + 2 + 1 + 1 + 4 (ttl) + 4 (uses)
        // + 4 (one IPv4). Version 2 wrote 8 bytes for the counter: 46.
        TEST_ASSERT_EQ(info.size, 42u);
        c.disable();
    }
    {
        InternalDnsCache c;
        TEST_ASSERT_TRUE(c.enable(1));
        size_t loaded = 0;
        TEST_ASSERT_TRUE(c.loadFromFile(path, &loaded));
        TEST_ASSERT_EQ(loaded, 1u);

        vector<string> out;
        uint32_t ttl = 0;
        TEST_ASSERT_TRUE(c.lookup("keep.test", 1, out, ttl));
        TEST_ASSERT_EQ(out.size(), 1u);
        TEST_ASSERT_STR_EQ(out[0], "1.2.3.4");
        // The stored counter came back untouched, and only the lookup above
        // added one — a load must not count as a use per record.
        TEST_ASSERT_EQ(c.stats().usesMax, saved + 1);
        c.disable();
    }
    remove(path);
    return 0;
}

/** A version 2 file — the one an older build wrote, with an 8-byte usage
 *  counter — must still load: this is the file sitting on the device when the
 *  format changed, and refusing it would cost the working set for nothing. */
static int test_load_reads_version2_file()
{
    resetClock();
    const char* path = "test_cache_v2_old.dat";

    // header: magic "DCC1" | version 2 | entryCount 1 | reserved 0
    // entry:  nameLen 9, "v2.testxx", qtype 1, nA 1, nAAAA 0, ttl 600,
    //         u64 uses = 4242, IPv4 5.6.7.8
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_TRUE(f != nullptr);
    const uint8_t hdr[16] = {'D', 'C', 'C', '1', 2, 0, 0, 0,
                             1, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQ(fwrite(hdr, 1, sizeof(hdr), f), sizeof(hdr));
    const uint8_t nameLen = 9;
    TEST_ASSERT_EQ(fwrite(&nameLen, 1, 1, f), 1u);
    TEST_ASSERT_EQ(fwrite("v2.testxx", 1, nameLen, f), (size_t)nameLen);
    const uint8_t tail[8] = {1, 0, 1, 0, 88, 2, 0, 0};   // A, 1 address, ttl 600
    TEST_ASSERT_EQ(fwrite(tail, 1, sizeof(tail), f), sizeof(tail));
    const uint8_t uses64[8] = {0x92, 0x10, 0, 0, 0, 0, 0, 0};   // 4242
    TEST_ASSERT_EQ(fwrite(uses64, 1, sizeof(uses64), f), sizeof(uses64));
    const uint8_t ip[4] = {5, 6, 7, 8};
    TEST_ASSERT_EQ(fwrite(ip, 1, sizeof(ip), f), sizeof(ip));
    fclose(f);

    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    size_t loaded = 0;
    TEST_ASSERT_TRUE(c.loadFromFile(path, &loaded));
    TEST_ASSERT_EQ(loaded, 1u);
    TEST_ASSERT_EQ(c.stats().usesMax, 4242u);   // the 8 bytes were read as 8 bytes

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_TRUE(c.lookup("v2.testxx", 1, out, ttl));
    TEST_ASSERT_EQ(out.size(), 1u);
    TEST_ASSERT_STR_EQ(out[0], "5.6.7.8");

    // And what this build writes is four bytes shorter per record.
    const string again = string(path) + ".saved";
    size_t written = 0;
    TEST_ASSERT_TRUE(c.saveToFile(again.c_str(), &written));
    TEST_ASSERT_EQ(written, 1u);
    TEST_ASSERT_EQ(c.fileInfo(again.c_str()).version, 3u);
    // 16 B header + 1 + 9 (name) + 2 + 1 + 1 + 4 (ttl) + 4 (uses) + 4 (IPv4).
    TEST_ASSERT_EQ(c.fileInfo(again.c_str()).size, 42u);
    remove(again.c_str());

    remove(path);
    return 0;
}

/** Files written before the counter existed (version 1) still load. */
static int test_load_reads_version1_file()
{
    resetClock();
    const char* path = "test_cache_v1.dat";

    // header: magic "DCC1" | version 1 | entryCount 1 | reserved 0
    // entry:  nameLen 8, "old.test", qtype 1, nA 1, nAAAA 0, ttl 60, IPv4
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_TRUE(f != nullptr);
    const uint8_t hdr[16] = {'D', 'C', 'C', '1', 1, 0, 0, 0,
                             1, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQ(fwrite(hdr, 1, sizeof(hdr), f), sizeof(hdr));
    const uint8_t nameLen = 8;
    TEST_ASSERT_EQ(fwrite(&nameLen, 1, 1, f), 1u);
    TEST_ASSERT_EQ(fwrite("old.test", 1, nameLen, f), (size_t)nameLen);
    const uint8_t tail[8] = {1, 0, 1, 0, 60, 0, 0, 0};
    TEST_ASSERT_EQ(fwrite(tail, 1, sizeof(tail), f), sizeof(tail));
    const uint8_t ip[4] = {8, 8, 4, 4};
    TEST_ASSERT_EQ(fwrite(ip, 1, sizeof(ip), f), sizeof(ip));
    fclose(f);

    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    size_t loaded = 0;
    TEST_ASSERT_TRUE(c.loadFromFile(path, &loaded));
    TEST_ASSERT_EQ(loaded, 1u);
    TEST_ASSERT_EQ(c.fileInfo(path).version, 1u);

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_TRUE(c.lookup("old.test", 1, out, ttl));
    TEST_ASSERT_EQ(out.size(), 1u);
    TEST_ASSERT_STR_EQ(out[0], "8.8.4.4");

    const auto s = c.stats();
    TEST_ASSERT_EQ(s.entries, 1u);
    // No counter in the file → the record starts at zero, and only the lookup
    // above counts as a use.
    TEST_ASSERT_EQ(s.usesTotal, 1);
    TEST_ASSERT_EQ(s.usesMax, 1);
    TEST_ASSERT_STR_EQ(s.topName, "old.test");

    remove(path);
    return 0;
}

/** The usage counter never wraps through zero: a record whose counter already
 *  sits at the maximum stays there. The file read here is a **version 2** one —
 *  written by the build where the field was 64-bit — so this case also proves
 *  that an older file still loads, with its larger value clamped to the width
 *  the field has in memory. One step from the maximum is reachable — and
 *  testable — through the cache file; 2^32 increments are not. */
static int test_usage_counter_saturates_at_max()
{
    resetClock();
    const char* path = "test_cache_max.dat";

    // A version 2 file with one record whose 8-byte counter is UINT64_MAX. The
    // in-memory counter is 32-bit, so it comes back clamped to UINT32_MAX.
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_TRUE(f != nullptr);
    const uint8_t hdr[16] = {'D', 'C', 'C', '1', 2, 0, 0, 0,
                             1, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQ(fwrite(hdr, 1, sizeof(hdr), f), sizeof(hdr));
    const uint8_t nameLen = 8;                                  // "max.test"
    TEST_ASSERT_EQ(fwrite(&nameLen, 1, 1, f), 1u);
    TEST_ASSERT_EQ(fwrite("max.test", 1, nameLen, f), (size_t)nameLen);
    const uint8_t tail[8] = {1, 0, 1, 0, 60, 0, 0, 0};   // A, 1 address, ttl 60
    TEST_ASSERT_EQ(fwrite(tail, 1, sizeof(tail), f), sizeof(tail));
    const uint8_t maxUses[8] = {0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQ(fwrite(maxUses, 1, sizeof(maxUses), f), sizeof(maxUses));
    const uint8_t ip[4] = {8, 8, 8, 8};
    TEST_ASSERT_EQ(fwrite(ip, 1, sizeof(ip), f), sizeof(ip));
    fclose(f);

    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    size_t loaded = 0;
    TEST_ASSERT_TRUE(c.loadFromFile(path, &loaded));
    TEST_ASSERT_EQ(loaded, 1u);
    TEST_ASSERT_EQ(c.stats().usesMax, UINT32_MAX);          // clamped on load

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_TRUE(c.lookup("max.test", 1, out, ttl));   // one step from Max
    TEST_ASSERT_EQ(c.stats().usesMax, UINT32_MAX);          // … and it stays Max

    // Fill the pool and force one eviction. A counter that had wrapped to zero
    // would make this record look like the least used one and evict it.
    const size_t cap = c.stats().capacity;
    for (size_t i = 0; i + 1 < cap; i++) {
        c.store("m" + to_string(i) + ".test", 1, ipA("10.4.4.4"), 3600);
    }
    TEST_ASSERT_EQ(c.stats().entries, cap);
    c.store("overflow.test", 1, ipA("10.3.3.3"), 3600);

    TEST_ASSERT_TRUE(c.lookup("max.test", 1, out, ttl));
    TEST_ASSERT_EQ(out.size(), 1u);
    TEST_ASSERT_STR_EQ(out[0], "8.8.8.8");

    remove(path);
    return 0;
}

/** The store timestamp must stay exact across the 32-bit millisecond mark (49.7
 *  days of uptime) — the moment a 32-bit counter would turn over. */
static int test_age_across_the_32_bit_millisecond_mark()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    const size_t cap = c.stats().capacity;

    // 49.7 days of uptime: the next millisecond is where the 32-bit millisecond
    // timestamp of a record turns over.
    advanceMs(4294967000ULL);
    c.store("older.test", 1, ipA("10.5.5.5"), 3600);   // stored before the turnover
    advanceMs(1000);
    c.store("newer.test", 1, ipA("10.6.6.6"), 3600);   // stored after it
    c.store("age.test", 1, ipA("10.7.7.7"), 3600);

    vector<string> out;
    uint32_t ttl = 0;
    // Just stored: its age is zero, not 49.7 days.
    TEST_ASSERT_TRUE(c.lookup("age.test", 1, out, ttl));
    TEST_ASSERT_EQ(ttl, 3600u);
    advanceMs(1000);
    TEST_ASSERT_TRUE(c.lookup("age.test", 1, out, ttl));
    TEST_ASSERT_EQ(ttl, 3599u);   // exactly one second of its TTL is gone

    // Fill the pool and force one eviction: at equal usage counters the oldest
    // record goes — and the oldest is the one stored *before* the mark, which a
    // truncated (32-bit) timestamp would report as the newest.
    for (size_t i = 0; i + 3 < cap; i++) {
        c.store("w" + to_string(i) + ".test", 1, ipA("10.4.4.4"), 3600);
    }
    TEST_ASSERT_EQ(c.stats().entries, cap);
    c.store("overflow.test", 1, ipA("10.3.3.3"), 3600);

    TEST_ASSERT_FALSE(c.lookup("older.test", 1, out, ttl));   // evicted: oldest
    TEST_ASSERT_TRUE(c.lookup("newer.test", 1, out, ttl));
    return 0;
}

/** Save/load must survive the same mark. A truncated timestamp made the file
 *  lose almost everything (records looked expired and were skipped) and could
 *  not read back what it did write: `loadFromFile()` stamps the records with
 *  "now", and their age was computed against the truncated value. */
static int test_save_and_load_across_the_mark()
{
    resetClock();
    const char* path = "test_cache_mark.dat";
    remove(path);

    // 49.7 days of uptime: a 32-bit millisecond counter would turn over here.
    advanceMs(4294967000ULL);
    {
        InternalDnsCache c;
        TEST_ASSERT_TRUE(c.enable(1));
        c.store("before.test", 1, ipA("10.0.0.1"), 3600);
        advanceMs(1000);                              // past the mark
        c.store("after.test", 1, ipA("10.0.0.2"), 3600);
        c.store("after2.test", 1, ipA("10.0.0.3"), 1800);

        size_t written = 0;
        TEST_ASSERT_TRUE(c.saveToFile(path, &written));
        TEST_ASSERT_EQ(written, 3u);
        TEST_ASSERT_EQ(c.fileInfo(path).entries, 3u);
        c.disable();
    }
    {
        // A fresh cache, as after a reboot: everything must come back readable
        // with the lifetime that was left in the file (the first record was
        // stored a second before the save).
        InternalDnsCache c;
        TEST_ASSERT_TRUE(c.enable(1));
        size_t loaded = 0;
        TEST_ASSERT_TRUE(c.loadFromFile(path, &loaded));
        TEST_ASSERT_EQ(loaded, 3u);
        TEST_ASSERT_EQ(c.stats().entries, 3u);

        vector<string> out;
        uint32_t ttl = 0;
        TEST_ASSERT_TRUE(c.lookup("before.test", 1, out, ttl));
        TEST_ASSERT_EQ(ttl, 3599u);
        TEST_ASSERT_TRUE(c.lookup("after.test", 1, out, ttl));
        TEST_ASSERT_EQ(ttl, 3600u);
        TEST_ASSERT_TRUE(c.lookup("after2.test", 1, out, ttl));
        TEST_ASSERT_EQ(ttl, 1800u);
        c.disable();
    }

    remove(path);
    return 0;
}

/** With TTL expiry off a record answers until it is evicted or cleared. */
static int test_ignore_ttl()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    c.setIgnoreTtl(true);
    c.store("forever.test", 1, ipA("10.4.4.4"), 10);
    advanceMs(600000);

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_TRUE(c.lookup("forever.test", 1, out, ttl));
    TEST_ASSERT_EQ(ttl, 10u);   // the original TTL, not a remaining one

    c.clear();
    const auto s = c.stats();
    TEST_ASSERT_EQ(s.entries, 0u);
    // Records are gone, so a "hottest name" would name nothing.
    TEST_ASSERT_EQ(s.usesMax, 0);
    TEST_ASSERT_STR_EQ(s.topName, "");
    return 0;
}

/** The reported average is the whole lookup, and a lookup takes the arena mutex
 *  first — so the cache has to say how much of it was waiting and how many
 *  records it walked. On the host both numbers are exact: the stub mutex never
 *  contends (the wait is 0) and the clock is only what the test moves. */
static int test_hit_reports_its_time_split()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    c.store("split.test", 1, ipA("10.1.1.1"), 60);
    TEST_ASSERT_EQ(c.stats().stores, 1);            // the store that created it

    vector<string> out;
    uint32_t ttl = 0;
    TEST_ASSERT_TRUE(c.lookup("split.test", 1, out, ttl));
    auto s = c.stats();
    TEST_ASSERT_EQ(s.hits, 1);
    TEST_ASSERT_EQ(s.waitUs, 0);                    // nothing else holds the lock
    TEST_ASSERT_EQ(s.walkedNodes, 1);               // one record in its bucket

    TEST_ASSERT_TRUE(c.lookup("split.test", 1, out, ttl));
    s = c.stats();
    TEST_ASSERT_EQ(s.hits, 2);
    TEST_ASSERT_EQ(s.walkedNodes, 2);               // counted per hit

    // A miss walks a chain as well, but it is not a hit: its walk must not end
    // up in a number the page divides by hits.
    TEST_ASSERT_FALSE(c.lookup("absent.test", 1, out, ttl));
    TEST_ASSERT_EQ(c.stats().walkedNodes, 2);
    TEST_ASSERT_EQ(c.stats().misses, 1);

    // A refresh is a store, not a hit.
    c.store("split.test", 1, ipA("10.1.1.2"), 60);
    TEST_ASSERT_EQ(c.stats().stores, 2);
    return 0;
}

/** The full-arena eviction scan holds the mutex while it walks everything, so it
 *  is the one event that can make somebody else's hit wait for milliseconds.
 *  Counted and timed, and its size is the pool — not a guess. */
static int test_eviction_scan_is_measured()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    const size_t cap = c.stats().capacity;

    for (size_t i = 0; i < cap; i++) {
        c.store("s" + to_string(i) + ".test", 1, ipA("10.2.2.2"), 3600);
    }
    TEST_ASSERT_EQ(c.stats().entries, cap);
    TEST_ASSERT_EQ(c.stats().evictScans, 0);        // the pool filled without one

    c.store("overflow.test", 1, ipA("10.3.3.3"), 3600);   // the pool is full
    const auto s = c.stats();
    TEST_ASSERT_EQ(s.evictScans, 1);
    TEST_ASSERT_EQ(s.evictScanNodes, cap);          // the scan is the whole pool
    TEST_ASSERT_EQ(s.evictScanUs, 0);               // the host clock does not move
    TEST_ASSERT_EQ(s.evicted, 1u);
    return 0;
}

/** The read-back check a planned restart runs (stage 169) — `CacheFileReader` —
 *  has to pass on the file this cache writes, and must not pass on one that was
 *  cut short. That is the whole difference between "saveToFile returned" and
 *  "the volume holds what was saved", and it is checked here on the writer's own
 *  output rather than on a file assembled by hand. */
static int test_saved_file_passes_the_restart_check()
{
    resetClock();
    InternalDnsCache c;
    TEST_ASSERT_TRUE(c.enable(1));
    c.store("example.com", 1, ipA("93.184.216.34"), 60);
    c.store("example.net", 1, ipA("93.184.216.35"), 60);

    const char* path = "test_cache_check.dat";
    remove(path);
    size_t written = 0;
    TEST_ASSERT_TRUE(c.saveToFile(path, &written));
    TEST_ASSERT_EQ(written, 2u);

    const CacheFileCheck good = CacheFileReader::check(path, written);
    TEST_ASSERT_TRUE(good.ok);
    TEST_ASSERT_EQ(good.records, 2u);

    // The count the save reported is part of the check: a file that holds fewer
    // records than were written is not the file that was written.
    const CacheFileCheck wrong_count = CacheFileReader::check(path, written + 1);
    TEST_ASSERT_FALSE(wrong_count.ok);
    TEST_ASSERT_TRUE(wrong_count.why.find("2 records") != string::npos);

    // A file cut short is a torn write, and a torn write must not pass.
    string bytes;
    {
        FILE* in = fopen(path, "rb");
        TEST_ASSERT_TRUE(in != nullptr);
        char buf[512];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) bytes.append(buf, n);
        fclose(in);
    }
    TEST_ASSERT_TRUE(bytes.size() > 1);
    bytes.resize(bytes.size() - 1);

    const char* cut = "test_cache_check_cut.dat";
    remove(cut);
    {
        FILE* out = fopen(cut, "wb");
        TEST_ASSERT_TRUE(out != nullptr);
        fwrite(bytes.data(), 1, bytes.size(), out);
        fclose(out);
    }
    const CacheFileCheck torn = CacheFileReader::check(cut, written);
    TEST_ASSERT_FALSE(torn.ok);

    remove(cut);
    remove(path);
    return 0;
}

void app_main()
{
    printf("Running InternalDnsCache tests...\n");
    int failures = 0;

    failures += test_basic_answer();
    failures += test_usage_counter();
    failures += test_expired_is_not_a_use();
    failures += test_least_used_is_evicted();
    failures += test_recycled_node_starts_from_zero();
    failures += test_save_load_keeps_counters();
    failures += test_load_reads_version2_file();
    failures += test_load_reads_version1_file();
    failures += test_usage_counter_saturates_at_max();
    failures += test_age_across_the_32_bit_millisecond_mark();
    failures += test_save_and_load_across_the_mark();
    failures += test_save_reports_nothing_to_save();
    failures += test_ignore_ttl();
    failures += test_hit_reports_its_time_split();
    failures += test_eviction_scan_is_measured();
    failures += test_saved_file_passes_the_restart_check();

    if (failures == 0) {
        printf("All InternalDnsCache tests PASSED!\n");
    } else {
        printf("Some InternalDnsCache tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#else   // !DHCP_TEST_HOST

#include <cstdio>

extern "C" void app_main()
{
    printf("test_internalcache is a host-only test (build with -DDHCP_TEST_HOST)"
           " — see the file header.\n");
}

#endif  // DHCP_TEST_HOST
