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
 * the original TTL and the store timestamp.
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
     * @return false if the cache is disabled or the file could not be written.
     */
    bool saveToFile(const char* path, size_t* entriesWritten = nullptr,
                    ProgressFn progress = nullptr, void* progressCtx = nullptr);

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
        uint32_t storedMs;  // store timestamp (ms)
        uint32_t ttl;       // original TTL (seconds)
        char     name[128]; // lowercased domain, NUL-terminated
        uint32_t a4[16];    // up to 16 IPv4 (network byte order)
        uint8_t  a6[8][16]; // up to 8 IPv6
    };

    static uint32_t hashName(const char* s);
    static std::string lower(const std::string& s);
    static uint64_t nowMs();

    uint32_t bucketOf(uint32_t hash) const { return hash % numBuckets_; }
    int  findNode(uint32_t bucket, uint32_t hash,
                  const char* name, uint16_t qtype) const;
    int  allocNode();
    void freeNode(int idx);
    int  findOldestUsed(int* bucketOut) const;
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
    bool     ignoreTtl_ = false;
    void*    mutex_ = nullptr;    // SemaphoreHandle_t
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_INTERNALDNSCACHE_H
