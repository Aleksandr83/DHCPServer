#ifndef DHCP_DNS_CACHEFILEREADER_H
#define DHCP_DNS_CACHEFILEREADER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dhcp {
namespace dns {

/**
 * @brief One record read from a `cache.dat` (stage 169).
 *
 * The addresses travel as the bytes the file holds — 4 per IPv4, 16 per IPv6 —
 * so this module stays free of lwIP and can be compiled on the host. Turning
 * them into text is the caller's business (`InternalDnsCache::loadFromFile`
 * does it with `inet_ntop`).
 */
struct CacheFileRecord {
    std::string name;                  ///< Domain name, as stored
    uint16_t qtype = 0;                ///< 1 = A, 28 = AAAA
    uint32_t ttlRemaining = 0;         ///< Seconds of life left at save time
    uint64_t uses = 0;                 ///< Usage counter (0 in a version 1 file)
    std::vector<std::string> addrs;    ///< Raw address bytes, in file order
};

/** @brief How a read of a cache file ended. */
enum class CacheFileStatus {
    Ok,                  ///< Every record parsed, the file ends with the last one
    NotFound,            ///< No such file (a device that never saved has none)
    BadHeader,           ///< Shorter than a header, or not a cache file at all
    UnsupportedVersion,  ///< A version this build does not know
    ImplausibleCount,    ///< The header claims more records than a file this size can hold
    Truncated,           ///< The file ends inside a record
    BadRecord,           ///< A record whose fields cannot be true (no name, no address, …)
    TrailingBytes,       ///< Bytes after the last record the header promised
};

/** @brief Everything a read found, including why it stopped. */
struct CacheFileRead {
    CacheFileStatus status = CacheFileStatus::NotFound;
    uint32_t version = 0;        ///< Format version from the header (0 when unread)
    uint32_t headerCount = 0;    ///< Records the header promises
    size_t sizeBytes = 0;        ///< File size (0 when it could not be measured)
    size_t records = 0;          ///< Records actually parsed, whole ones only
    std::string why;             ///< English reason, with numbers — the log and the dialog read it
};

/**
 * @brief Result of the structural check a planned restart asks for (stage 169).
 *
 * `ok` means: the file is there, its header is ours, every record parses, the
 * file ends where the last record ends, and the records are exactly the ones
 * the save reported writing.
 */
struct CacheFileCheck {
    bool ok = false;
    size_t records = 0;
    std::string why;
};

/**
 * @brief Reader of the built-in cache's file, and the definition of its format.
 *
 * The format used to be described only by the writer
 * (`InternalDnsCache::saveToFile`); a restart now has to read a saved file back
 * and say whether it holds what was written (stage 169), so the layout lives
 * here and both sides use it:
 *
 * ```
 *   header 16 B: magic "DCC1" | u32 version | u32 recordCount | u32 reserved(0)
 *   per record:
 *     u8  nameLen, name[nameLen]
 *     u16 qtype, u8 nA, u8 nAAAA, u32 ttlRemainingSec
 *     u32 uses                      (version 3; version 2 has it as u64,
 *                                    version 1 has no counter at all)
 *     nA    × 4 B  (IPv4, network byte order)
 *     nAAAA × 16 B (IPv6)
 * ```
 *
 * Reading **never touches the cache arena** and never inserts anything: the
 * same reader serves the boot-time restore and the read-back check, which is
 * the point — a check that used a different parser than the restore could pass
 * on a file the restore then refuses.
 *
 * Deliberately free of ESP-IDF (plain `fopen` on the VFS), which is what lets
 * the format and the check be tested on the host.
 */
class CacheFileReader {
public:
    // ─── The format, in one place (rule 39: no bare numbers) ───
    static constexpr size_t kHeaderBytes = 16;      // magic, version, count, reserved
    static constexpr char kMagic[] = "DCC1";
    static constexpr size_t kMagicBytes = 4;
    static constexpr size_t kVersionOffset = 4;     // u32: format version
    static constexpr size_t kCountOffset = 8;       // u32: record count
    static constexpr size_t kReservedOffset = 12;   // u32: unused, written as 0
    static constexpr size_t kTailBytes = 8;         // qtype(2) + nA + nAAAA + ttl(4)
    static constexpr uint32_t kVersion = 3;         // written now (4-byte usage counter)
    static constexpr uint32_t kVersionMin = 1;      // readable: 1, 2 and 3
    static constexpr uint32_t kVersionUses64 = 2;   // up to this version the counter is 8 B
    static constexpr size_t kMaxNameBytes = 127;    // fits Node::name[128] with the NUL
    static constexpr uint32_t kMaxPlausibleRecords = 2000000;  // ~20 MB of records
    static constexpr uint8_t kMaxA = 16;            // IPv4 addresses one record may hold
    static constexpr uint8_t kMaxAaaa = 8;          // IPv6 addresses one record may hold
    static constexpr uint16_t kTypeA = 1;           // QTYPE of an IPv4 answer
    static constexpr uint16_t kTypeAaaa = 28;       // QTYPE of an IPv6 answer

    /**
     * @brief Called for every whole record, in file order.
     *
     * @param record What was read.
     * @param index  0-based position of this record.
     * @param count  How many records the header promises (the total).
     */
    using RecordHandler = std::function<void(const CacheFileRecord& record,
                                             uint32_t index, uint32_t count)>;

    /**
     * @brief Called while the file is being read: records so far, and the total.
     *
     * A planned restart reads the saved file back, and on a table of a few
     * megabytes that pass takes seconds — long enough that the operator has to be
     * able to see it. The reader reports its own progress for exactly that reason
     * (stage 169): the check is a second pass over the file, so a page that only
     * knew the save's percentage would sit at "100 %" while the device was still
     * reading it.
     *
     * @param done  Records read so far (whole ones only).
     * @param total Records the header promises (`0` when the header was never
     *              read, so there is nothing to measure).
     */
    using ProgressFn = std::function<void(uint32_t done, uint32_t total)>;

    /**
     * @brief Read @p path from beginning to end.
     *
     * @param handler Called per record; may be empty when only the outcome is
     *                wanted (that is what check() does).
     * @return What was found. `status == Ok` is the only state in which the
     *         file held exactly what its header promised.
     */
    static CacheFileRead read(const char* path, const RecordHandler& handler,
                              const ProgressFn& progress = ProgressFn());

    /**
     * @brief Read @p path back and compare it with what a save reported writing.
     *
     * @param expectedRecords The count `saveToFile()` returned for this file.
     * @param progress Optional: the read-back pass reports its own progress, so a
     *                 page can say "checking" instead of freezing at the save's
     *                 last percentage.
     * @return `ok` only when every record parsed, the file ends with the last
     *         one, and there are exactly @p expectedRecords of them. `why` names
     *         the difference in words a page can show.
     */
    static CacheFileCheck check(const char* path, size_t expectedRecords,
                                const ProgressFn& progress = ProgressFn());

    /// @brief Short English name of a status (logs and test output).
    static const char* statusName(CacheFileStatus status);
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_CACHEFILEREADER_H
