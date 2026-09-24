#ifndef DHCP_DNS_DNSSERVER_H
#define DHCP_DNS_DNSSERVER_H

#include "IDnsServer.h"
#include "DnsCache.h"
#include "DnsLogger.h"
#include "InternalDnsCache.h"
#include "CacheAutosave.h"
#include "RestartSaveJobState.h"
#include "RestartSaveVerify.h"
#include "../dhcp/IDhcpServer.h"

#include <string>
#include <map>
#include <vector>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace dhcp {
namespace dns {

/**
 * @brief DNS proxy server implementation.
 *
 * Processing pipeline for each query:
 *   1. Log the request (terminal + optional REST)
 *   2. Search local hosts file (in-memory map)
 *   3. Search external cache (DnsCache REST client, if enabled)
 *   4. Forward to external DNS server → return result (and store in cache)
 *
 * Listens on UDP port 53 in a dedicated FreeRTOS task.
 */
class DnsServer : public IDnsServer {
public:
    DnsServer();
    ~DnsServer() override;

    // IDnsServer interface
    bool start() override;
    void stop() override;
    DnsServerState state() const override { return state_; }
    bool isRunning() const override { return state_ == DnsServerState::RUNNING; }
    uint32_t queryCount() const override { return queryCount_; }
    std::string stateString() const override;

    /**
     * @brief Add a local host entry (domain → IP).
     * Used for the local hosts file lookup step.
     */
    void addLocalHost(const std::string& domain, const std::string& ip);

    /**
     * @brief Clear all local host entries.
     */
    void clearLocalHosts();

    /**
     * @brief Enable/disable terminal logging at runtime.
     */
    void setLogTerminal(bool enabled);

    /**
     * @brief Enable/disable blocking of non-A/AAAA query forwarding.
     *
     * When enabled, queries whose type is neither A (1) nor AAAA (28) and
     * which are not answered from local hosts are answered with NODATA
     * instead of being sent to the external cache / upstream DNS.
     */
    void setBlockForwardNonAA(bool enabled);

    /**
     * @brief (Re)apply the "own subnet only" client filter.
     *
     * Re-reads the on/off flag from the DNS config and the network definition
     * (device address + netmask) from the DHCP settings, since that page owns
     * the subnet. Called from start() and whenever the DNS or DHCP settings
     * change. When the flag is on but the DHCP subnet cannot be parsed the
     * filter is skipped with a warning (fail-open) — a broken subnet must not
     * lock the whole LAN out of DNS.
     */
    void applySubnetFilter();

    /** @brief DNS queries dropped by the own-subnet filter (for diagnostics). */
    uint32_t foreignDroppedCount() const { return foreignDropped_; }

    /**
     * @brief Re-point the REST logger at the local hosts map.
     *
     * Called at startup and after Local Hosts change at runtime, so the
     * REST URL host resolution always uses the current list (no reboot
     * needed after saving Local Hosts).
     */
    void syncLoggerLocalHosts();

    /**
     * @brief Get references to logger and cache for configuration.
     */
    DnsLogger& logger() { return logger_; }
    DnsCache& cache() { return cache_; }
    InternalDnsCache& internalCache() { return internalCache_; }

    /**
     * @brief (Re)apply the built-in (PSRAM) cache config at runtime.
     *
     * Enables/disables the internal cache, resizes it when the limit changed
     * and updates the ignore-TTL flag. Safe to call from the REST handler.
     */
    void applyInternalCache(bool enabled, uint32_t sizeMb, bool ignoreTtl);

    /**
     * @brief Apply the automatic-save settings of the built-in cache.
     *
     * Stage 153: turning it on starts (or re-arms) the timer, turning it off
     * stops it. Called with the values the operator saved, and again whenever
     * they change.
     */
    void applyCacheAutosave(bool enabled, core::AutosavePeriod unit,
                             uint16_t interval);

    /**
     * @brief Built-in cache statistics + per-query counters (main page).
     */
    InternalDnsCache::Stats internalCacheStats() { return internalCache_.stats(); }
    // Queries answered directly from the built-in cache.
    uint32_t internalCacheHits() const { return internalCacheHits_; }
    // Queries that were forwarded to the external DNS server.
    uint32_t forwardedCount() const { return forwardedCount_; }
    // Average time of a successful built-in-cache lookup in µs (0 if no hits).
    uint32_t internalCacheAvgHitUs() const
    {
        return internalCacheHits_ > 0
                   ? static_cast<uint32_t>(internalCacheHitUs_ / internalCacheHits_)
                   : 0;
    }

    // ─── Keeping state across a planned restart ───
    /**
     * @brief How one "keep it across the restart" step ended.
     *
     * The reboot paths call the steps unconditionally and ignore the result;
     * `POST /api/device/reboot/prepare` reports it, because the Device
     * Management page shows the operator what is being written.
     */
    enum class RestartSave {
        Ok,            // statistics: the save job is running; cache: ditto
        Skipped,       // the operator has the switch off (or the cache is absent)
        NothingToSave, // the cache holds no entry — there is no file to update
        Busy,          // a job of this kind was already in flight and will be waited for
        Failed,
    };

    // ─── Statistics persistence (Statistica.dat on FAT) ───
    /**
     * @brief Keep the main-page counters across a restart.
     *
     * The three counters above live here and are never reset by anything but a
     * reboot, so a restore simply seeds them and every later query keeps adding.
     * The file is written before a restart (the reboot button, an OTA update, the
     * terminal menu) and read back once at boot — deliberately **after** the
     * built-in cache has been enabled, because `InternalDnsCache::enable()` zeroes
     * the cache's own counters and would wipe an earlier restore.
     *
     * All three are no-ops while the operator has the flag off; the reboot paths
     * call them unconditionally so they never have to know the policy.
     */
    bool restoreStatsFromFile();

    /** @brief Drop the file — a factory reset must not keep statistics. */
    bool deleteStatsFile();

    /**
     * @brief State of the statistics job, for GET /api/dns/stats/progress.
     *
     * Both fields come from one locked read on purpose: two calls would let a
     * page see `busy=false` from the old run and `result=None` from the new one,
     * which is exactly the "no verdict" state it treats as unknown.
     */
    struct StatsProgress {
        bool busy = false;   // the background write is running
        RestartSaveJobState::Verdict result = RestartSaveJobState::Verdict::None;
        /// True while the job is reading the file back rather than writing it
        /// (stage 169). The check of 92 bytes is over in microseconds, so a page
        /// that polls twice a second will rarely see it — but it is the truth
        /// about what the device is doing, and the line for it exists for the
        /// case where it is caught.
        bool checking = false;
        /// True when the last finished job **read the file back and it matched**
        /// (stage 169). It is how a page can say "the file was checked" without
        /// guessing: a job that only wrote the file (or a firmware that does not
        /// check at all) leaves this false, and `ok` alone would not tell the two
        /// apart.
        bool checked = false;
        /// Where the file lives (`/fat/Statistica.dat`). The page shows it, so
        /// the operator can see which file is being written and checked.
        std::string path;
        /// Why the last job failed, in the device's words ("cannot publish the
        /// file"). It travels to the page because the terminal is not always
        /// there — the operator has no serial console, which is exactly how a
        /// failed write went unexplained once.
        std::string detail;
    };

    /**
     * @brief Start the background write of Statistica.dat (single-flight).
     *
     * The file is nine numbers — 92 bytes — and it is still written on a task
     * of its own, for the same reason the cache is: the reboot paths run on the
     * single httpd task, and a file write there (a slow card, a filesystem
     * hiccup) answers nobody else for as long as it takes. The operator asked
     * for the two files to be treated alike, and they are: two jobs, because
     * their switches are independent and the page shows them as two steps.
     *
     * @return Ok when the job was started, Skipped when the switch is off, Busy
     * when one is already running (the caller waits for that one instead),
     * Failed when the task could not be created.
     */
    RestartSave startStatsSaveJob();

    /**
     * @brief Write Statistica.dat before a planned restart and wait for it.
     *
     * For the paths that have no page to poll: an OTA update and the console's
     * `reboot`. The wait is bounded by kPersistStallMs; a stuck job is logged
     * and the restart continues, because the write is published with
     * `.tmp` + `rename` and an unfinished one leaves the previous file intact.
     */
    bool saveStatsBeforeRestart();

    /**
     * @brief Wait for the statistics job to finish. Returns false when it was
     * still running after @p stallMs milliseconds.
     */
    bool waitForStatsJob(uint32_t stallMs);

    /**
     * @brief Current statistics job state (busy + verdict of the last one).
     */
    StatsProgress statsProgress() const;

    // ─── Keeping the cache itself across a planned restart ───
    /**
     * @brief Start the cache save a planned restart wants, without waiting.
     *
     * The caller is the one that knows how long it may wait, and that is the
     * point of the split: `POST /api/device/reboot/prepare` returns at once so
     * the page can poll the job's progress and only then ask for the restart,
     * while the other paths (OTA, the console, a plain reboot call) use
     * saveCacheBeforeRestart() below and wait inside the request.
     */
    RestartSave startCacheSaveForRestart();

    /**
     * @brief Write cache.dat before a planned restart, when the operator asked
     * for it (`cacheInternalSaveCache`).
     *
     * The cache is far bigger than the statistics file, so this runs the normal
     * background save job (`startPersistJob(true)`) and waits for it here: the
     * file I/O must not happen on the caller's stack — the console's `reboot`
     * runs on the ~3.5 KB main task, while the persist task owns a dedicated
     * 8 KB one and is the path the web page already exercises.
     *
     * The wait is bounded and progress-aware: a job that stops moving for
     * kPersistStallMs must not be able to keep the device from restarting.
     * Returns true when the cache is on its way to the file (or when there was
     * nothing to do because the flag is off), false when it could not be saved.
     */
    bool saveCacheBeforeRestart();

    // ─── Built-in cache persistence (cache.dat on FAT) ───
    /**
     * @brief Path of the built-in cache persistence file on the FAT partition.
     * The FAT filesystem is optional — all persistence operations are safe
     * no-ops (false) when /fat is not mounted.
     */
    static constexpr const char* kCacheDatPath = "/fat/cache.dat";

    /**
     * @brief How long a save/load job may make no progress before a reboot path
     * stops waiting for it (see saveCacheBeforeRestart()).
     */
    static constexpr uint32_t kPersistStallMs = 5000;

    /**
     * @brief How the last save/load ended, for clients that have to say it out
     * loud (the Device Management page reports the step of a planned restart).
     *
     * `Empty` is not an error: a device that has not answered a query yet, or
     * whose entries have all expired, has simply nothing to write — and telling
     * the operator "the cache could not be saved" in that case is a lie a
     * boolean could not avoid (it was the sole reason for this enum).
     *
     * `Mismatch` (stage 169) is the third kind of bad news and the reason it is
     * not folded into `Failed`: the file *was* written, but reading it back did
     * not give what was written. The page asks the operator a different question
     * about it ("the cache on the card does not match — reboot anyway?"), and the
     * device retried the save once before saying so.
     */
    enum class PersistResult { None, Ok, Empty, Mismatch, Failed };

    /**
     * @brief Progress of the running save/load job.
     */
    struct PersistProgress {
        bool     busy = false;   // a background job is running
        bool     isSave = false; // true=save, false=load
        /// True while a restart-requested save is reading the file back instead
        /// of writing it (stage 169). On a table of a few megabytes that pass
        /// takes seconds, so the page says "checking" with its own progress
        /// rather than freezing at the save's last percentage.
        bool     checking = false;
        /// True when the last finished job **read the file back and it matched**
        /// (stage 169) — the same meaning as the statistics job's flag: a manual
        /// save from the DNS page does not verify, so its `ok` is not a check.
        bool     checked = false;
        PersistResult result = PersistResult::None;  // outcome of the last finished job
        uint32_t done = 0;       // records processed so far
        uint32_t total = 0;      // total records (0 until known)
        /// Where the file lives (`/fat/cache.dat`), for the page's status line.
        std::string path;
        /// Why the last job ended that way, in the device's own words (empty
        /// unless it failed or its content did not match). It travels to the page
        /// for the same reason the statistics job's detail does: the operator
        /// does not always have a terminal, and "the cache could not be saved"
        /// without the reason is exactly what made him ask what went wrong.
        std::string detail;
    };

    /**
     * @brief Start a background save/load of the built-in cache (single-flight).
     *
     * Spawns a low-priority task ("ic_persist") so the long file I/O never
     * blocks the httpd task (web stays responsive) nor holds the arena mutex
     * for the whole operation. Progress is polled via persistProgress().
     * @param save true → cache → /fat/cache.dat; false → file → cache.
     * @param force true → load even when the file's checksum does not match the
     * one this device stored (the operator confirmed it on the page). A save is
     * never forced: there is nothing to override there.
     * @return true when the job was started; false when another job is running,
     * the cache is disabled, or the operation is not possible (e.g. no file
     * to load).
     */
    bool startPersistJob(bool save, bool force = false);

    /**
     * @brief State of the checksum the device keeps for its own cache file.
     *
     * The file is only loaded when it is the one this device wrote. Three ways
     * the check can fail, and they are different things: the file is not there
     * at all, the device has no checksum for it (fresh flash, factory reset, or
     * a file put there by the explorer), or the checksum simply differs (a
     * half-written file, a damaged block).
     */
    struct CacheFileMd5 {
        bool fileExists = false;   // /fat/cache.dat is present
        bool hasStored = false;    // the device remembers a checksum for it
        bool match = false;        // and that is the checksum the file has now
        bool readable = false;     // the file could be read at all
        std::string fileMd5;       // digest of the file now (empty when unreadable)
        std::string storedMd5;     // digest of the file as it was written
    };

    /**
     * @brief Compare the stored checksum with the one the file has now.
     *
     * Reads the whole file (one pass, 4 KB at a time), so it costs what one read
     * of the cache costs — callers are the reboot path and the load handler, not
     * a polling path.
     */
    CacheFileMd5 checkCacheFileMd5();

    /**
     * @brief Remember @p md5 as the checksum of the cache file just written.
     *
     * Written through Config (NVS) immediately: the value has to survive the very
     * restart it is meant to protect against.
     */
    bool storeCacheFileMd5(const std::string& md5);

    /**
     * @brief Current save/load progress (for GET .../progress).
     */
    PersistProgress persistProgress() const;

    /**
     * @brief Info (exists/size/entries) about kCacheDatPath.
     */
    InternalDnsCache::FileInfo internalCacheFileInfo() const;

    /**
     * @brief Provide the DHCP server for the client IP → MAC lookup fallback
     * (used when the ARP cache has no entry for a DNS client).
     */
    void setDhcpServer(::dhcp::dhcp::IDhcpServer* dhcp);

private:
    /**
     * @brief Resolve a client IPv4 (network byte order) to a MAC string.
     *
     * Combined lookup: ARP cache first (lwIP), then the DHCP lease table.
     * Returns "xx:xx:xx:xx:xx:xx" or an empty string if unknown.
     */
    std::string resolveClientMac(uint32_t clientIpNet) const;
    // Task
    static void serverTask(void* arg);
    void serverLoop();
    // Background write of Statistica.dat (startStatsSaveJob). Single-flight,
    // like the cache job below; the two never share a task or a mutex.
    static void statsJobTask(void* arg);
    // The actual write, called by the job task. @p detail receives the device's
    // own reason when the write failed (empty otherwise).
    RestartSaveJobState::Verdict writeStatsNow(std::string& detail);
    // Background manual save/load job (startPersistJob). Single-flight. Also
    // used for the boot-time auto-restore from /fat/cache.dat.
    static void persistJobTask(void* arg);
    // Progress callback fed to InternalDnsCache::saveToFile/loadFromFile.
    static void onPersistProgress(uint32_t done, uint32_t total, void* ctx);
    // Wait until no save/load job is running any more, as long as it keeps
    // making progress. Returns false when it stalled for stallMs milliseconds.
    bool waitForPersistJob(uint32_t stallMs);

    // DNS message parsing
    bool parseQuery(const uint8_t* buf, size_t len,
                    std::string& domain, uint16_t& type,
                    uint16_t& cls, uint16_t& id);

    // Extract A/AAAA answer IPs (and the minimum TTL) from an upstream DNS
    // reply, so the resolved mapping can be stored in the built-in cache and
    // the external cache.
    void parseForwardAnswer(const uint8_t* buf, size_t len,
                            std::vector<std::string>& ips,
                            uint32_t& ttlSec);

    // DNS message building
    size_t buildAnswer(uint8_t* buf, size_t bufSize,
                       uint16_t id, const std::string& domain,
                       uint16_t type, uint16_t cls,
                       const std::vector<std::string>& ips,
                       uint32_t ttl);

    size_t buildNxdomain(uint8_t* buf, size_t bufSize,
                         uint16_t id, const std::string& domain,
                         uint16_t type, uint16_t cls);

    // Domain name encoding helpers
    static size_t encodeDomainName(uint8_t* dst, const std::string& domain);
    static std::string decodeDomainName(const uint8_t* data, size_t len,
                                        size_t& offset);

    // Async forwarding state — one slot per in-flight client query.
    // Strict cache-first: the external cache is consulted first, and the
    // query is sent to the external DNS ONLY on a cache miss (or cache-wait
    // timeout). A slot has two phases:
    //   phase 1 (waitingCache): awaiting the async external-cache result; the
    //     query is NOT yet sent upstream (external DNS is only queried when
    //     the cache misses, per the cache semantics).
    //   phase 2 (forwarding): the cache missed/timed out and the query was
    //     sent to the external DNS; the client is answered from the first
    //     arriving upstream reply.
    // The main loop uses select() on the listening socket, the sockets of all
    // phase-2 forwards and the cache wakeup socket, so no slow upstream or
    // cache server ever blocks handling of other queries.
    struct PendingForward {
        bool active = false;
        bool waitingCache = true;    // phase 1: awaiting the cache result
        int fwdFd = -1;              // UDP socket used for this forward
        struct sockaddr_in client;   // original client to reply to
        socklen_t clientLen = 0;
        uint16_t qid = 0;
        uint16_t qtype = 0;
        uint16_t qclass = 0;
        std::string domain;          // for NXDOMAIN fallback on timeout
        uint8_t query[1024] = {0};   // raw query (for the delayed forward)
        uint16_t queryLen = 0;
        uint64_t createdMs = 0;      // when the slot was created
        uint64_t sentMs = 0;         // when the query was forwarded (phase 2)
    };
    static constexpr int kMaxPendingForwards = 32;
    static constexpr int kCacheWaitTimeoutMs = 2000;  // cache result cap
    PendingForward pendingForwards_[kMaxPendingForwards];

    // Find a free pending slot, or -1 if all are busy.
    int allocPendingSlot();
    // Close the socket and deactivate a pending forward slot.
    void freePendingSlot(int idx);
    // Reply NXDOMAIN to the pending slot's client and free the slot.
    void expirePendingSlot(int idx);
    // Start the phase-2 upstream forward for a pending slot. Returns false
    // (slot left in cache phase) if the socket/send failed.
    bool startForward(int idx, uint64_t now);
    uint64_t nowMs() const;

    DnsServerState state_ = DnsServerState::STOPPED;
    TaskHandle_t taskHandle_ = nullptr;
    int socketFd_ = -1;
    bool stopRequested_ = false;
    uint32_t queryCount_ = 0;
    uint32_t internalCacheHits_ = 0;  // answered from the built-in cache
    uint64_t internalCacheHitUs_ = 0; // total lookup time of those hits (µs)
    uint32_t forwardedCount_ = 0;     // sent to the external DNS server

    // Background manual persist job state (save/load of cache.dat). Guarded
    // by persistJobMutex_. Read by the REST GET .../progress handler, written
    // by the persist job task (progress callback).
    void*  persistJobMutex_ = nullptr;   // SemaphoreHandle_t
    bool   persistBusy_ = false;
    bool   persistSave_ = false;         // true=save, false=load
    bool   persistForce_ = false;        // load even on a checksum mismatch
    // Verdict of the last finished job. The page that polls the progress needs
    // it: "busy went false" alone cannot tell "the cache is on the card" from
    // "there was nothing to write" or "the write failed", and reporting a save
    // that did not happen is worse than reporting nothing.
    PersistResult persistResult_ = PersistResult::None;
    uint32_t persistDone_ = 0;
    uint32_t persistTotal_ = 0;
    TaskHandle_t persistTaskHandle_ = nullptr;

    // Background statistics write state (startStatsSaveJob), guarded by
    // statsJobMutex_. The rules it enforces — single-flight, a verdict the page
    // reads, no inherited luck — are in RestartSaveJobState, where they are
    // host-tested; here it is only the task and the mutex around it.
    void*  statsJobMutex_ = nullptr;     // SemaphoreHandle_t
    RestartSaveJobState statsJob_;
    TaskHandle_t statsTaskHandle_ = nullptr;

    // ─── The read-back check a planned restart asks for (stage 169) ───
    /// @brief Take (and clear) the restart's request for a verified cache save.
    ///
    /// The request is a flag rather than a parameter of startPersistJob(), for one
    /// reason: the restart path may find a save already running (a manual one, or
    /// the boot-time restore), and it is still *that* file the restart will keep —
    /// so the job in flight has to be able to pick the request up. Jobs that were
    /// not asked do not verify: the operator's rule names the two restart paths.
    bool takePersistVerifyOwed();
    /// @brief Report one verified-save attempt to the error log.
    void reportCacheVerify(int attempt, RestartSaveVerify::AttemptResult result,
                           const std::string& why);
    /// @brief Mark the read-back phase of the cache job (progress endpoint).
    void setPersistChecking(bool checking);
    /// @brief Mark the read-back phase of the statistics job (progress endpoint).
    void setStatsChecking(bool checking);

    bool   persistVerifyOwed_ = false;   // guarded by persistJobMutex_
    bool   persistChecking_ = false;     // ditto — the read-back pass is running
    bool   persistChecked_ = false;      // ditto — the last save was read back and matched
    std::string persistDetail_;          // ditto — why the last job ended that way
    bool   statsChecking_ = false;       // guarded by statsJobMutex_
    bool   statsChecked_ = false;        // ditto — the last write was read back and matched

    // External DNS server address
    uint32_t externalDnsIp_ = 0;

    // Terminal logging flag (gates the per-query ESP_LOGI lines)
    bool logTerminal_ = false;

    // When true, non-A/AAAA queries that miss local hosts are answered with
    // NODATA instead of being forwarded to the external cache / upstream DNS.
    bool blockForwardNonAA_ = false;

    // Own-subnet client filter (allowOwnSubnet config). subnetNet_/subnetMask_
    // are host byte order (0 mask = no usable subnet → filter skipped).
    bool allowOwnSubnet_ = false;
    uint32_t subnetNet_ = 0;
    uint32_t subnetMask_ = 0;
    uint32_t foreignDropped_ = 0;      // queries dropped by the filter
    uint64_t lastForeignLogMs_ = 0;    // throttles the drop log line

    // Local hosts (domain → IP)
    std::map<std::string, std::vector<std::string>> localHosts_;

    // DHCP server (for client IP → MAC fallback lookup)
    ::dhcp::dhcp::IDhcpServer* dhcpServer_ = nullptr;

    // Sub-components
    DnsLogger logger_;
    DnsCache cache_;
    InternalDnsCache internalCache_;
    /// Stage 153: writes the cache to the card on a timer (see CacheAutosave).
    CacheAutosave cacheAutosave_{internalCache_, kCacheDatPath};
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSSERVER_H
