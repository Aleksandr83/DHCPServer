#include "DnsStatStore.h"

#include "../core/ErrorLogCore.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

using namespace std;

namespace dhcp {
namespace dns {

namespace {

constexpr char kMagic[4] = {'D', 'S', 'T', '1'};

/** @brief Append a little-endian unsigned value. */
void putU32(string& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void putU64(string& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

uint32_t getU32(const string& in, size_t offset)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(static_cast<unsigned char>(in[offset + i])) << (8 * i);
    }
    return value;
}

uint64_t getU64(const string& in, size_t offset)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<unsigned char>(in[offset + i])) << (8 * i);
    }
    return value;
}

/** @brief Sum of the bytes of @p text (the file's own integrity check). */
uint32_t checksumOf(const string& text)
{
    uint32_t sum = 0;
    for (char c : text) sum += static_cast<unsigned char>(c);
    return sum;
}

} // namespace

string DnsStatStore::encode(const DnsStatTotals& totals)
{
    string out;
    out.reserve(kRecordSize);
    out.append(kMagic, sizeof(kMagic));
    putU32(out, kVersion);
    putU32(out, kPayloadV2Bytes);            // payload size: nine 64-bit counters
    putU32(out, 0);             // reserved — the next growth of the format
    putU64(out, totals.hits);
    putU64(out, totals.forwards);
    putU64(out, totals.hitUsSum);
    // Stage 128: the sums behind the split of the average. They are written
    // rather than the averages, for the same reason the average itself is not:
    // only a total can be continued after a reboot.
    putU64(out, totals.waitUs);
    putU64(out, totals.walkedNodes);
    putU64(out, totals.stores);
    putU64(out, totals.evictScans);
    putU64(out, totals.evictScanUs);
    putU64(out, totals.evictScanNodes);
    putU32(out, checksumOf(out));
    return out;
}

bool DnsStatStore::decode(const string& record, DnsStatTotals& out, string* why)
{
    if (record.size() != kRecordSize && record.size() != kRecordSizeLegacy) {
        if (why) *why = "unexpected size";
        return false;
    }
    if (memcmp(record.data(), kMagic, sizeof(kMagic)) != 0) {
        if (why) *why = "not a statistics file";
        return false;
    }

    const uint32_t version = getU32(record, kVersionOffset);
    if (version == 0 || version > kVersion) {
        // A newer file is refused on purpose: this build cannot know what the
        // extra fields mean, and guessing would show wrong numbers on the page.
        if (why) *why = "unsupported version";
        return false;
    }
    // The payload size has to agree with the version *and* with the length of
    // the record: that is what tells a version 1 file (three counters) from a
    // version 2 one (nine), and a torn write from a plausible file.
    const uint32_t wantPayload = (version == kVersionLegacy) ? kPayloadV1Bytes : kPayloadV2Bytes;
    const uint32_t wantSize = kHeaderBytes + wantPayload + kChecksumBytes;
    if (getU32(record, kPayloadSizeOffset) != wantPayload || record.size() != wantSize) {
        if (why) *why = "payload size mismatch";
        return false;
    }

    const uint32_t stored = getU32(record, record.size() - kChecksumBytes);
    if (stored != checksumOf(record.substr(0, record.size() - kChecksumBytes))) {
        if (why) *why = "checksum mismatch";
        return false;
    }

    out.hits = getU64(record, kOffsetHits);
    out.forwards = getU64(record, kOffsetForwards);
    out.hitUsSum = getU64(record, kOffsetHitUsSum);
    if (version >= 2) {
        out.waitUs = getU64(record, kOffsetWaitUs);
        out.walkedNodes = getU64(record, kOffsetWalkedNodes);
        out.stores = getU64(record, kOffsetStores);
        out.evictScans = getU64(record, 64);
        out.evictScanUs = getU64(record, 72);
        out.evictScanNodes = getU64(record, 80);
    }
    return true;
}

bool DnsStatStore::save(const string& path, const DnsStatTotals& totals, string* why)
{
    const string record = encode(totals);

    // Straight into the file, exactly like `cache.dat` (InternalDnsCache::saveToFile).
    //
    // It used to be published through `<path>.tmp` + `rename`, and that does not
    // work on this filesystem: **FatFS refuses a rename onto an existing name**, so
    // from the second save on every attempt failed with "cannot publish the file"
    // (the device log said so, three times in a row, and the operator's file was
    // there the whole time). The neighbour that got it right is
    // `FileSink::commit()`, which removes the destination before renaming — but the
    // operator asked for the simpler symmetry with the cache instead, and the trade
    // is written down rather than hidden: **an interrupted write can now leave a
    // half-written record**. What protects the reader is the format itself — magic,
    // version, payload size and checksum are checked on every load, so a torn file
    // is refused instead of being read as plausible numbers (the same protection
    // `cache.dat` relies on).
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        if (why) *why = "cannot create the file";
        return false;
    }

    const size_t written = fwrite(record.data(), 1, record.size(), file);
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;

    if (written != record.size() || !flushed || !closed) {
        if (why) *why = "write failed";
        return false;
    }
    return true;
}

bool DnsStatStore::verify(const string& path, const DnsStatTotals& totals, string* why)
{
    const string want = encode(totals);

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (why) *why = "the file is not there";
        return false;
    }

    // One byte more than a record is read, so a file that is *longer* than what
    // was written cannot pass just because its first 92 bytes happen to match.
    char got[kRecordSize + 1];
    const size_t n = fread(got, 1, sizeof(got), file);
    // The buffer was filled and the file was not over: "more than N bytes" is the
    // honest way to say it, and the operator reads these words in a dialog.
    const bool more = (n == sizeof(got)) && (fgetc(file) != EOF);
    fclose(file);

    if (n != want.size() || memcmp(got, want.data(), want.size()) != 0) {
        if (why) {
            *why = (n == want.size())
                       ? "the content of the file differs from what was written"
                       : "the file holds " + string(more ? "more than " : "") +
                             to_string(n) + " bytes instead of " + to_string(want.size());
        }
        return false;
    }
    return true;
}

RestartSaveVerify::Outcome DnsStatStore::saveWithRetry(const string& path,
                                                       const DnsStatTotals& totals,
                                                       string* why, core::ErrorLogCore* log,
                                                       const PhaseHandler& phase)
{
    // The policy reports every attempt before the next one starts, which is what
    // makes the log read in the order things happened: the failure, the decision
    // to retry, and then the retry's own outcome.
    string lastWhy;
    const auto report = [&](int attempt, RestartSaveVerify::AttemptResult result,
                            const string& reason) {
        if (!reason.empty()) lastWhy = reason;
        if (log == nullptr) return;

        if (result == RestartSaveVerify::AttemptResult::Ok) {
            if (attempt > 1) {
                log->submit(core::LogLevel::Warn, "stats",
                            "the second attempt saved " + path);
            }
            return;
        }
        const bool mismatch = (result == RestartSaveVerify::AttemptResult::Mismatch);
        if (attempt == 1) {
            log->submit(core::LogLevel::Error, "stats",
                        mismatch
                            ? "the statistics file does not hold what was written (" + reason + ")"
                            : "statistics could not be saved to " + path + " (" + reason + ")");
            // Start over from nothing: a leftover .tmp, or the destination
            // itself, may be the very reason the first attempt failed.
            log->submit(core::LogLevel::Warn, "stats",
                        "retrying " + path + " (the file is removed first)");
            return;
        }
        log->submit(core::LogLevel::Error, "stats",
                    mismatch
                        ? "the second attempt wrote a file that still differs (" + reason +
                              ") — there is no usable statistics file now"
                        : "the second attempt failed too (" + reason +
                              ") — there is no statistics file now");
    };

    const auto attempt = [&](int n, string& reason) {
        // The retry starts from a clean slate (the operator's rule, kept from
        // stage 123): a leftover `.tmp`, or the destination itself, may be what
        // the first attempt tripped over.
        if (n > 1) {
            std::remove(path.c_str());
            std::remove((path + ".tmp").c_str());
        }
        if (!save(path, totals, &reason)) return RestartSaveVerify::AttemptResult::Failed;
        // The write is done and the file is about to be read back: the caller is
        // told, so the page can show "checking" rather than the save's last word.
        if (phase) phase(true);
        const bool holds = verify(path, totals, &reason);
        if (phase) phase(false);
        if (!holds) return RestartSaveVerify::AttemptResult::Mismatch;
        return RestartSaveVerify::AttemptResult::Ok;
    };

    const RestartSaveVerify::Outcome outcome = RestartSaveVerify::run(attempt, report);
    if (why && outcome != RestartSaveVerify::Outcome::Ok) *why = lastWhy;
    return outcome;
}

bool DnsStatStore::load(const string& path, DnsStatTotals& out, string* why)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (why) *why = "no statistics file";
        return false;
    }

    // Read as much as any record can hold, then hand `decode()` exactly the
    // length the header claims — a version 1 file is 44 bytes and must not be
    // refused just because this build writes 92.
    char buffer[kRecordSize];
    const size_t got = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    if (got < kHeaderBytes) {
        if (why) *why = "unexpected size";
        return false;
    }
    const uint32_t payload = getU32(string(buffer, kHeaderBytes), kPayloadSizeOffset);
    const size_t size = kHeaderBytes + payload + kChecksumBytes;
    if (size > got) {
        if (why) *why = "truncated file";
        return false;
    }
    return decode(string(buffer, size), out, why);
}

bool DnsStatStore::remove(const string& path)
{
    return std::remove(path.c_str()) == 0;
}

bool DnsStatStore::exists(const string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

} // namespace dns
} // namespace dhcp
