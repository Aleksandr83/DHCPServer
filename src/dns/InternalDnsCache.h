#ifndef DHCP_DNS_INTERNALDNSCACHE_H
#define DHCP_DNS_INTERNALDNSCACHE_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace dhcp {
namespace dns {

/**
 * @brief Built-in DNS answer cache — hash table stored in external PSRAM.
 *
 * Target: Waveshare ESP32-P4-ETH (32 MB PSRAM). The whole table (bucket heads
 * + a fixed node pool) is allocated as ONE block with heap_caps_malloc(
 * ..., MALLOC_CAP_SPIRAM). On platforms without usable PSRAM the cache is
 * unavailable (available() == false) and every operation is a safe no-op.
 *
 * Chained hash table, FNV-1a over the lowercased domain; the qtype is part of
 * the key. Each node stores up to 16 IPv4 (A) and 8 IPv6 (AAAA) addresses,
 * the original TTL, the store timestamp and a usage counter.
 *
 * Store timestamp (`Node::storedMs`) — "when the record was put in":
 * milliseconds since boot over **32 bits**, refreshed on every store() (a fresh
 * upstream answer makes the record young again). The value is never read as an
 * absolute time: the age is a **modular subtraction** from the clock, written
 * in exactly one place (`elapsedMs()`), so the 49.7-day turnover of a 32-bit
 * millisecond counter cancels out — `now - stored` is the true age of every
 * record whose age is below 49.7 days, which is every record whose TTL is below
 * that limit (a TTL above it, or an age that has to stay exact past it, would
 * need a wider field). The ceiling is accepted for now: the operator decided to
 * lift it by a different mechanism, later, once the pieces that mechanism needs
 * are in place — not by widening this field again. The field answers four
 * questions: is the record expired (aging in `lookup()` and when writing the
 * file), how much of its TTL is left for the client, which record is the oldest
 * when a full pool has to drop one, and how much lifetime to write into the
 * file. It is not the DNS TTL of the answer (that is `ttl`), not a "last used"
 * mark (the record has none — `uses` only counts), and it is not persisted (the
 * file carries the remaining TTL instead). Width: 4 bytes; with the 32-bit
 * usage counter below the record is back to 352 bytes (20 MB: 59 520 entries
 * instead of 58 197).
 *
 * Usage counter (`Node::uses`, 32-bit): how often the name was needed — one
 * per hit in lookup() and one per store(), so every query that involves the
 * cache adds exactly one (a hit is answered from the cache and is not stored,
 * a miss is stored and does not hit). A restored record keeps its saved
 * counter, and the counter survives a refresh of the same name/type; it is
 * zeroed only when the node is handed out of the free pool, so a recycled
 * node can never inherit the count of the record it replaced. The counter
 * **saturates** at UINT32_MAX instead of wrapping through zero (the same rule
 * as the running total of the status): a record that rolled over to 0 would
 * look like the least used one and be evicted first. Two uses of
 * the number: the eviction policy (the least used record goes first, ties
 * broken by the oldest store time) and the “hottest name” the status reports.
 * The status widens the two 32-bit counters to 64-bit fields, and a value
 * restored from the file above UINT32_MAX is clamped.
 *
 * Lookup-path cost: a hit does two saturating increments and, when the record
 * leads the field, records **which** record that is (an index) — the 128-byte
 * name copy happens only in `noteTop()`, which runs when the status is read and
 * never from `lookup()`. The two are separated on purpose: `bumpUse()` is the
 * hot path, `noteTop()` is not.
 *
 * TTL semantics:
 *   - ignoreTtl() == false (default): an entry older than its TTL is a miss
 *     and is purged lazily on lookup (a client refresh happens naturally).
 *   - ignoreTtl() == true: the TTL is stored but never expires an entry; a
 *     dedicated actualization mechanism will be added later.
 *
 * Capacity: when the node pool is full and a NEW domain/type is stored, the
 * oldest entry (smallest store timestamp) is evicted.
 *
 * Thread safety: an internal mutex guards store/lookup/clear/stats so the DNS
 * server task and the REST API (status page) can use the cache concurrently.
 */
class InternalDnsCache {
public:
    InternalDnsCache();
    ~InternalDnsCache();

    InternalDnsCache(const InternalDnsCache&) = delete;
    InternalDnsCache& operator=(const InternalDnsCache&) = delete;

    /**
     * @brief Allocate the PSRAM hash table.
     * @param sizeMb Arena size in megabytes (1..20). If the requested size
     *               exceeds free PSRAM, a smaller allocation is not retried —
     *               enable() fails (available() == false) unless the full
     *               amount could be allocated. Returns false when PSRAM is
     *               missing or the allocation fails.
     */
    bool enable(size_t sizeMb);

    /**
     * @brief Free the arena and reset counters. No-op if not enabled.
     */
    void disable();

    /**
     * @brief Whether the cache is usable (PSRAM arena allocated).
     */
    bool available() const { return arena_ != nullptr; }

    size_t sizeMb() const { return sizeMb_; }

    /**
     * @brief Enable/disable TTL expiry. Off (default) expires entries by TTL;
     * on keeps them until evicted/cleared.
     */
    void setIgnoreTtl(bool ignore) { ignoreTtl_ = ignore; }
    bool ignoreTtl() const { return ignoreTtl_; }

    /**
     * @brief Store (upsert) a domain→IP mapping.
     * @param domain Domain name (case-insensitive, e.g. "example.com").
     * @param qtype  DNS query type (1 = A, 28 = AAAA).
     * @param ips    Textual IP addresses matching @p qtype.
     * @param ttl    Original DNS TTL in seconds.
     */
    void store(const std::string& domain, uint16_t qtype,
               const std::vector<std::string>& ips, uint32_t ttl);

    /**
     * @brief Look up a domain/type mapping.
     * @return true on a hit; @p ips is filled and @p ttl receives the
     *         remaining TTL (when expiry is honored) or the original TTL
     *         (when ignoreTtl is on).
     */
    bool lookup(const std::string& domain, uint16_t qtype,
                std::vector<std::string>& ips, uint32_t& ttl);

    /**
     * @brief Remove every entry.
     */
    void clear();

    /**
     * @brief Put back the measured totals read from the statistics file.
     *
     * The counters behind the split of the average (stages 127/128) live here
     * and are zeroed when the arena is created, so the restore has to happen
     * after enable() — the same rule the usage counters follow, which come back
     * through store(..., countUse = false). Without this the page would show an
     * average that survived the reboot next to a split that starts from zero.
     */
    void restoreMeasurement(uint64_t waitUs, uint64_t walkedNodes, uint64_t stores,
                            uint64_t evictScans, uint64_t evictScanUs,
                            uint64_t evictScanNodes);

    // Progress callback: called periodically with the number of records
    // processed and the estimated total (0 until known). Used to render a
    // progress bar while the background persist task runs. Never called with
    // the arena mutex held for the whole operation (save snapshots node
    // indices under a short lock, then writes without holding the lock, so
    // DNS lookups keep working during a large save).
    typedef void (*ProgressFn)(uint32_t done, uint32_t total, void* ctx);

    /**
     * @brief Persist the whole cache to a binary file (FAT, e.g. /fat/cache.dat).
     *
     * The used node indices are snapshotted under a short arena lock, then the
     * file is written WITHOUT holding the lock (DNS stays responsive). Expired
     * entries are skipped when TTL expiry is honored.
     * @param path VFS path of the destination file.
     * @param entriesWritten Optional out-param with the number of records saved.
     * @param progress Optional callback invoked periodically (done,total).
     * @param progressCtx User pointer passed to @p progress.
     * @param nothingToSave Optional out-param, set to true when the file was not
     * written because there was nothing live to write (an empty table, or one
     * whose entries have all expired). Callers that show the operator what
     * happened must tell that case apart from a write that failed: "nothing to
     * save" is normal on a freshly started device, and reporting it as an error
     * is exactly the kind of lie this parameter exists to prevent.
     * @return false if the cache is disabled, there was nothing to save, or the
     * file could not be written.
     */
    bool saveToFile(const char* path, size_t* entriesWritten = nullptr,
                    ProgressFn progress = nullptr, void* progressCtx = nullptr,
                    bool* nothingToSave = nullptr);

    /**
     * @brief Restore the cache from a file written by saveToFile().
     *
     * Verifies the magic/version, then re-inserts every record via store() with
     * ttl = the stored remaining TTL, so storedMs becomes "now" and the entry
     * expires after the remaining time. Requires the cache to be enabled.
     * @param path VFS path of the source file.
     * @param entriesLoaded Optional out-param with the number of records read.
     * @param progress Optional callback invoked periodically (done,total).
     * @param progressCtx User pointer passed to @p progress.
     * @return false if the file is missing, corrupt, or the cache is disabled.
     */
    bool loadFromFile(const char* path, size_t* entriesLoaded = nullptr,
                      ProgressFn progress = nullptr, void* progressCtx = nullptr);

    struct FileInfo {
        bool     exists = false;
        size_t   size = 0;      // file size in bytes
        size_t   entries = 0;   // record count from the header (0 if invalid)
        uint32_t version = 0;   // format version (0 if invalid)
    };
    /**
     * @brief Read-only header info about a cache file (no arena lock needed).
     */
    FileInfo fileInfo(const char* path) const;

    struct Stats {
        bool available = false;
        size_t sizeMb = 0;     // configured arena size
        size_t capacity = 0;   // node pool size (max entries)
        size_t entries = 0;    // current entries
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evicted = 0;  // purged/evicted entries (overflow + TTL purge)
        // ─── Where the lookup time goes (stage 127, diagnosis) ───
        // The meter in DnsServer times the whole lookup() call, and a lookup
        // starts by taking the arena mutex — so "how slow is a hit" mixes our
        // own work with however long somebody else was holding the lock. These
        // split it. All of them are totals; the page divides by `hits`.
        uint64_t waitUs = 0;       // time spent waiting for the arena mutex
        uint64_t walkedNodes = 0;  // chain nodes examined to find the record
        uint64_t stores = 0;       // records written (insert, refresh, restore)
        uint64_t evictScans = 0;   // full-arena victim scans (the pool was full)
        uint64_t evictScanUs = 0;  // total time spent inside those scans
        uint64_t evictScanNodes = 0;  // nodes those scans visited
        // Usage counters.
        uint64_t usesTotal = 0;  // counted uses since the cache was enabled
        uint64_t usesMax = 0;    // high-water mark of one record's counter
        // The name behind usesMax (empty when nothing has been counted yet).
        // It is a high-water mark, not a live reading: the record may have
        // been evicted since, in which case the mark stays until another name
        // passes it. Cheaper than a scan of the whole pool on every poll,
        // which would hold the arena lock and stall DNS.
        std::string topName;
        uint16_t topQtype = 0;
        // Approximate data bytes actually stored / free in the node pool.
        size_t usedBytes = 0;   // entries × node size
        size_t freeBytes = 0;   // (capacity − entries) × node size
    };
    Stats stats() const;

private:
    // Fixed layout node — keeps the arena layout predictable.
    struct alignas(8) Node {
        int32_t  next;      // next node index in the bucket chain (-1 = end)
        uint32_t hash;      // FNV-1a of the lowercased domain
        uint16_t qtype;     // DNS record type
        uint16_t nA;        // number of IPv4 addresses
        uint16_t nAAAA;     // number of IPv6 addresses
        uint16_t _pad;
        // Store timestamp: milliseconds since boot, 32-bit — the age is a
        // modular subtraction from the clock, so the 49.7-day turnover of this
        // counter cancels out (see the class comment and elapsedMs()).
        uint32_t storedMs;
        uint32_t ttl;       // original TTL (seconds)
        // Usage counter, 0 on a node handed out of the pool. Saturates at
        // UINT32_MAX. The cache file writes it as 4 bytes (format version 3),
        // matching the field; a version 2 file, which carries 8, still loads.
        uint32_t uses;
        char     name[128]; // lowercased domain, NUL-terminated
        uint32_t a4[16];    // up to 16 IPv4 (network byte order)
        uint8_t  a6[8][16]; // up to 8 IPv6
    };

    static uint32_t hashName(const char* s);
    static std::string lower(const std::string& s);
    // Milliseconds since boot, truncated to 32 bits. Never compare two of these
    // as absolute times — take the difference with elapsedMs().
    static uint32_t nowMs();
    // The age of a record in milliseconds. This is the ONE place where the
    // modulo of the 32-bit millisecond clock lives: unsigned subtraction already
    // yields the correct difference across the turnover, as long as the real age
    // is below 49.7 days (which every TTL below that limit guarantees).
    static uint32_t elapsedMs(uint32_t now, uint32_t stored) {
        return now - stored;
    }

    // store() and loadFromFile() share this body; they differ only in what
    // happens to the usage counter:
    //   countUse == true  → the name was just needed: counter grows by one
    //   countUse == false → a restore: counter is written as usesExact
    void storeInternal(const std::string& domain, uint16_t qtype,
                       const std::vector<std::string>& ips, uint32_t ttl,
                       bool countUse, uint64_t usesExact);
    // Hot path: saturating +1 on the record, +1 on the running total, and (when
    // the record leads the field) remember WHICH record leads — an index and a
    // qtype, no name copy.
    void bumpUse(Node& n, int idx);
    // The mark itself, from a record whose counter was just written: which
    // record leads and by how much. O(1), no copy. Used by the restore path
    // (a restored counter is what it is — not a use now).
    void noteMark(const Node& n, int idx);
    // The mark's name. This is where the 128 bytes are copied, and it is called
    // when the status is read — never from the lookup path. Const because it
    // writes the cache's own mark (mutable), not the pool.
    void noteTop() const;
    void resetTop();

    uint32_t bucketOf(uint32_t hash) const { return hash % numBuckets_; }
    // @param walked  Optional: how many chain nodes the walk examined (the
    //                lookup path reports it, so the load factor stops being a
    //                guess — stage 127).
    int  findNode(uint32_t bucket, uint32_t hash,
                  const char* name, uint16_t qtype,
                  uint32_t* walked = nullptr) const;
    int  allocNode();
    void freeNode(int idx);
    // Eviction victim: the least used record, ties broken by the oldest store
    // time (a record never hit has uses == 1 and the oldest one goes first).
    // `now` is the caller's clock reading: "oldest" is compared as an age
    // (modular), never as two absolute timestamps.
    // @param walked  Optional: how many records the scan visited (it walks the
    //                whole arena, so this is the pool size — stage 127).
    int  findEvictVictim(uint32_t now, int* bucketOut,
                         uint32_t* walked = nullptr) const;
    void unlinkNode(uint32_t bucket, int idx);
    void lock() const;
    void unlock() const;

    uint8_t* arena_ = nullptr;
    size_t   sizeMb_ = 0;
    size_t   arenaBytes_ = 0;
    int32_t* buckets_ = nullptr;  // head indices per bucket (arena)
    uint32_t numBuckets_ = 0;
    Node*    nodes_ = nullptr;    // node pool (arena)
    int32_t  nodeCount_ = 0;
    int32_t  freeHead_ = -1;      // free-node stack head
    size_t   entries_ = 0;
    mutable uint64_t hits_ = 0;
    mutable uint64_t misses_ = 0;
    uint64_t evicted_ = 0;
    // Where the lookup time went (stage 127). The wait and the walk are counted
    // for hits only, so they divide by hits_ and stay comparable with the
    // average DnsServer reports; the store/scan counters are their own story.
    mutable uint64_t lookupWaitUs_ = 0;
    mutable uint64_t walkedNodes_ = 0;
    uint64_t stores_ = 0;
    uint64_t evictScans_ = 0;
    uint64_t evictScanUs_ = 0;
    uint64_t evictScanNodes_ = 0;
    // Usage bookkeeping (see the class comment): a monotonic total and the
    // high-water "hottest name" mark, both maintained in O(1) per use so that
    // stats() never walks the pool. The mark's name is materialised separately.
    uint32_t usesTotal_ = 0;
    uint32_t topUses_ = 0;
    int32_t  topIdx_ = -1;              // record behind the mark (-1 = none)
    uint16_t topQtype_ = 0;
    mutable char topName_[128] = {0};   // written by noteTop(), from stats()
    bool     topValid_ = false;
    bool     ignoreTtl_ = false;
    void*    mutex_ = nullptr;    // SemaphoreHandle_t
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_INTERNALDNSCACHE_H
