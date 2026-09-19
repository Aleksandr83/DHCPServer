// Host test of the statistics file (format + atomicity).
//
// `DnsStatStore` is free of ESP-IDF on purpose: the file it writes is what makes
// the numbers on the main page survive a reboot, and "the file survived but the
// numbers are wrong" is exactly the kind of defect that has to be provable
// without a board. The test runs against the real host filesystem, in a
// temporary directory.
//
// Build (MinGW g++ 13, PATH must contain C:\Qt\Tools\mingw1310_64\bin):
//   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -Itest/stubs -I.
//       test/test_dnsstatstore.cpp src/dns/DnsStatStore.cpp -o t_dnsstatstore.exe
//
// The harness always exits with 0 — the proof is the printed text.

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "src/dns/DnsStatStore.h"
#include "src/core/ErrorLogCore.h"

using dhcp::dns::DnsStatStore;
using dhcp::dns::DnsStatTotals;

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

static int makeDir(const std::string& path)
{
#ifdef _WIN32
    return ::_mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0777);
#endif
}

static bool exists(const std::string& path)
{
    struct stat st = {};
    return ::stat(path.c_str(), &st) == 0;
}

static std::string tempBase()
{
    const char* env = std::getenv("TEMP");
    if (env == nullptr) env = std::getenv("TMP");
    return (env != nullptr) ? std::string(env) : std::string(".");
}

static std::string g_dir;

static void test_encode_has_a_fixed_size_and_a_magic()
{
    std::printf("test_encode_has_a_fixed_size_and_a_magic\n");
    DnsStatTotals totals;
    totals.hits = 132;
    totals.forwards = 9;
    totals.hitUsSum = 132 * 199;

    const std::string record = DnsStatStore::encode(totals);
    check(record.size() == DnsStatStore::kRecordSize, "the record is 92 bytes");
    check(record.compare(0, 4, "DST1") == 0, "it starts with the magic");
    check(DnsStatStore::kVersion == 2, "this build writes version 2");
    check(DnsStatStore::kRecordSizeLegacy == 44, "version 1 records are 44 bytes");
}

/** Stage 128: the sums behind the split of the average travel with it, exactly
 *  as the average's own sum does — a stored average cannot be continued, and
 *  neither can a stored "wait" or "walk". */
static void test_the_split_survives_the_round_trip()
{
    std::printf("test_the_split_survives_the_round_trip\n");
    DnsStatTotals in;
    in.hits = 1000;
    in.forwards = 10;
    in.hitUsSum = 1000 * 120;
    in.waitUs = 1000 * 7;
    in.walkedNodes = 1130;                 // 1.13 nodes per hit
    in.stores = 10;
    in.evictScans = 3;
    in.evictScanUs = 3 * 4000;
    in.evictScanNodes = 3 * 59520;

    DnsStatTotals out;
    std::string why;
    check(DnsStatStore::decode(DnsStatStore::encode(in), out, &why), "the record decodes");
    check(out.hits == in.hits && out.hitUsSum == in.hitUsSum,
          "the numbers the average is made of are exact");
    check(out.waitUs == in.waitUs && out.walkedNodes == in.walkedNodes &&
          out.stores == in.stores && out.evictScans == in.evictScans &&
          out.evictScanUs == in.evictScanUs && out.evictScanNodes == in.evictScanNodes,
          "all six sums of the split come back exactly");
    check(out.avgHitUs() == 120 && out.avgWaitUs() == 7, "so both averages derive as before");
    check(out.walkX100() == 113, "and the walk reads as 1.13 nodes per hit");

    // Every counter at its maximum: nine 64-bit fields must not have brought an
    // alignment or a sign problem with them.
    DnsStatTotals max;
    max.hits = 18446744073709551615ull;
    max.hitUsSum = 18446744073709551615ull;
    max.waitUs = 18446744073709551615ull;
    max.walkedNodes = 18446744073709551615ull;
    max.stores = 18446744073709551615ull;
    max.evictScans = 18446744073709551615ull;
    max.evictScanUs = 18446744073709551615ull;
    max.evictScanNodes = 18446744073709551615ull;
    DnsStatTotals mOut;
    check(DnsStatStore::decode(DnsStatStore::encode(max), mOut, &why), "a maximal record decodes");
    check(mOut.waitUs == max.waitUs && mOut.evictScanNodes == max.evictScanNodes,
          "the full 64-bit range survives");
}

/** The version and the payload size have to agree with the length of the record.
 *  Without that check a torn file could pass itself off as a shorter, valid one
 *  — the version 1 shape is exactly such a shorter file. */
static void test_the_version_and_the_payload_size_must_agree()
{
    std::printf("test_the_version_and_the_payload_size_must_agree\n");
    DnsStatTotals totals;
    totals.hits = 5;
    const std::string good = DnsStatStore::encode(totals);
    DnsStatTotals out;
    std::string why;

    std::string asV1 = good;
    asV1[4] = 1;                 // claim version 1, keep the 72-byte payload
    check(!DnsStatStore::decode(asV1, out, &why) && why == "payload size mismatch",
          "a version 1 header on a version 2 payload is refused");

    const std::string shortV2 = good.substr(0, DnsStatStore::kRecordSizeLegacy);
    check(!DnsStatStore::decode(shortV2, out, &why),
          "a 44-byte record claiming version 2 is refused");

    std::string split = good;
    split[40] = static_cast<char>(split[40] ^ 0x01);   // flip a bit in `waitUs`
    check(!DnsStatStore::decode(split, out, &why) && why == "checksum mismatch",
          "the checksum covers the new counters too");
}

/** A version 1 file — three counters, 44 bytes — is what the operator's device
 *  has on it right now. It must load, with the newer sums at zero: refusing it
 *  would throw away the only history the average has. */
static void test_a_version1_file_still_loads()
{
    std::printf("test_a_version1_file_still_loads\n");
    const std::string path = g_dir + "/legacy.dat";

    std::string r;
    r.append("DST1", 4);
    auto u32 = [&r](uint32_t v) {
        for (int i = 0; i < 4; i++) r.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    auto u64 = [&r](uint64_t v) {
        for (int i = 0; i < 8; i++) r.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    u32(1);              // version 1
    u32(24);             // payload: three counters
    u32(0);              // reserved
    u64(500);            // hits
    u64(7);              // forwards
    u64(500 * 42);       // hitUsSum -> an average of 42 us
    uint32_t sum = 0;
    for (char c : r) sum += static_cast<unsigned char>(c);
    u32(sum);            // checksum
    check(r.size() == DnsStatStore::kRecordSizeLegacy, "the legacy record is 44 bytes");

    std::FILE* f = std::fopen(path.c_str(), "wb");
    check(f != nullptr, "the legacy file is written");
    if (f != nullptr) {
        std::fwrite(r.data(), 1, r.size(), f);
        std::fclose(f);
    }

    DnsStatTotals back;
    std::string why;
    check(DnsStatStore::load(path, back, &why), "a version 1 file loads (" + why + ")");
    check(back.hits == 500 && back.forwards == 7, "its counters come back");
    check(back.avgHitUs() == 42, "and so does the average they make");
    check(back.waitUs == 0 && back.walkedNodes == 0 && back.evictScans == 0 &&
          back.evictScanNodes == 0, "the counters version 1 never had stay zero");
}

static void test_round_trip_keeps_every_number()
{
    std::printf("test_round_trip_keeps_every_number\n");
    DnsStatTotals in;
    in.hits = 18446744073709551615ull;      // every counter at its maximum
    in.forwards = 12345678901234567890ull;
    in.hitUsSum = 9876543210987654321ull;

    DnsStatTotals out;
    std::string why;
    check(DnsStatStore::decode(DnsStatStore::encode(in), out, &why), "a record decodes");
    check(out.hits == in.hits && out.forwards == in.forwards && out.hitUsSum == in.hitUsSum,
          "all three numbers survive a 64-bit round trip");

    DnsStatTotals small;
    small.hits = 132;
    small.forwards = 9;
    small.hitUsSum = 132 * 199;
    DnsStatTotals back;
    check(DnsStatStore::decode(DnsStatStore::encode(small), back, &why), "a small record decodes");
    check(back.hits == 132 && back.forwards == 9, "hits and forwards are exact");
    check(back.avgHitUs() == 199, "the average derives from the sum: 199 us");
}

static void test_the_average_is_an_average_across_a_reboot()
{
    std::printf("test_the_average_is_an_average_across_a_reboot\n");
    // Saved before the reboot: 4 hits of 100 us. After it, two more hits of 400 us.
    DnsStatTotals saved;
    saved.hits = 4;
    saved.hitUsSum = 400;
    check(saved.avgHitUs() == 100, "100 us before the reboot");

    saved.hits += 2;
    saved.hitUsSum += 800;
    check(saved.avgHitUs() == 200, "after two slower hits the average is 200 us, not 250");
    check(saved.avgHitUs() * 6 == 1200, "which is the honest (sum / count)");

    DnsStatTotals empty;
    check(empty.avgHitUs() == 0, "no hits means no average (no division by zero)");
}

static void test_damaged_records_are_refused_with_a_reason()
{
    std::printf("test_damaged_records_are_refused_with_a_reason\n");
    DnsStatTotals totals;
    totals.hits = 5;
    const std::string good = DnsStatStore::encode(totals);
    DnsStatTotals out;
    std::string why;

    check(!DnsStatStore::decode("", out, &why) && why == "unexpected size",
          "an empty file is refused");
    check(!DnsStatStore::decode(good.substr(0, good.size() - 1), out, &why) &&
          why == "unexpected size", "a truncated file is refused");
    check(!DnsStatStore::decode(good + "x", out, &why) && why == "unexpected size",
          "a file with trailing bytes is refused");

    std::string foreign = good;
    foreign[0] = 'X';
    check(!DnsStatStore::decode(foreign, out, &why) && why == "not a statistics file",
          "a foreign file is refused");

    // A version from the future: this build cannot know what its extra fields mean.
    std::string newer = good;
    newer[4] = 9;
    check(!DnsStatStore::decode(newer, out, &why) && why == "unsupported version",
          "a newer version is refused instead of guessed");

    std::string corrupt = good;
    corrupt[16] = static_cast<char>(corrupt[16] ^ 0x01);   // flip a bit in `hits`
    check(!DnsStatStore::decode(corrupt, out, &why) && why == "checksum mismatch",
          "a single flipped bit is caught by the checksum");

    std::string wrongSize = good;
    wrongSize[8] = 25;
    check(!DnsStatStore::decode(wrongSize, out, &why), "a payload-size mismatch is refused");
}

static void test_save_load_and_no_temporary_file_is_left()
{
    std::printf("test_save_load_and_no_temporary_file_is_left\n");
    const std::string path = g_dir + "/Statistica.dat";

    DnsStatTotals totals;
    totals.hits = 132;
    totals.forwards = 9;
    totals.hitUsSum = 132 * 199;

    std::string why;
    check(DnsStatStore::save(path, totals, &why), "the file is written");
    check(exists(path), "it is there");
    check(!exists(path + ".tmp"), "no temporary file stays behind");

    DnsStatTotals back;
    check(DnsStatStore::load(path, back, &why), "it is read back");
    check(back.hits == 132 && back.forwards == 9 && back.avgHitUs() == 199,
          "the numbers came back as they were written");
}

// A torn write is the price of writing in place (stage 125), and the format is
// what pays for it: a record that is half there must be refused, not read as
// plausible numbers. This is the guarantee that replaced "the previous file
// survives a failed save" — that one is simply gone, the operator chose it.
static void test_a_torn_write_is_refused_by_the_format()
{
    std::printf("test_a_torn_write_is_refused_by_the_format\n");
    const std::string path = g_dir + "/torn.dat";

    DnsStatTotals totals;
    totals.hits = 900;
    totals.hitUsSum = 900 * 20;
    std::string why;
    check(DnsStatStore::save(path, totals, &why), "a good record is written");

    // Cut it in the middle: the first 20 bytes of the 92 are what a power cut
    // leaves behind.
    const std::string record = DnsStatStore::encode(totals);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    check(f != nullptr, "the file can be reopened for the simulation");
    if (f != nullptr) {
        std::fwrite(record.data(), 1, 20, f);
        std::fclose(f);
    }

    DnsStatTotals back;
    check(!DnsStatStore::load(path, back, &why),
          "the truncated record is refused instead of being trusted");
    check(!why.empty(), "and it says why (" + why + ")");
}

static void test_missing_and_unwritable_paths_are_reported()
{
    std::printf("test_missing_and_unwritable_paths_are_reported\n");
    DnsStatTotals out;
    std::string why;

    check(!DnsStatStore::load(g_dir + "/nothing_here.dat", out, &why) &&
          why == "no statistics file", "a device that never saved has no file (not an error)");

    const std::string garbage = g_dir + "/garbage.dat";
    std::FILE* f = std::fopen(garbage.c_str(), "wb");
    if (f != nullptr) { std::fwrite("not a record at all", 1, 19, f); std::fclose(f); }
    check(!DnsStatStore::load(garbage, out, &why), "a garbage file is refused");

    check(!DnsStatStore::save(g_dir + "/no_such_dir/Statistica.dat", out, &why),
          "saving into a missing directory fails instead of crashing");
    check(why == "cannot create the file", "with the filesystem reason");
}

static void test_remove_is_idempotent_enough_for_a_factory_reset()
{
    std::printf("test_remove_is_idempotent_enough_for_a_factory_reset\n");
    const std::string path = g_dir + "/gone.dat";

    DnsStatTotals totals;
    totals.hits = 3;
    check(DnsStatStore::save(path, totals), "the file exists");
    check(DnsStatStore::remove(path), "a factory reset can delete it");
    check(!exists(path), "it is gone");
    check(!DnsStatStore::remove(path), "deleting it twice reports the miss, does not crash");
}

// ── Writing in place, over an existing file (stage 125) ──
//
// The point of this test is a defect the board revealed and the host could have:
// the record used to be published through `<path>.tmp` + `rename`, and **FatFS
// refuses a rename onto an existing name**, so from the second save on every
// attempt failed ("cannot publish the file" in the device's error log, while the
// file sat there unchanged). MSVCRT refuses it as well — the previous version of
// this test even worked *around* that with `::remove(path)`, which is exactly how
// the defect stayed hidden. Saving twice in a row is the normal case: it happens
// on every planned restart.
static void test_saving_over_an_existing_file_works()
{
    std::printf("test_saving_over_an_existing_file_works\n");
    const std::string path = g_dir + "/over.dat";
    ::remove(path.c_str());

    DnsStatTotals first;
    first.hits = 100;
    first.forwards = 10;
    first.hitUsSum = 100 * 30;
    std::string why;
    check(DnsStatStore::save(path, first, &why), "the first save creates the file");

    DnsStatTotals second;
    second.hits = 250;
    second.forwards = 11;
    second.hitUsSum = 250 * 40;
    check(DnsStatStore::save(path, second, &why),
          "the second save overwrites it (" + why + ")");

    DnsStatTotals back;
    check(DnsStatStore::load(path, back, &why), "the file still reads back");
    check(back.hits == 250 && back.forwards == 11 && back.avgHitUs() == 40,
          "and holds the numbers of the second save");

    check(!exists(path + ".tmp"), "no temporary file is involved at all any more");
}

// ── The retry policy (stage 123) ──
//
// The operator asked for exactly this: when the record cannot be written, remove
// the file and write it once more — and every step must land in the error log.
// The host can force the failure that matters (the path cannot be opened at all),
// and that is what is asserted here; the "the file was removed first" branch needs
// a write that fails while the file is there — a full volume — which a host cannot
// fake, and it is named as unverified in the stage report instead of pretended.

/** A queue and a target that only keep their contents for the test to read. */
class TestQueue : public dhcp::core::IErrorQueue {
public:
    std::vector<dhcp::core::ErrorLogEntry> items;

    bool push(const dhcp::core::ErrorLogEntry& entry) override
    {
        items.push_back(entry);
        return true;
    }

    bool pop(dhcp::core::ErrorLogEntry& out, uint32_t /*timeoutMs*/) override
    {
        if (items.empty()) return false;
        out = items.front();
        items.erase(items.begin());
        return true;
    }
};

class TestTarget : public dhcp::core::IErrorLogTarget {
public:
    std::vector<std::string> lines;
    std::string name = "test";

    bool append(dhcp::core::LogLevel /*level*/, const std::string& line) override
    {
        lines.push_back(line);
        return true;
    }

    const std::string& description() const override { return name; }
};

static bool anyLineHas(const std::vector<std::string>& lines, const std::string& needle)
{
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

static void test_retry_is_silent_when_the_first_attempt_works()
{
    std::printf("test_retry_is_silent_when_the_first_attempt_works\n");
    const std::string path = g_dir + "/quiet.dat";
    ::remove(path.c_str());

    TestQueue queue;
    TestTarget target;
    dhcp::core::ErrorLogCore log(queue, target);

    DnsStatTotals totals;
    totals.hits = 5;
    std::string why;
    check(DnsStatStore::saveWithRetry(path, totals, &why, &log),
          "a save that works returns true");
    log.drain(0);
    check(target.lines.empty(), "and says nothing in the log (the log is for failures)");
}

static void test_retry_removes_the_file_and_logs_every_step()
{
    std::printf("test_retry_removes_the_file_and_logs_every_step\n");
    const std::string path = g_dir + "/retry.dat";
    ::remove(path.c_str());
    ::rmdir(path.c_str());
    // A directory under the record's name: fopen(path, "wb") fails, which is the
    // obstacle this test can rely on in every host filesystem.
    check(makeDir(path) == 0, "an obstacle sits under the record's name");

    TestQueue queue;
    TestTarget target;
    dhcp::core::ErrorLogCore log(queue, target);

    DnsStatTotals totals;
    totals.hits = 7;
    std::string why;
    check(!DnsStatStore::saveWithRetry(path, totals, &why, &log),
          "both attempts fail — and the call says so");
    check(why == "cannot create the file",
          "the reason reported to the page is the last attempt's: " + why);

    log.drain(0);
    check(target.lines.size() == 3, "the log holds all three steps (failure, retry, failure)");
    check(anyLineHas(target.lines, "statistics could not be saved") &&
          anyLineHas(target.lines, "cannot create the file"),
          "the first failure and its reason are in the log");
    check(anyLineHas(target.lines, "retrying"),
          "so is the retry itself, the way the operator asked for it");
    check(anyLineHas(target.lines, "the second attempt failed too"),
          "and the second failure is admitted, not hidden");

    check(::rmdir(path.c_str()) == 0, "the obstacle is removed");
}

static void test_retry_keeps_the_file_out_of_the_way_when_it_can()
{
    std::printf("test_retry_keeps_the_file_out_of_the_way_when_it_can\n");
    const std::string path = g_dir + "/untouched.dat";
    ::remove(path.c_str());

    TestQueue queue;
    TestTarget target;
    dhcp::core::ErrorLogCore log(queue, target);

    DnsStatTotals totals;
    totals.hits = 3;
    std::string why;
    check(DnsStatStore::saveWithRetry(path, totals, &why, &log),
          "a working save goes through the retry path untouched");
    log.drain(0);
    check(target.lines.empty(), "and leaves nothing in the log");

    DnsStatTotals back;
    check(DnsStatStore::load(path, back, &why) && back.hits == 3, "the record is readable");
}

int main()
{
    g_dir = tempBase() + "/dhcpserver_statstore_test";
    ::remove((g_dir + "/Statistica.dat").c_str());
    ::remove((g_dir + "/kept.dat").c_str());
    makeDir(g_dir);

    test_encode_has_a_fixed_size_and_a_magic();
    test_round_trip_keeps_every_number();
    test_the_split_survives_the_round_trip();
    test_the_version_and_the_payload_size_must_agree();
    test_a_version1_file_still_loads();
    test_the_average_is_an_average_across_a_reboot();
    test_damaged_records_are_refused_with_a_reason();
    test_save_load_and_no_temporary_file_is_left();
    test_saving_over_an_existing_file_works();
    test_a_torn_write_is_refused_by_the_format();
    test_missing_and_unwritable_paths_are_reported();
    test_remove_is_idempotent_enough_for_a_factory_reset();
    test_retry_is_silent_when_the_first_attempt_works();
    test_retry_removes_the_file_and_logs_every_step();
    test_retry_keeps_the_file_out_of_the_way_when_it_can();

    if (g_fail != 0) {
        std::printf("FAILED (%d checks)\n", g_fail);
    } else {
        std::printf("PASSED!\n");
    }
    return 0;
}

#endif // DHCP_TEST_HOST
