#include "InternalDnsCache.h"

#include <cstring>
#include <cctype>
#include <cstdio>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char* TAG = "InternalCache";

namespace dhcp {
namespace dns {

namespace {
constexpr size_t kMaxNameLen = 127;      // chars (fits Node::name[128])
constexpr size_t kMaxA    = 16;          // max IPv4 addresses per entry
constexpr size_t kMaxAAAA = 8;           // max IPv6 addresses per entry
} // namespace

// ─────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────

InternalDnsCache::InternalDnsCache()
{
    mutex_ = xSemaphoreCreateMutex();
}

InternalDnsCache::~InternalDnsCache()
{
    disable();
    if (mutex_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(mutex_));
    mutex_ = nullptr;
}

// ─────────────────────────────────────────────────────
// enable / disable
// ─────────────────────────────────────────────────────

bool InternalDnsCache::enable(size_t sizeMb)
{
    lock();
    if (arena_) {  // already enabled — just update the reported size
        sizeMb_ = sizeMb;
        unlock();
        return true;
    }
    if (sizeMb < 1) sizeMb = 1;
    if (sizeMb > 20) sizeMb = 20;  // cap 20 MB (fits cache.dat on the FAT partition)

    const size_t nodeSize = sizeof(Node);
    const size_t arenaBytes = sizeMb * 1024 * 1024;

    // Bucket table ~ one 4-byte head per 4 KiB of arena (a small fraction),
    // the rest is the fixed node pool.
    uint32_t numBuckets = static_cast<uint32_t>(arenaBytes / 4096);
    if (numBuckets < 1024) numBuckets = 1024;
    const size_t bucketBytes = static_cast<size_t>(numBuckets) * sizeof(int32_t);
    if (bucketBytes >= arenaBytes) {
        ESP_LOGW(TAG, "Arena %u MB too small for the bucket table", (unsigned)sizeMb);
        unlock();
        return false;
    }
    const size_t nodeBytes = arenaBytes - bucketBytes;
    int32_t nodeCount = static_cast<int32_t>(nodeBytes / nodeSize);
    if (nodeCount < 1) {
        ESP_LOGW(TAG, "Arena %u MB too small for a single node", (unsigned)sizeMb);
        unlock();
        return false;
    }

    // Allocate the whole table in PSRAM.
    uint8_t* arena = static_cast<uint8_t*>(
        heap_caps_malloc(arenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arena) {
        ESP_LOGW(TAG, "PSRAM unavailable or %u MB allocation failed "
                 "(internal DNS cache disabled)",
                 (unsigned)sizeMb);
        unlock();
        return false;
    }
    memset(arena, 0, arenaBytes);

    arena_ = arena;
    arenaBytes_ = arenaBytes;
    sizeMb_ = sizeMb;
    buckets_ = reinterpret_cast<int32_t*>(arena);
    numBuckets_ = numBuckets;
    nodes_ = reinterpret_cast<Node*>(arena + bucketBytes);
    nodeCount_ = nodeCount;

    // Initialise the bucket heads. The arena comes from memset(0), so an empty
    // bucket would otherwise read **0** — and 0 is a perfectly valid node index.
    // That made every unused bucket a chain into node 0: while node 0 was still
    // free it happened to terminate (its `next` was the free-list link, so walks
    // crawled the free list first), but as soon as the pool filled up and node 0
    // became a real record, storing into an empty bucket wrote
    // `n.next = buckets_[b] = 0` — the node pointed at itself and every chain
    // walk (a lookup, an eviction) spun forever. `-1` is the terminator the walk
    // loops already expect, and `clear()` has always set it.
    for (uint32_t i = 0; i < numBuckets_; i++) buckets_[i] = -1;

    // Initialise the free-node stack.
    freeHead_ = -1;
    for (int32_t i = 0; i < nodeCount_; i++) {
        nodes_[i].next = freeHead_;
        freeHead_ = i;
    }
    entries_ = 0;
    hits_ = 0;
    misses_ = 0;
    evicted_ = 0;
    usesTotal_ = 0;
    resetTop();

    ESP_LOGI(TAG, "Internal DNS cache enabled: %u MB arena, %u buckets, "
             "%d entries max (%u bytes PSRAM used)",
             (unsigned)sizeMb_, (unsigned)numBuckets_, (int)nodeCount_,
             (unsigned)arenaBytes_);
    unlock();
    return true;
}

void InternalDnsCache::disable()
{
    lock();
    if (arena_) heap_caps_free(arena_);
    arena_ = nullptr;
    buckets_ = nullptr;
    nodes_ = nullptr;
    nodeCount_ = 0;
    numBuckets_ = 0;
    freeHead_ = -1;
    entries_ = 0;
    arenaBytes_ = 0;
    sizeMb_ = 0;
    resetTop();   // no arena — nothing to report about
    unlock();
}

// ─────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────

uint64_t InternalDnsCache::nowMs()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

std::string InternalDnsCache::lower(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

uint32_t InternalDnsCache::hashName(const char* s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
         *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

int InternalDnsCache::allocNode()
{
    if (freeHead_ < 0) return -1;
    int idx = freeHead_;
    freeHead_ = nodes_[idx].next;
    nodes_[idx].next = -1;
    return idx;
}

void InternalDnsCache::freeNode(int idx)
{
    nodes_[idx].next = freeHead_;
    freeHead_ = idx;
}

int InternalDnsCache::findNode(uint32_t bucket, uint32_t hash,
                               const char* name, uint16_t qtype) const
{
    for (int idx = buckets_[bucket]; idx >= 0; idx = nodes_[idx].next) {
        const Node& n = nodes_[idx];
        if (n.hash == hash && n.qtype == qtype &&
            strcmp(n.name, name) == 0) {
            return idx;
        }
    }
    return -1;
}

// Walk every bucket chain and return the used node with the lowest usage
// counter (ties: the oldest store time). Used for overflow eviction — the
// record nobody asks for goes first, not merely the one stored earliest.
int InternalDnsCache::findEvictVictim(int* bucketOut) const
{
    int best = -1;
    uint64_t bestUses = UINT64_MAX;
    uint64_t bestMs = UINT64_MAX;   // storedMs cannot turn over, so smallest = oldest
    for (uint32_t b = 0; b < numBuckets_; b++) {
        for (int idx = buckets_[b]; idx >= 0; idx = nodes_[idx].next) {
            const Node& n = nodes_[idx];
            if (best >= 0 && n.uses > bestUses) continue;
            if (n.uses == bestUses && n.storedMs >= bestMs) continue;
            bestUses = n.uses;
            bestMs = n.storedMs;
            best = idx;
            if (bucketOut) *bucketOut = static_cast<int>(b);
        }
    }
    return best;
}

void InternalDnsCache::unlinkNode(uint32_t bucket, int idx)
{
    int prev = -1;
    for (int cur = buckets_[bucket]; cur >= 0; cur = nodes_[cur].next) {
        if (cur == idx) {
            if (prev < 0) {
                buckets_[bucket] = nodes_[idx].next;
            } else {
                nodes_[prev].next = nodes_[idx].next;
            }
            freeNode(idx);
            entries_--;
            return;
        }
        prev = cur;
    }
}

void InternalDnsCache::lock() const
{
    if (mutex_) xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_),
                               portMAX_DELAY);
}

void InternalDnsCache::unlock() const
{
    if (mutex_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

// ─────────────────────────────────────────────────────
// store / lookup
// ─────────────────────────────────────────────────────

void InternalDnsCache::store(const std::string& domain, uint16_t qtype,
                             const std::vector<std::string>& ips,
                             uint32_t ttl)
{
    // A query needed this name (either because the cache missed, or because a
    // local/upstream answer is being kept) — that is one use of the record.
    storeInternal(domain, qtype, ips, ttl, /*countUse=*/true, 0);
}

void InternalDnsCache::storeInternal(const std::string& domain, uint16_t qtype,
                                     const std::vector<std::string>& ips,
                                     uint32_t ttl, bool countUse,
                                     uint64_t usesExact)
{
    if (!arena_) return;
    if (domain.empty() || ips.empty()) return;
    // Only A (IPv4) and AAAA (IPv6) answers are cached.
    if (qtype != 1 && qtype != 28) return;

    const std::string lname = lower(domain);
    if (lname.size() >= kMaxNameLen) return;

    lock();

    uint32_t h = hashName(lname.c_str());
    uint32_t b = bucketOf(h);
    const uint64_t now = nowMs();

    // Upsert existing entry (domain + type).
    int idx = findNode(b, h, lname.c_str(), qtype);
    if (idx < 0) {
        // New entry — need a free node (evict the least used if the pool is
        // full).
        idx = allocNode();
        if (idx < 0) {
            int oldBucket = 0;
            int victim = findEvictVictim(&oldBucket);
            if (victim < 0) {  // pool full but nothing to evict — should not happen
                unlock();
                return;
            }
            unlinkNode(static_cast<uint32_t>(oldBucket), victim);
            evicted_++;
            idx = allocNode();
        }
        if (idx < 0) {
            unlock();
            return;
        }
        Node& n = nodes_[idx];
        n.hash = h;
        n.qtype = qtype;
        n.uses = 0;   // a recycled node must not inherit the previous count
        n.storedMs = now;
        memcpy(n.name, lname.c_str(), lname.size() + 1);
        n.next = buckets_[b];
        buckets_[b] = idx;
        entries_++;
    }

    Node& n = nodes_[idx];
    n.storedMs = now;
    n.ttl = ttl;
    n.nA = 0;
    n.nAAAA = 0;

    if (qtype == 1) {  // A
        for (const auto& ip : ips) {
            if (n.nA >= kMaxA) break;
            inet_pton(AF_INET, ip.c_str(), &n.a4[n.nA]);
            n.nA++;
        }
    } else if (qtype == 28) {  // AAAA
        for (const auto& ip : ips) {
            if (n.nAAAA >= kMaxAAAA) break;
            inet_pton(AF_INET6, ip.c_str(), n.a6[n.nAAAA]);
            n.nAAAA++;
        }
    }

    // Counter last, so it sees the final record: a refresh keeps the old count
    // and adds today's use; a restore writes the value that was saved.
    if (countUse) {
        bumpUse(n);
    } else {
        n.uses = usesExact;
        noteTop(n);
    }

    unlock();
}

// Saturating +1 on a record's counter, plus the O(1) bookkeeping the status
// needs: the running total and the high-water "hottest name" mark.
//
// Both counters **saturate**: a counter that has reached its maximum stays
// there instead of wrapping through zero. For a usage counter a wrap is not
// just a wrong number — `uses` is what the eviction orders by, so a record
// that rolled over to 0 would look like the least used one and be thrown out
// first, and the running total would report that names were never used.
void InternalDnsCache::bumpUse(Node& n)
{
    if (n.uses != UINT64_MAX) n.uses++;
    if (usesTotal_ != UINT64_MAX) usesTotal_++;
    noteTop(n);
}

// A record whose counter reached the highest value so far becomes the new
// mark. "The same value twice" also refreshes the mark, so the reported name
// is one that is actually in the cache at the moment it was passed.
void InternalDnsCache::noteTop(const Node& n)
{
    if (n.uses == 0) return;   // a record nobody used is not a candidate
    if (topValid_ && n.uses < topUses_) return;
    topUses_ = n.uses;
    topQtype_ = n.qtype;
    memcpy(topName_, n.name, sizeof(topName_));
    topName_[sizeof(topName_) - 1] = '\0';
    topValid_ = true;
}

void InternalDnsCache::resetTop()
{
    topUses_ = 0;
    topQtype_ = 0;
    topName_[0] = '\0';
    topValid_ = false;
}

bool InternalDnsCache::lookup(const std::string& domain, uint16_t qtype,
                              std::vector<std::string>& ips, uint32_t& ttl)
{
    if (!arena_) return false;
    if (domain.empty()) return false;

    const std::string lname = lower(domain);
    if (lname.size() >= kMaxNameLen) return false;

    lock();

    uint32_t h = hashName(lname.c_str());
    uint32_t b = bucketOf(h);
    int idx = findNode(b, h, lname.c_str(), qtype);

    if (idx < 0) {
        misses_++;
        unlock();
        return false;
    }

    Node& n = nodes_[idx];
    // One age for both decisions below (two clock reads could straddle a
    // millisecond and disagree).
    const uint64_t elapsed = nowMs() - n.storedMs;

    // Honoring TTL: an entry older than its TTL is a miss and is purged.
    if (!ignoreTtl_ && n.ttl != 0 && elapsed / 1000ULL >= n.ttl) {
        unlinkNode(b, idx);
        evicted_++;
        misses_++;
        unlock();
        return false;
    }

    ips.clear();
    if (qtype == 1) {
        for (uint16_t i = 0; i < n.nA; i++) {
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &n.a4[i], buf, sizeof(buf))) {
                ips.emplace_back(buf);
            }
        }
    } else if (qtype == 28) {
        for (uint16_t i = 0; i < n.nAAAA; i++) {
            char buf[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, n.a6[i], buf, sizeof(buf))) {
                ips.emplace_back(buf);
            }
        }
    }

    if (ips.empty()) {
        misses_++;
        unlock();
        return false;
    }

    // Remaining TTL (when expiry is honored) or the original TTL (ignore).
    // The expiry check above guarantees elapsed < ttl*1000, so this is not
    // negative.
    if (ignoreTtl_ || n.ttl == 0) {
        ttl = n.ttl;
    } else {
        const uint64_t remainMs = (static_cast<uint64_t>(n.ttl) * 1000ULL) - elapsed;
        ttl = static_cast<uint32_t>((remainMs + 999) / 1000ULL);  // ceil to seconds
    }

    // The record served the client — that is one use of this name (an expired
    // or empty answer above returned early and is not counted).
    bumpUse(n);
    hits_++;
    unlock();
    return true;
}

void InternalDnsCache::clear()
{
    lock();
    if (!arena_) {
        unlock();
        return;
    }
    for (uint32_t b = 0; b < numBuckets_; b++) buckets_[b] = -1;
    freeHead_ = -1;
    for (int32_t i = 0; i < nodeCount_; i++) {
        nodes_[i].next = freeHead_;
        freeHead_ = i;
    }
    entries_ = 0;
    // Every record is gone, so a "hottest name" holding one of them would be
    // a claim about nothing. The running total stays (it counts uses, not
    // records) — it is reset only when the arena is created anew.
    resetTop();
    unlock();
    ESP_LOGI(TAG, "Internal DNS cache cleared");
}

// ─────────────────────────────────────────────────────
// Persistence (cache.dat on FAT)
// ─────────────────────────────────────────────────────
//
// Binary layout (little-endian):
//   header (16 B): magic "DCC1" (4) | u32 version | u32 entryCount | u32 reserved(0)
//   per entry:
//     u8  nameLen, name[nameLen]
//     u16 qtype
//     u8  nA, u8 nAAAA
//     u32 ttlRemainingSec
//     u64 uses            (version 2 only)
//     nA  × 4 B  (IPv4, network byte order)
//     nAAAA × 16 B (IPv6)
//
// Version 1 files (written before the usage counter existed) are still read:
// their records come back with uses == 0.
namespace {
constexpr uint32_t kFileVersion = 2;      // written now
constexpr uint32_t kFileVersionMin = 1;   // still readable
constexpr uint32_t kMaxNameSave = 127;

void putU32(uint8_t* d, uint32_t v)
{
    d[0] = static_cast<uint8_t>(v & 0xFF);
    d[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    d[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    d[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
uint32_t getU32(const uint8_t* s)
{
    return static_cast<uint32_t>(s[0]) |
           (static_cast<uint32_t>(s[1]) << 8) |
           (static_cast<uint32_t>(s[2]) << 16) |
           (static_cast<uint32_t>(s[3]) << 24);
}
void putU64(uint8_t* d, uint64_t v)
{
    for (int i = 0; i < 8; i++) d[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
uint64_t getU64(const uint8_t* s)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(s[i]) << (8 * i);
    return v;
}
} // namespace

bool InternalDnsCache::saveToFile(const char* path, size_t* entriesWritten,
                                  ProgressFn progress, void* progressCtx,
                                  bool* nothingToSave)
{
    if (entriesWritten) *entriesWritten = 0;
    if (nothingToSave) *nothingToSave = false;
    if (!path || !*path) return false;
    if (!arena_) return false;

    // Pass 1 (under a short lock): snapshot the indices of the nodes we are
    // going to persist. The arena lock must NOT be held while streaming the
    // file (that would stall every DNS query needing the cache), so we copy
    // the index list first and release the lock before any file I/O.
    int32_t* snap = nullptr;
    uint32_t total = 0;
    {
        lock();
        if (!arena_) {
            unlock();
            return false;
        }
        // Count first (we only know entries_ total, but expired entries are
        // skipped below when expiry is honored — count those we keep).
        const uint64_t now = nowMs();
        uint32_t keep = 0;
        for (uint32_t b = 0; b < numBuckets_; b++) {
            for (int idx = buckets_[b]; idx >= 0; idx = nodes_[idx].next) {
                const Node& n = nodes_[idx];
                if (n.name[0] == '\0') continue;
                if (!ignoreTtl_ && n.ttl != 0 &&
                    (now - n.storedMs) / 1000ULL >= n.ttl) {
                    continue;  // already expired — skip
                }
                keep++;
            }
        }
        if (keep == 0) {
            unlock();
            if (nothingToSave) *nothingToSave = true;
            ESP_LOGW(TAG, "saveToFile: cache is empty — nothing to save");
            return false;
        }
        snap = static_cast<int32_t*>(
            heap_caps_malloc(static_cast<size_t>(keep) * sizeof(int32_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!snap) {
            unlock();
            ESP_LOGE(TAG, "saveToFile: no PSRAM for %u-node snapshot", (unsigned)keep);
            return false;
        }
        uint32_t s = 0;
        for (uint32_t b = 0; b < numBuckets_; b++) {
            for (int idx = buckets_[b]; idx >= 0; idx = nodes_[idx].next) {
                const Node& n = nodes_[idx];
                if (n.name[0] == '\0') continue;
                if (!ignoreTtl_ && n.ttl != 0 &&
                    (now - n.storedMs) / 1000ULL >= n.ttl) {
                    continue;
                }
                snap[s++] = idx;
            }
        }
        total = s;
        unlock();
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        heap_caps_free(snap);
        ESP_LOGW(TAG, "saveToFile: cannot open %s", path);
        return false;
    }

    // Header: magic "DCC1" | version u32 | entryCount u32 | reserved u32.
    // entryCount is patched at the end with the exact number actually written.
    uint8_t hdr[16];
    memcpy(hdr, "DCC1", 4);
    putU32(hdr + 4, kFileVersion);
    putU32(hdr + 8, 0);
    putU32(hdr + 12, 0);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        heap_caps_free(snap);
        return false;
    }

    const uint64_t now = nowMs();
    bool ok = true;
    uint32_t written = 0;
    for (uint32_t s = 0; s < total && ok; s++) {
        // Re-read the node under a short lock (it may have been evicted /
        // re-used meanwhile — that is fine for a snapshot file; store()
        // upserts on load).
        Node n;
        {
            lock();
            n = nodes_[snap[s]];
            unlock();
        }
        const size_t nameLen = strlen(n.name);
        if (nameLen == 0 || nameLen > kMaxNameSave) continue;

        uint32_t ttlRem = n.ttl;
        if (!ignoreTtl_ && n.ttl != 0) {
            const uint64_t ageSec = (now - n.storedMs) / 1000ULL;
            if (ageSec >= n.ttl) {
                // Expired between the snapshot and this write — keep the header
                // count consistent and store a 1 s lifetime so it self-purges
                // shortly after being loaded.
                ttlRem = 1;
            } else {
                ttlRem = static_cast<uint32_t>(n.ttl - ageSec);
            }
        }

        const uint8_t nameLenU8 = static_cast<uint8_t>(nameLen);
        if (fwrite(&nameLenU8, 1, 1, f) != 1) { ok = false; break; }
        if (fwrite(n.name, 1, nameLen, f) != nameLen) { ok = false; break; }

        uint8_t tail[8];  // qtype(2) + nA(1) + nAAAA(1) + ttl(4)
        tail[0] = static_cast<uint8_t>(n.qtype & 0xFF);
        tail[1] = static_cast<uint8_t>((n.qtype >> 8) & 0xFF);
        tail[2] = static_cast<uint8_t>(n.nA);
        tail[3] = static_cast<uint8_t>(n.nAAAA);
        putU32(tail + 4, ttlRem);
        if (fwrite(tail, 1, sizeof(tail), f) != sizeof(tail)) { ok = false; break; }

        // Usage counter (version 2): the frequency survives a save/load, so a
        // restored cache keeps its eviction order and its "hottest name".
        uint8_t usesBuf[8];
        putU64(usesBuf, n.uses);
        if (fwrite(usesBuf, 1, sizeof(usesBuf), f) != sizeof(usesBuf)) {
            ok = false;
            break;
        }

        if (n.nA > 0 && fwrite(n.a4, 4, n.nA, f) != n.nA) { ok = false; break; }
        for (uint16_t i = 0; i < n.nAAAA && ok; i++) {
            if (fwrite(n.a6[i], 1, 16, f) != 16) { ok = false; break; }
        }
        written++;

        // Report progress periodically (every 64 records keeps the overhead
        // negligible for a 60k-node snapshot).
        if (progress && (written % 64 == 0 || written == total)) {
            progress(written, total, progressCtx);
        }
    }

    // Patch the real entry count into the header (offset 8).
    if (ok) {
        uint8_t cnt[4];
        putU32(cnt, written);
        if (fseek(f, 8, SEEK_SET) == 0 &&
            fwrite(cnt, 1, 4, f) != 4) {
            ok = false;
        }
    }

    const int ferr = fclose(f);
    heap_caps_free(snap);
    if (!ok || ferr != 0) {
        ESP_LOGE(TAG, "saveToFile: write failed for %s", path);
        return false;
    }
    if (entriesWritten) *entriesWritten = written;
    if (progress) progress(written, total, progressCtx);
    ESP_LOGI(TAG, "Cache saved to %s: %u entries", path, (unsigned)written);
    return true;
}

bool InternalDnsCache::loadFromFile(const char* path, size_t* entriesLoaded,
                                    ProgressFn progress, void* progressCtx)
{
    if (entriesLoaded) *entriesLoaded = 0;
    if (!path || !*path) return false;
    if (!arena_) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return false;
    }
    const uint32_t ver = (memcmp(hdr, "DCC1", 4) == 0) ? getU32(hdr + 4) : 0;
    if (ver < kFileVersionMin || ver > kFileVersion) {
        ESP_LOGW(TAG, "loadFromFile: %s has unsupported header", path);
        fclose(f);
        return false;
    }
    const uint32_t want = getU32(hdr + 8);
    if (want > 2000000) {  // sanity bound (~20 MB / min record)
        ESP_LOGW(TAG, "loadFromFile: %s header count %u implausible", path,
                 (unsigned)want);
        fclose(f);
        return false;
    }

    size_t loaded = 0;
    for (uint32_t i = 0; i < want; i++) {
        uint8_t nameLen;
        if (fread(&nameLen, 1, 1, f) != 1) break;
        if (nameLen == 0 || nameLen > kMaxNameSave) break;
        char name[128];
        if (fread(name, 1, nameLen, f) != nameLen) break;
        name[nameLen] = '\0';
        uint8_t tail[8];
        if (fread(tail, 1, 8, f) != 8) break;
        const uint16_t qtype = static_cast<uint16_t>(tail[0] | (tail[1] << 8));
        const uint8_t nA = tail[2];
        const uint8_t nAAAA = tail[3];
        const uint32_t ttlRem = getU32(tail + 4);
        if (nA > 16 || nAAAA > 8) break;

        // Version 2 carries the usage counter; version 1 files simply have
        // none, and their records come back counted as never used.
        uint64_t uses = 0;
        if (ver >= 2) {
            uint8_t usesBuf[8];
            if (fread(usesBuf, 1, sizeof(usesBuf), f) != sizeof(usesBuf)) break;
            uses = getU64(usesBuf);
        }

        std::vector<std::string> ips;
        bool entryOk = true;
        if (qtype == 1) {
            for (uint8_t j = 0; j < nA; j++) {
                uint32_t a4;
                if (fread(&a4, 1, 4, f) != 4) { entryOk = false; break; }
                char buf[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &a4, buf, sizeof(buf))) ips.push_back(buf);
            }
        } else if (qtype == 28) {
            for (uint8_t j = 0; j < nAAAA; j++) {
                uint8_t a6[16];
                if (fread(a6, 1, 16, f) != 16) { entryOk = false; break; }
                char buf[INET6_ADDRSTRLEN];
                if (inet_ntop(AF_INET6, a6, buf, sizeof(buf))) ips.push_back(buf);
            }
        } else {
            break;
        }
        if (!entryOk || ips.empty()) break;
        // A restore writes the saved counter as it is — store() would count
        // the load itself as a use and inflate every record by one.
        storeInternal(name, qtype, ips, ttlRem, /*countUse=*/false, uses);
        loaded++;

        // Report progress periodically.
        if (progress && (loaded % 64 == 0 || loaded == want)) {
            progress(static_cast<uint32_t>(loaded), want, progressCtx);
        }
    }

    fclose(f);
    if (entriesLoaded) *entriesLoaded = loaded;
    if (progress) progress(static_cast<uint32_t>(loaded), want, progressCtx);
    ESP_LOGI(TAG, "Cache loaded from %s: %u entries", path, (unsigned)loaded);
    return true;
}

InternalDnsCache::FileInfo InternalDnsCache::fileInfo(const char* path) const
{
    FileInfo info;
    if (!path || !*path) return info;
    FILE* f = fopen(path, "rb");
    if (!f) return info;
    info.exists = true;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    info.size = (sz > 0) ? static_cast<size_t>(sz) : 0;
    fseek(f, 0, SEEK_SET);
    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
        memcmp(hdr, "DCC1", 4) == 0) {
        info.version = getU32(hdr + 4);
        info.entries = getU32(hdr + 8);
    }
    fclose(f);
    return info;
}

InternalDnsCache::Stats InternalDnsCache::stats() const
{
    Stats s;
    lock();
    s.available = (arena_ != nullptr);
    s.sizeMb = sizeMb_;
    s.capacity = (nodeCount_ > 0) ? static_cast<size_t>(nodeCount_) : 0;
    s.entries = entries_;
    s.hits = hits_;
    s.misses = misses_;
    s.evicted = evicted_;
    s.usesTotal = usesTotal_;
    s.usesMax = topUses_;
    if (topValid_) {
        s.topName.assign(topName_);
        s.topQtype = topQtype_;
    }
    const size_t nodeSize = sizeof(Node);
    s.usedBytes = entries_ * nodeSize;
    s.freeBytes = (nodeCount_ > static_cast<int32_t>(entries_))
                      ? static_cast<size_t>(nodeCount_ - static_cast<int32_t>(entries_)) * nodeSize
                      : 0;
    unlock();
    return s;
}

} // namespace dns
} // namespace dhcp
