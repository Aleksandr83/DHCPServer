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
 * @brief The three numbers the main page shows, and what they need to survive a
 *        reboot.
 *
 * `hits` and `forwards` are what the page prints as "From cache" and "Forward";
 * the average hit time is **derived** from @ref hitUsSum and @ref hits rather
 * than stored, because a stored average cannot be continued: after a reboot the
 * new queries would be averaged against an average, which is no longer an
 * average of anything. Keeping the sum makes the number honest across restarts
 * — (sum + new) / (hits + new) — and the mechanism is meant to grow from here.
 */
struct DnsStatTotals {
    uint64_t hits = 0;       ///< Queries answered from the internal cache
    uint64_t forwards = 0;   ///< Queries forwarded upstream
    uint64_t hitUsSum = 0;   ///< Sum of the hit durations in microseconds

    /** @brief Average hit time in microseconds (`0` while nothing was counted). */
    uint32_t avgHitUs() const
    {
        return (hits == 0) ? 0u : static_cast<uint32_t>(hitUsSum / hits);
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
 *   4       4     version (little-endian)
 *   8       4     payload size in bytes (24 — a size mismatch is a corrupt file)
 *   12      4     reserved (0; the next growth of the format uses it)
 *   16      8     hits
 *   24      8     forwards
 *   32      8     sum of the hit durations in microseconds
 *   40      4     checksum (sum of the bytes before it, mod 2^32)
 * ```
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
    static constexpr uint32_t kVersion = 1;

    /** @brief Size of one record, in bytes. */
    static constexpr uint32_t kRecordSize = 44;

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
