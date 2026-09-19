#ifndef DHCP_DNS_DNSSTATSTORE_H
#define DHCP_DNS_DNSSTATSTORE_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace core {
class ErrorLogCore;
}
} // namespace dhcp

namespace dhcp {
namespace dns {

/**
 * @brief The numbers the main page shows, and what they need to survive a
 *        reboot.
 *
 * `hits` and `forwards` are what the page prints as "From cache" and "Forward";
 * the average hit time is **derived** from @ref hitUsSum and @ref hits rather
 * than stored, because a stored average cannot be continued: after a reboot the
 * new queries would be averaged against an average, which is no longer an
 * average of anything. Keeping the sum makes the number honest across restarts
 * — (sum + new) / (hits + new) — and the mechanism is meant to grow from here.
 *
 * It grew in stage 128: the six sums behind the split of that average (waiting
 * for the arena mutex, chain nodes walked, records written, and what the
 * full-arena eviction scan costs) travel with it, for exactly the same reason.
 */
struct DnsStatTotals {
    uint64_t hits = 0;       ///< Queries answered from the internal cache
    uint64_t forwards = 0;   ///< Queries forwarded upstream
    uint64_t hitUsSum = 0;   ///< Sum of the hit durations in microseconds
    // Stage 128 — the split, summed rather than averaged (see above).
    uint64_t waitUs = 0;          ///< Sum of the time those hits waited for the lock
    uint64_t walkedNodes = 0;     ///< Sum of the chain nodes those hits walked
    uint64_t stores = 0;          ///< Records written (insert, refresh, restore)
    uint64_t evictScans = 0;      ///< Full-arena eviction scans (a full pool)
    uint64_t evictScanUs = 0;     ///< Sum of the time spent inside those scans
    uint64_t evictScanNodes = 0;  ///< Nodes those scans visited

    /** @brief Average hit time in microseconds (`0` while nothing was counted). */
    uint32_t avgHitUs() const
    {
        return (hits == 0) ? 0u : static_cast<uint32_t>(hitUsSum / hits);
    }

    /** @brief Average wait of a hit for the arena mutex, in microseconds. */
    uint32_t avgWaitUs() const
    {
        return (hits == 0) ? 0u : static_cast<uint32_t>(waitUs / hits);
    }

    /** @brief Average chain length of a hit, in hundredths of a node (110 = 1.1). */
    uint32_t walkX100() const
    {
        return (hits == 0) ? 0u : static_cast<uint32_t>((walkedNodes * 100u) / hits);
    }
};

/**
 * @brief Reader and writer of `Statistica.dat` on the internal volume.
 *
 * The file is a **fixed-size record** with a magic, a version and a checksum:
 *
 * ```
 *   offset  size  field
 *   0       4     magic "DST1"
 *   4       4     version (little-endian; 1 = three counters, 2 = all nine)
 *   8       4     payload size in bytes (24 for version 1, 72 for version 2 —
 *                 a size that does not match its version is a corrupt file)
 *   12      4     reserved (0; the next growth of the format uses it)
 *   16      8     hits
 *   24      8     forwards
 *   32      8     sum of the hit durations in microseconds
 *   40      8     sum of the time those hits waited for the arena mutex
 *   48      8     sum of the chain nodes those hits walked
 *   56      8     records written (insert, refresh, restore)
 *   64      8     full-arena eviction scans
 *   72      8     time spent inside those scans, microseconds
 *   80      8     nodes those scans visited
 *   88      4     checksum (sum of the bytes before it, mod 2^32)
 * ```
 *
 * Version 1 records (44 bytes) are still read: their three counters come back
 * and the six newer sums stay zero. That is what lets the operator's existing
 * `Statistica.dat` survive this build — dropping it would throw away the only
 * history the average has.
 *
 * Why not just dump a struct: the record has to be readable after a partial
 * write (power cut in the middle of a save), after a version change, and on a
 * volume that was never written — and a memory image of a struct gives no way to
 * tell "damaged" from "plausible". The magic, the size and the checksum together
 * make that decision explicit, and the version field is what lets the format grow
 * (the operator already plans to).
 *
 * The file is written **in place**, like `cache.dat`, and not published through a
 * temporary file: FatFS refuses a rename onto an existing name (see save()). A
 * torn write is therefore possible in principle, which is exactly why the checksum
 * is not decoration — `load()` refuses a damaged record instead of trusting it.
 *
 * Deliberately free of ESP-IDF (plain `fopen` on the VFS), which is what lets the
 * format be tested on the host (see `test/test_dnsstatstore.cpp`).
 */
class DnsStatStore {
public:
    /** @brief Where the statistics live on the device (internal FAT volume). */
    static constexpr const char* kPath = "/fat/Statistica.dat";

    /** @brief Format version this build writes. */
    static constexpr uint32_t kVersion = 2;

    /** @brief Size of one record this build writes, in bytes. */
    static constexpr uint32_t kRecordSize = 92;

    /** @brief Version 1 records: still read, never written (44 bytes). */
    static constexpr uint32_t kVersionLegacy = 1;
    static constexpr uint32_t kRecordSizeLegacy = 44;

    /// Rule 39: the record layout used to live only in the table above.
    static constexpr uint32_t kVersionOffset = 4;      // u32: format version
    static constexpr uint32_t kPayloadSizeOffset = 8;  // u32: bytes of counters
    static constexpr uint32_t kHeaderBytes = 16;       // version, size, reserved
    static constexpr uint32_t kChecksumBytes = 4;      // trailing checksum
    static constexpr uint32_t kPayloadV1Bytes = 24;    // three 64-bit counters
    static constexpr uint32_t kPayloadV2Bytes = 72;    // nine 64-bit counters
    static constexpr uint32_t kOffsetHits = 16;        // the counters, in order
    static constexpr uint32_t kOffsetForwards = 24;
    static constexpr uint32_t kOffsetHitUsSum = 32;
    static constexpr uint32_t kOffsetWaitUs = 40;
    static constexpr uint32_t kOffsetWalkedNodes = 48;
    static constexpr uint32_t kOffsetStores = 56;

    /** @brief Serialise @p totals into exactly @ref kRecordSize bytes. */
    static std::string encode(const DnsStatTotals& totals);

    /**
     * @brief Parse a record.
     *
     * @param[out] out The totals, untouched when the record is refused.
     * @param[out] why Optional English reason (goes to the log).
     * @return false for a foreign, truncated, damaged or future-version file.
     */
    static bool decode(const std::string& record, DnsStatTotals& out,
                       std::string* why = nullptr);

    /**
     * @brief Write @p totals to @p path — in place, no temporary file.
     *
     * Pressing the same path again is normal and must work: that is what the
     * temporary-file publish broke on FatFS (a rename onto an existing name is
     * refused), and it is why this function is as simple as it looks.
     *
     * @return false on any filesystem error; @p why carries the reason
     *         (`cannot create the file`, `write failed`).
     */
    static bool save(const std::string& path, const DnsStatTotals& totals,
                     std::string* why = nullptr);

    /**
     * @brief Save, and when that fails remove the file and write it once more.
     *
     * The operator asked for exactly this: a 44-byte record that cannot be
     * written is worth a second attempt, and the attempt has to start from a
     * clean slate — so the destination is removed first (together with a `.tmp`
     * left over from the builds that used one).
     *
     * Both attempts and both reasons go to the error log, including the price of
     * the policy: after a failure the record that was there is gone, so a second
     * failure leaves no statistics file at all — the counters then start from zero
     * at boot. That is the trade the operator chose, and it is written down here so
     * nobody has to guess where his file went.
     *
     * @param log where to report (nullptr = no log, the return value still
     *            answers what happened — the host test uses a fake).
     */
    static bool saveWithRetry(const std::string& path, const DnsStatTotals& totals,
                              std::string* why, core::ErrorLogCore* log);

    /**
     * @brief Read @p path.
     *
     * A missing file is not an error worth a warning — it is the normal state of
     * a device that has never saved statistics — but it does return false.
     */
    static bool load(const std::string& path, DnsStatTotals& out,
                     std::string* why = nullptr);

    /** @brief Delete the file (a factory reset must not keep statistics). */
    static bool remove(const std::string& path);

    /**
     * @brief Is there a file at @p path?
     *
     * Used to tell "a device that never saved" (normal) from "a file that is there
     * and cannot be read" (worth a line in the error log, see
     * DnsServer::restoreStatsFromFile).
     */
    static bool exists(const std::string& path);
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSSTATSTORE_H
