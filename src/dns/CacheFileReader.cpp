#include "CacheFileReader.h"

#include <cstdio>
#include <cstring>

// Rule 40: the directive sits in a translation unit of its own, so it shortens
// `string` here without following any header into another file.
using namespace std;

namespace dhcp {
namespace dns {

namespace {
constexpr uint32_t kShiftPerByte = 8;      // one byte of a little-endian value
constexpr size_t kBytesPerIpv4 = 4;
constexpr size_t kBytesPerIpv6 = 16;
constexpr uint32_t kProgressEveryRecords = 64;  // how often a read says where it is

/** @brief Little-endian u32 out of @p s. */
uint32_t getU32(const uint8_t* s)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= static_cast<uint32_t>(s[i]) << (kShiftPerByte * i);
    }
    return v;
}

/** @brief Little-endian u64 out of @p s (version 2 files carry the counter so). */
uint64_t getU64(const uint8_t* s)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= static_cast<uint64_t>(s[i]) << (kShiftPerByte * i);
    }
    return v;
}

/** @brief Read exactly @p n bytes; false when the file ends first. */
bool readExact(FILE* file, void* dst, size_t n)
{
    return fread(dst, 1, n, file) == n;
}
} // namespace

const char* CacheFileReader::statusName(CacheFileStatus status)
{
    switch (status) {
        case CacheFileStatus::Ok:                 return "ok";
        case CacheFileStatus::NotFound:           return "not found";
        case CacheFileStatus::BadHeader:          return "bad header";
        case CacheFileStatus::UnsupportedVersion: return "unsupported version";
        case CacheFileStatus::ImplausibleCount:   return "implausible record count";
        case CacheFileStatus::Truncated:          return "truncated";
        case CacheFileStatus::BadRecord:          return "bad record";
        case CacheFileStatus::TrailingBytes:      return "trailing bytes";
    }
    return "unknown";
}

CacheFileRead CacheFileReader::read(const char* path, const RecordHandler& handler,
                                    const ProgressFn& progress)
{
    CacheFileRead out;

    if (path == nullptr || *path == '\0') {
        out.why = "no file name was given";
        return out;
    }

    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        out.status = CacheFileStatus::NotFound;
        out.why = "the file is not there";
        return out;
    }

    // The size is worth having on its own: "the file is 16 bytes" next to "the
    // header promises 1188 records" is exactly what a torn write looks like, and
    // both numbers travel to the operator.
    if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        if (end > 0) out.sizeBytes = static_cast<size_t>(end);
        fseek(file, 0, SEEK_SET);
    }

    uint8_t header[kHeaderBytes];
    if (!readExact(file, header, sizeof(header))) {
        out.status = CacheFileStatus::BadHeader;
        out.why = "the file is shorter than the " + to_string(kHeaderBytes) +
                  "-byte header (" + to_string(out.sizeBytes) + " bytes)";
        fclose(file);
        return out;
    }
    if (memcmp(header, kMagic, kMagicBytes) != 0) {
        out.status = CacheFileStatus::BadHeader;
        out.why = "the file does not start with the cache magic";
        fclose(file);
        return out;
    }

    out.version = getU32(header + kVersionOffset);
    if (out.version < kVersionMin || out.version > kVersion) {
        out.status = CacheFileStatus::UnsupportedVersion;
        out.why = "format version " + to_string(out.version) +
                  " is not one this build knows";
        fclose(file);
        return out;
    }

    const uint32_t count = getU32(header + kCountOffset);
    out.headerCount = count;
    if (count > kMaxPlausibleRecords) {
        out.status = CacheFileStatus::ImplausibleCount;
        out.why = "the header claims " + to_string(count) + " records";
        fclose(file);
        return out;
    }

    // Version 1 records carry no usage counter; version 2 carries eight bytes of
    // it, version 3 four (the width the field has now).
    const bool hasUses = (out.version >= kVersionUses64);
    const bool usesAre64 = (out.version == kVersionUses64);

    bool stopped = false;
    CacheFileStatus stopStatus = CacheFileStatus::Ok;
    string stopWhy;

    auto fail = [&](CacheFileStatus status, const string& why) {
        stopped = true;
        stopStatus = status;
        stopWhy = why;
    };

    for (uint32_t i = 0; i < count && !stopped; i++) {
        CacheFileRecord rec;
        const string where = "record " + to_string(i + 1) + " of " + to_string(count);

        uint8_t nameLen = 0;
        if (!readExact(file, &nameLen, 1)) {
            fail(CacheFileStatus::Truncated, "the file ends before " + where);
            break;
        }
        if (nameLen == 0 || nameLen > kMaxNameBytes) {
            fail(CacheFileStatus::BadRecord,
                 where + " has an impossible name length (" + to_string(nameLen) + ")");
            break;
        }
        char name[kMaxNameBytes + 1];
        if (!readExact(file, name, nameLen)) {
            fail(CacheFileStatus::Truncated, "the file ends inside the name of " + where);
            break;
        }
        name[nameLen] = '\0';
        rec.name.assign(name, nameLen);

        uint8_t tail[kTailBytes];
        if (!readExact(file, tail, sizeof(tail))) {
            fail(CacheFileStatus::Truncated, "the file ends inside the fields of " + where);
            break;
        }
        rec.qtype = static_cast<uint16_t>(tail[0] | (tail[1] << kShiftPerByte));
        const uint8_t countA = tail[2];
        const uint8_t countAaaa = tail[3];
        rec.ttlRemaining = getU32(tail + 4);

        if (countA > kMaxA || countAaaa > kMaxAaaa) {
            fail(CacheFileStatus::BadRecord, where + " claims more addresses than a record holds");
            break;
        }
        const bool isA = (rec.qtype == kTypeA);
        if (!isA && rec.qtype != kTypeAaaa) {
            fail(CacheFileStatus::BadRecord,
                 where + " has query type " + to_string(rec.qtype) + ", which is neither A nor AAAA");
            break;
        }

        if (hasUses) {
            if (usesAre64) {
                uint8_t uses[8];
                if (!readExact(file, uses, sizeof(uses))) {
                    fail(CacheFileStatus::Truncated, "the file ends inside the usage counter of " + where);
                    break;
                }
                rec.uses = getU64(uses);
            } else {
                uint8_t uses[4];
                if (!readExact(file, uses, sizeof(uses))) {
                    fail(CacheFileStatus::Truncated, "the file ends inside the usage counter of " + where);
                    break;
                }
                rec.uses = getU32(uses);
            }
        }

        const uint8_t addrCount = isA ? countA : countAaaa;
        const size_t addrBytes = isA ? kBytesPerIpv4 : kBytesPerIpv6;
        if (addrCount == 0) {
            // The writer never stores a record without an address of its own
            // type, and the restore would have nothing to insert — the old
            // loader stopped here as well, it just could not say why.
            fail(CacheFileStatus::BadRecord, where + " holds no address");
            break;
        }
        for (uint8_t j = 0; j < addrCount; j++) {
            char raw[kBytesPerIpv6];
            if (!readExact(file, raw, addrBytes)) {
                fail(CacheFileStatus::Truncated, "the file ends inside the addresses of " + where);
                break;
            }
            rec.addrs.emplace_back(raw, addrBytes);
        }
        if (stopped) break;

        out.records++;
        if (handler) handler(rec, i, count);
        // Where the read is, so a page can show the pass instead of the save's
        // last percentage (stage 169).
        if (progress && (out.records % kProgressEveryRecords) == 0) {
            progress(static_cast<uint32_t>(out.records), count);
        }
    }

    if (!stopped) {
        // The file has to end with the last record the header promised: bytes
        // left over mean the header and the body disagree, which is a shape a
        // torn write takes as well (the count is patched at the very end).
        if (fgetc(file) != EOF) {
            stopStatus = CacheFileStatus::TrailingBytes;
            stopWhy = "the file holds more bytes than its " + to_string(count) + " records";
        } else {
            stopStatus = CacheFileStatus::Ok;
        }
    }

    fclose(file);
    out.status = stopStatus;
    out.why = stopWhy;
    // The last word on the pass: even a file that stopped early says how far the
    // read got, and a whole one says it got to the end.
    if (progress && out.records > 0) {
        progress(static_cast<uint32_t>(out.records), count);
    }
    return out;
}

CacheFileCheck CacheFileReader::check(const char* path, size_t expectedRecords,
                                      const ProgressFn& progress)
{
    CacheFileCheck out;

    const CacheFileRead read = CacheFileReader::read(path, nullptr, progress);
    out.records = read.records;

    if (read.status != CacheFileStatus::Ok) {
        out.why = read.why.empty() ? string(statusName(read.status)) : read.why;
        return out;
    }
    if (read.records != expectedRecords) {
        out.why = "the file holds " + to_string(read.records) + " records, " +
                  to_string(expectedRecords) + " were written";
        return out;
    }

    out.ok = true;
    return out;
}

} // namespace dns
} // namespace dhcp
