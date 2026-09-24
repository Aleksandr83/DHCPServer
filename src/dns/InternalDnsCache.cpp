#include "InternalDnsCache.h"

#include "CacheFileReader.h"

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

// Rule 40: a using-directive is allowed where it cannot leak. This file is a
// translation unit of its own, so `using namespace std;` here shortens `string`,
// `vector` and `tolower` without touching any other file — a header must never do
// this, because its using-directive would follow the header into every includer.
using namespace std;

static const char* TAG = "InternalCache";

namespace dhcp {
namespace dns {

namespace {
constexpr size_t kMaxNameLen = 127;      // chars (fits Node::name[128])
constexpr size_t kMaxA    = 16;          // max IPv4 addresses per entry
constexpr size_t kMaxAAAA = 8;           // max IPv6 addresses per entry

// Rule 39: the rest of the numbers this module used to spell out.
constexpr uint16_t kTypeA = 1;                 // QTYPE of an IPv4 answer
constexpr uint16_t kTypeAaaa = 28;             // QTYPE of an IPv6 answer
constexpr uint32_t kMinCacheSizeMb = 1;        // smallest arena the page accepts
constexpr uint32_t kMaxCacheSizeMb = 20;       // fits cache.dat on the FAT volume
constexpr size_t kBucketGranularityBytes = 4096;  // one bucket head per this much
constexpr uint32_t kMinBuckets = 1024;         // fewer than this is pointless
// The file format itself — magic, version, record layout — lives in
// CacheFileReader (stage 169): a planned restart now reads the saved file back
// with the very reader the boot-time restore uses, so the two can never disagree
// about what a `cache.dat` is. What stays here are the node pool's own limits,
// and the four `static_assert`s below pin them to the reader's copy, so the
// writer could not produce a record the format does not describe.
constexpr uint64_t kProgressEvery = 64;

static_assert(kMaxA == CacheFileReader::kMaxA,
              "the pool and the file format must agree on the IPv4 addresses of a record");
static_assert(kMaxAAAA == CacheFileReader::kMaxAaaa,
              "the pool and the file format must agree on the IPv6 addresses of a record");
static_assert(kTypeA == CacheFileReader::kTypeA,
              "the pool and the file format must agree on the QTYPE of an IPv4 answer");
static_assert(kTypeAaaa == CacheFileReader::kTypeAaaa,
              "the pool and the file format must agree on the QTYPE of an IPv6 answer");
// Rule 39: FNV-1a, the same hash the DHCP names use — offset basis and prime.
constexpr uint32_t kFnvOffsetBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;        // report progress every N entries
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
    if (sizeMb < kMinCacheSizeMb) sizeMb = kMinCacheSizeMb;
    if (sizeMb > kMaxCacheSizeMb) sizeMb = kMaxCacheSizeMb;  // cap 20 MB (fits cache.dat on the FAT partition)

    const size_t nodeSize = sizeof(Node);
    const size_t arenaBytes = sizeMb * 1024 * 1024;

    // Bucket table ~ one 4-byte head per 4 KiB of arena (a small fraction),
    // the rest is the fixed node pool.
    uint32_t numBuckets = static_cast<uint32_t>(arenaBytes / kBucketGranularityBytes);
    if (numBuckets < kMinBuckets) numBuckets = kMinBuckets;
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
    lookupWaitUs_ = 0;
    walkedNodes_ = 0;
    stores_ = 0;
    evictScans_ = 0;
    evictScanUs_ = 0;
    evictScanNodes_ = 0;
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

uint32_t InternalDnsCache::nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

string InternalDnsCache::lower(const string& s)
{
    string out = s;
    for (auto& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

uint32_t InternalDnsCache::hashName(const char* s)
{
    uint32_t h = kFnvOffsetBasis;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
         *p; ++p) {
        h ^= *p;
        h *= kFnvPrime;
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
                               const char* name, uint16_t qtype,
                               uint32_t* walked) const
{
    uint32_t seen = 0;
    for (int idx = buckets_[bucket]; idx >= 0; idx = nodes_[idx].next) {
        seen++;
        const Node& n = nodes_[idx];
        if (n.hash == hash && n.qtype == qtype &&
            strcmp(n.name, name) == 0) {
            if (walked) *walked = seen;
            return idx;
        }
    }
    if (walked) *walked = seen;   // a miss walks its chain to the end
    return -1;
}

// Walk every bucket chain and return the used node with the lowest usage
// counter (ties: the oldest store time). Used for overflow eviction — the
// record nobody asks for goes first, not merely the one stored earliest.
int InternalDnsCache::findEvictVictim(uint32_t now, int* bucketOut,
                                     uint32_t* walked) const
{
    int best = -1;
    uint32_t bestUses = UINT32_MAX;
    uint32_t bestAge = 0;   // the largest age is the oldest record
    uint32_t seen = 0;
    for (uint32_t b = 0; b < numBuckets_; b++) {
        for (int idx = buckets_[b]; idx >= 0; idx = nodes_[idx].next) {
            seen++;
            const Node& n = nodes_[idx];
            if (best >= 0 && n.uses > bestUses) continue;
            const uint32_t age = elapsedMs(now, n.storedMs);
            if (best >= 0 && n.uses == bestUses && age <= bestAge) continue;
            bestUses = n.uses;
            bestAge = age;
            best = idx;
            if (bucketOut) *bucketOut = static_cast<int>(b);
        }
    }
    if (walked) *walked = seen;   // the scan visits the whole pool
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

void InternalDnsCache::store(const string& domain, uint16_t qtype,
                             const vector<string>& ips,
                             uint32_t ttl)
{
    // A query needed this name (either because the cache missed, or because a
    // local/upstream answer is being kept) — that is one use of the record.
    storeInternal(domain, qtype, ips, ttl, /*countUse=*/true, 0);
}

void InternalDnsCache::storeInternal(const string& domain, uint16_t qtype,
                                     const vector<string>& ips,
                                     uint32_t ttl, bool countUse,
                                     uint64_t usesExact)
{
    if (!arena_) return;
    if (domain.empty() || ips.empty()) return;
    // Only A (IPv4) and AAAA (IPv6) answers are cached.
    if (qtype != kTypeA && qtype != kTypeAaaa) return;

    const string lname = lower(domain);
    if (lname.size() >= kMaxNameLen) return;

    lock();

    uint32_t h = hashName(lname.c_str());
    uint32_t b = bucketOf(h);
    const uint32_t now = nowMs();

    // Upsert existing entry (domain + type). The walk count is only reported
    // from the lookup path, which is the one the meter times.
    int idx = findNode(b, h, lname.c_str(), qtype);
    if (idx < 0) {
        // New entry — need a free node (evict the least used if the pool is
        // full).
        idx = allocNode();
        if (idx < 0) {
            int oldBucket = 0;
            uint32_t scanned = 0;
            // Stage 127: this scan walks the whole arena while holding the
            // mutex, so every concurrent lookup waits inside its own measured
            // hit time. Count and time it — that is the one thing in this cache
            // that can turn tens of microseconds into milliseconds.
            const int64_t scanStart = esp_timer_get_time();
            int victim = findEvictVictim(now, &oldBucket, &scanned);
            evictScanUs_ += static_cast<uint64_t>(esp_timer_get_time() - scanStart);
            evictScans_++;
            evictScanNodes_ += scanned;
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
    stores_++;   // one record written (new, refreshed or restored)
    if (countUse) {
        bumpUse(n, idx);
    } else {
        // A restore is not a use: the counter is written as it was saved. The
        // in-memory field is 32-bit, so a larger saved value is clamped.
        n.uses = (usesExact > UINT32_MAX) ? UINT32_MAX
                                          : static_cast<uint32_t>(usesExact);
        noteMark(n, idx);
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
void InternalDnsCache::bumpUse(Node& n, int idx)
{
    if (n.uses != UINT32_MAX) n.uses++;
    if (usesTotal_ != UINT32_MAX) usesTotal_++;
    noteMark(n, idx);
}

// Which record leads the field — an index and a qtype, nothing else. Deliberately
// no name copy: this runs on every hit, and the name is only needed when the
// status is read (see noteTop()).
//
// A record whose counter reached the highest value so far becomes the new mark;
// "the same value twice" also refreshes it, so what the mark points at is a
// record that is actually in the cache at the moment it was passed.
void InternalDnsCache::noteMark(const Node& n, int idx)
{
    if (n.uses == 0) return;   // a record nobody used is not a candidate
    if (topValid_ && n.uses < topUses_) return;
    topUses_ = n.uses;
    topQtype_ = n.qtype;
    topIdx_ = idx;
    topValid_ = true;
}

// The mark's name, produced on demand. This is the only place that copies the
// 128 bytes; it is called under the arena lock from stats(), never from
// lookup(). The slot may have been recycled since the mark was taken — the
// counter check then fails and the previous name stays, which is what the mark
// always promised (a high-water mark, not a live reading).
void InternalDnsCache::noteTop() const
{
    if (!topValid_) {
        topName_[0] = '\0';
        return;
    }
    if (topIdx_ < 0 || topIdx_ >= nodeCount_) return;
    const Node& n = nodes_[topIdx_];
    if (n.uses != topUses_) return;
    memcpy(topName_, n.name, sizeof(topName_));
    topName_[sizeof(topName_) - 1] = '\0';
}

void InternalDnsCache::resetTop()
{
    topUses_ = 0;
    topQtype_ = 0;
    topIdx_ = -1;
    topName_[0] = '\0';
    topValid_ = false;
}

bool InternalDnsCache::lookup(const string& domain, uint16_t qtype,
                              vector<string>& ips, uint32_t& ttl)
{
    if (!arena_) return false;
    if (domain.empty()) return false;

    const string lname = lower(domain);
    if (lname.size() >= kMaxNameLen) return false;

    // Stage 127: the wait for the arena mutex is inside the interval the caller
    // measures, and it is somebody else's work (a store evicting from a full
    // pool, a save taking its snapshot), not ours. Time it separately.
    const int64_t lockStart = esp_timer_get_time();
    lock();
    const uint64_t waitUs = static_cast<uint64_t>(esp_timer_get_time() - lockStart);

    uint32_t h = hashName(lname.c_str());
    uint32_t b = bucketOf(h);
    uint32_t walked = 0;
    int idx = findNode(b, h, lname.c_str(), qtype, &walked);

    if (idx < 0) {
        misses_++;
        unlock();
        return false;
    }

    Node& n = nodes_[idx];
    // One age for both decisions below (two clock reads could straddle a
    // millisecond and disagree). elapsedMs() is modular, so this stays correct
    // across the 49.7-day turnover of the 32-bit millisecond clock.
    const uint32_t elapsed = elapsedMs(nowMs(), n.storedMs);

    // Honoring TTL: an entry older than its TTL is a miss and is purged.
    if (!ignoreTtl_ && n.ttl != 0 && elapsed / 1000u >= n.ttl) {
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
        // Both operands are 32-bit seconds/ms and the expiry check above
        // guarantees elapsed < ttl*1000, so this neither underflows nor
        // overflows for any TTL below 49.7 days.
        const uint32_t remainMs = (static_cast<uint32_t>(n.ttl) * 1000u) - elapsed;
        ttl = (remainMs + 999u) / 1000u;  // ceil to seconds
    }

    // The record served the client — that is one use of this name (an expired
    // or empty answer above returned early and is not counted). The hot path
    // pays two saturating increments and the mark's index, not a name copy.
    // Stage 127: the wait and the walk are reported per hit, so the average
    // DnsServer publishes can be split into ours and somebody else's.
    lookupWaitUs_ += waitUs;
    walkedNodes_ += walked;
    bumpUse(n, idx);
    hits_++;
    unlock();
    return true;
}

void InternalDnsCache::restoreMeasurement(uint64_t waitUs, uint64_t walkedNodes,
                                         uint64_t stores, uint64_t evictScans,
                                         uint64_t evictScanUs,
                                         uint64_t evictScanNodes)
{
    lock();
    lookupWaitUs_ = waitUs;
    walkedNodes_ = walkedNodes;
    stores_ = stores;
    evictScans_ = evictScans;
    evictScanUs_ = evictScanUs;
    evictScanNodes_ = evictScanNodes;
    unlock();
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
// The layout of the file — header, record fields, the three versions that are
// read and what the version 3 change costs — is described next to the reader
// that owns it now (`CacheFileReader`, stage 169). What is left here is the
// writing half: the header, the records and the entry count patched in at the
// end, all spelled in the reader's constants so the two halves of the format
// cannot drift apart.
namespace {

// The file format belongs to CacheFileReader (stage 169); this short alias keeps
// the writing half below readable without copying a single number into this file.
using CacheFmt = CacheFileReader;

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
// The 64-bit reader a version 2 record needs now lives next to the format, in
// CacheFileReader — this file only writes version 3 (a 4-byte counter) and reads
// the two header fields it reports in fileInfo().
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
        const uint32_t now = nowMs();
        uint32_t keep = 0;
        for (uint32_t b = 0; b < numBuckets_; b++) {
            for (int idx = buckets_[b]; idx >= 0; idx = nodes_[idx].next) {
                const Node& n = nodes_[idx];
                if (n.name[0] == '\0') continue;
                if (!ignoreTtl_ && n.ttl != 0 &&
                    elapsedMs(now, n.storedMs) / 1000u >= n.ttl) {
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
                    elapsedMs(now, n.storedMs) / 1000u >= n.ttl) {
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
    uint8_t hdr[CacheFmt::kHeaderBytes];
    memcpy(hdr, CacheFmt::kMagic, CacheFmt::kMagicBytes);
    putU32(hdr + CacheFmt::kVersionOffset, CacheFmt::kVersion);
    putU32(hdr + CacheFmt::kCountOffset, 0);
    putU32(hdr + CacheFmt::kReservedOffset, 0);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        heap_caps_free(snap);
        return false;
    }

    const uint32_t now = nowMs();
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
        if (nameLen == 0 || nameLen > CacheFmt::kMaxNameBytes) continue;

        uint32_t ttlRem = n.ttl;
        if (!ignoreTtl_ && n.ttl != 0) {
            const uint32_t ageSec = elapsedMs(now, n.storedMs) / 1000u;
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

        uint8_t tail[CacheFmt::kTailBytes];  // qtype(2) + nA(1) + nAAAA(1) + ttl(4)
        tail[0] = static_cast<uint8_t>(n.qtype & 0xFF);
        tail[1] = static_cast<uint8_t>((n.qtype >> 8) & 0xFF);
        tail[2] = static_cast<uint8_t>(n.nA);
        tail[3] = static_cast<uint8_t>(n.nAAAA);
        putU32(tail + 4, ttlRem);
        if (fwrite(tail, 1, sizeof(tail), f) != sizeof(tail)) { ok = false; break; }

        // Usage counter (version 2 onward): the frequency survives a save/load,
        // so a restored cache keeps its eviction order and its "hottest name".
        // Version 3 writes four bytes — the width the field has in memory.
        uint8_t usesBuf[4];
        putU32(usesBuf, n.uses);
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
        if (progress && (written % kProgressEvery == 0 || written == total)) {
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

    // The file is read by the same reader a planned restart checks it with
    // (stage 169): a check that used a different parser could pass on a file
    // this restore then refuses, which would be the worst of both worlds.
    size_t loaded = 0;
    const CacheFileRead read = CacheFileReader::read(
        path, [&](const CacheFileRecord& rec, uint32_t index, uint32_t count) {
            // The reader hands the addresses over as the bytes the file holds
            // (it knows nothing of lwIP), so the text is made here — the same
            // text `store()` writes into a node.
            vector<string> ips;
            ips.reserve(rec.addrs.size());
            char buf[INET6_ADDRSTRLEN];
            for (const string& raw : rec.addrs) {
                const bool ipv6 = (rec.qtype == CacheFileReader::kTypeAaaa);
                const void* addr = raw.data();
                if (inet_ntop(ipv6 ? AF_INET6 : AF_INET, addr, buf, sizeof(buf))) {
                    ips.push_back(buf);
                }
            }
            if (ips.empty()) return;   // nothing to insert; the reader already validated the shape

            // A restore writes the saved counter as it is — store() would count
            // the load itself as a use and inflate every record by one.
            storeInternal(rec.name, rec.qtype, ips, rec.ttlRemaining,
                          /*countUse=*/false, rec.uses);
            loaded++;

            // Report progress periodically.
            if (progress && ((index + 1) % kProgressEvery == 0 || (index + 1) == count)) {
                progress(static_cast<uint32_t>(index + 1), count, progressCtx);
            }
        });

    switch (read.status) {
        case CacheFileStatus::NotFound:
        case CacheFileStatus::BadHeader:
        case CacheFileStatus::UnsupportedVersion:
        case CacheFileStatus::ImplausibleCount:
            ESP_LOGW(TAG, "loadFromFile: %s refused (%s)", path, read.why.c_str());
            return false;
        default:
            // Ok, and also a file that ended early: a partial restore is what
            // this path has always done with one (the boot log says how many
            // records came back), and the restart check is where a torn file is
            // named as such.
            break;
    }

    if (entriesLoaded) *entriesLoaded = loaded;
    if (progress) progress(static_cast<uint32_t>(loaded), read.headerCount, progressCtx);
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
    uint8_t hdr[CacheFmt::kHeaderBytes];
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
        memcmp(hdr, CacheFmt::kMagic, CacheFmt::kMagicBytes) == 0) {
        info.version = getU32(hdr + CacheFmt::kVersionOffset);
        info.entries = getU32(hdr + CacheFmt::kCountOffset);
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
    s.waitUs = lookupWaitUs_;
    s.walkedNodes = walkedNodes_;
    s.stores = stores_;
    s.evictScans = evictScans_;
    s.evictScanUs = evictScanUs_;
    s.evictScanNodes = evictScanNodes_;
    s.usesTotal = usesTotal_;
    s.usesMax = topUses_;
    noteTop();   // materialise the mark's name — the hot path never copies it
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
