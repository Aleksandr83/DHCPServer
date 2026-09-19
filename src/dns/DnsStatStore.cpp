#include "DnsStatStore.h"

#include "../core/ErrorLogCore.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

namespace dhcp {
namespace dns {

namespace {

constexpr char kMagic[4] = {'D', 'S', 'T', '1'};

/** @brief Append a little-endian unsigned value. */
void putU32(std::string& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void putU64(std::string& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

uint32_t getU32(const std::string& in, size_t offset)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(static_cast<unsigned char>(in[offset + i])) << (8 * i);
    }
    return value;
}

uint64_t getU64(const std::string& in, size_t offset)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<unsigned char>(in[offset + i])) << (8 * i);
    }
    return value;
}

/** @brief Sum of the bytes of @p text (the file's own integrity check). */
uint32_t checksumOf(const std::string& text)
{
    uint32_t sum = 0;
    for (char c : text) sum += static_cast<unsigned char>(c);
    return sum;
}

} // namespace

std::string DnsStatStore::encode(const DnsStatTotals& totals)
{
    std::string out;
    out.reserve(kRecordSize);
    out.append(kMagic, sizeof(kMagic));
    putU32(out, kVersion);
    putU32(out, 72);            // payload size: nine 64-bit counters
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

bool DnsStatStore::decode(const std::string& record, DnsStatTotals& out, std::string* why)
{
    if (record.size() != kRecordSize && record.size() != kRecordSizeLegacy) {
        if (why) *why = "unexpected size";
        return false;
    }
    if (std::memcmp(record.data(), kMagic, sizeof(kMagic)) != 0) {
        if (why) *why = "not a statistics file";
        return false;
    }

    const uint32_t version = getU32(record, 4);
    if (version == 0 || version > kVersion) {
        // A newer file is refused on purpose: this build cannot know what the
        // extra fields mean, and guessing would show wrong numbers on the page.
        if (why) *why = "unsupported version";
        return false;
    }
    // The payload size has to agree with the version *and* with the length of
    // the record: that is what tells a version 1 file (three counters) from a
    // version 2 one (nine), and a torn write from a plausible file.
    const uint32_t wantPayload = (version == kVersionLegacy) ? 24u : 72u;
    const uint32_t wantSize = 16u + wantPayload + 4u;
    if (getU32(record, 8) != wantPayload || record.size() != wantSize) {
        if (why) *why = "payload size mismatch";
        return false;
    }

    const uint32_t stored = getU32(record, record.size() - 4);
    if (stored != checksumOf(record.substr(0, record.size() - 4))) {
        if (why) *why = "checksum mismatch";
        return false;
    }

    out.hits = getU64(record, 16);
    out.forwards = getU64(record, 24);
    out.hitUsSum = getU64(record, 32);
    if (version >= 2) {
        out.waitUs = getU64(record, 40);
        out.walkedNodes = getU64(record, 48);
        out.stores = getU64(record, 56);
        out.evictScans = getU64(record, 64);
        out.evictScanUs = getU64(record, 72);
        out.evictScanNodes = getU64(record, 80);
    }
    return true;
}

bool DnsStatStore::save(const std::string& path, const DnsStatTotals& totals, std::string* why)
{
    const std::string record = encode(totals);

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
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        if (why) *why = "cannot create the file";
        return false;
    }

    const size_t written = std::fwrite(record.data(), 1, record.size(), file);
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;

    if (written != record.size() || !flushed || !closed) {
        if (why) *why = "write failed";
        return false;
    }
    return true;
}

bool DnsStatStore::saveWithRetry(const std::string& path, const DnsStatTotals& totals,
                                 std::string* why, core::ErrorLogCore* log)
{
    std::string first;
    if (save(path, totals, &first)) return true;

    if (log) log->submit(core::LogLevel::Error, "stats",
                         "statistics could not be saved to " + path + " (" + first + ")");

    // Start over from nothing: a leftover .tmp, or the destination itself, may be
    // the very reason the first attempt failed.
    const bool removedDest = std::remove(path.c_str()) == 0;
    std::remove((path + ".tmp").c_str());
    if (log) log->submit(core::LogLevel::Warn, "stats",
                         "retrying " + path + (removedDest ? " (the file was removed first)"
                                                          : " (there was no file to remove)"));

    std::string second;
    if (save(path, totals, &second)) {
        if (log) log->submit(core::LogLevel::Warn, "stats",
                             "the second attempt saved " + path);
        return true;
    }

    if (log) log->submit(core::LogLevel::Error, "stats",
                         "the second attempt failed too (" + second +
                             ") — there is no statistics file now");
    if (why) *why = second;
    return false;
}

bool DnsStatStore::load(const std::string& path, DnsStatTotals& out, std::string* why)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (why) *why = "no statistics file";
        return false;
    }

    // Read as much as any record can hold, then hand `decode()` exactly the
    // length the header claims — a version 1 file is 44 bytes and must not be
    // refused just because this build writes 92.
    char buffer[kRecordSize];
    const size_t got = std::fread(buffer, 1, sizeof(buffer), file);
    std::fclose(file);
    if (got < 16) {
        if (why) *why = "unexpected size";
        return false;
    }
    const uint32_t payload = getU32(std::string(buffer, 16), 8);
    const size_t size = 16u + payload + 4u;
    if (size > got) {
        if (why) *why = "truncated file";
        return false;
    }
    return decode(std::string(buffer, size), out, why);
}

bool DnsStatStore::remove(const std::string& path)
{
    return std::remove(path.c_str()) == 0;
}

bool DnsStatStore::exists(const std::string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

} // namespace dns
} // namespace dhcp
