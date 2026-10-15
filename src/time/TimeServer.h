#ifndef DHCP_TIME_TIMESERVER_H
#define DHCP_TIME_TIMESERVER_H

#include "ITimeServer.h"
#include "TimeLogger.h"
#include "NtpMessage.h"
#include "NtpProbeFormat.h"
#include "core/NtpServers.h"
#include "core/SubnetFilter.h"

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Forward declaration of the SNTP notification callback parameter type
// (defined in <sys/time.h>); avoids pulling the header into every includer.
struct timeval;

// The DHCP server, needed by the "allowed computers" filter (stage 196) only as
// a pointer to an interface, so the header stays free of the DHCP headers.
namespace dhcp {
namespace dhcp {
class IDhcpServer;
} // namespace dhcp
} // namespace dhcp

namespace dhcp {
namespace time {

/**
 * @brief NTP server implementation backed by the ESP-IDF SNTP client.
 *
 * Two logical blocks in one service:
 *   1. SNTP client — periodically synchronises the device clock from an
 *      external NTP server (esp_sntp, poll mode).
 *   2. NTP server — a dedicated task answering LAN clients on UDP port 123
 *      with a standard 48-byte mode-4 packet (stratum 3, UTC time).
 *
 * Between synchronisations the time is read from the system clock, which
 * keeps counting from the internal timer even without internet access.
 */
class TimeServer : public ITimeServer {
public:
    TimeServer();
    ~TimeServer() override;

    // ─── ITimeServer ────────────────────────────────
    bool start() override;
    void stop() override;
    TimeServerState state() const override { return state_; }
    bool isRunning() const override { return state_ == TimeServerState::RUNNING; }
    bool isSynced() const override { return synced_; }
    std::string stateString() const override;

    // ─── SNTP clock sync (independent of the NTP server) ───
    /**
     * @brief Start the SNTP client that keeps the device clock in sync.
     *
     * Independent from start()/stop() so the clock can be synchronised even
     * when the NTP server (serving LAN clients) is disabled.
     */
    void startSync();
    /** @brief Stop the SNTP client. */
    void stopSync();
    /** @brief True while the SNTP client is running. */
    bool isSyncRunning() const { return sntpStarted_; }

    /**
     * @brief How the SNTP client stands with its next poll (stage 190).
     *
     * The countdown is measured from the moment the client last had a reason to
     * start counting — a successful synchronisation, or the (re)start of the
     * client, which asks the server straight away — and it is measured in
     * uptime seconds rather than wall-clock seconds, so correcting the clock
     * cannot make the number jump (the correction is what the client is for).
     *
     * @note The interval is a schedule, not a promise: when a poll goes
     *       unanswered the network stack retries on its own, sooner than the
     *       number on the page, and the counter here is not told about it.
     */
    struct SyncStatus {
        bool counting = false;      ///< the countdown means something
        uint32_t remainingSec = 0;  ///< seconds left in the current interval
        uint32_t periodSec = 0;     ///< the configured interval
    };

    /** @brief Mirror of the state the page's "Next sync in" row draws. */
    SyncStatus syncStatus() const;

    // ─── Configuration ──────────────────────────────
    /**
     * @brief External NTP servers used for synchronisation (SNTP), in order.
     *
     * The client is given the whole list (lwIP moves to the next server when one
     * stops answering), which is the defence against one dead node in a pool:
     * the first address of an answer is what the client used to be stuck with
     * (stage 195). The strings are kept in this object because lwIP stores the
     * POINTER to each name rather than a copy, so replacing the list while the
     * client runs would free what it is still reading - which is why setting it
     * stops the client, and the caller starts it again.
     */
    void setServers(const std::vector<std::string>& names);
    const std::vector<std::string>& servers() const { return servers_; }
    /** @brief The list as one text, the shape the settings are stored in. */
    std::string serverText() const;

    /** @brief SNTP re-synchronisation interval (seconds). */
    void setSyncIntervalSec(uint32_t sec) { syncIntervalSec_ = sec; }
    uint32_t syncIntervalSec() const { return syncIntervalSec_; }

    /** @brief Timezone offset (hours from UTC) — display only. */
    void setUtcOffsetHours(int hours) { utcOffsetHours_ = hours; }
    int utcOffsetHours() const { return utcOffsetHours_; }

    /** @brief Timezone id (e.g. "Europe/Moscow"); empty = custom offset. */
    void setTimezoneName(const std::string& name) { timezoneName_ = name; }
    const std::string& timezoneName() const { return timezoneName_; }

    /** @brief Stratum advertised to NTP clients. */
    uint8_t stratum() const { return stratum_; }

    /** @brief Access the request logger (terminal + REST). */
    TimeLogger& logger() { return logger_; }

    /**
     * @brief The DHCP server, asked who owns the address of an NTP client.
     *
     * The "only allowed computers" filter needs it (stage 196): an NTP request
     * carries no MAC, so the lease table is the only way to find one. Wired once
     * at boot, like the DNS server's own fallback lookup.
     */
    void setDhcpServer(::dhcp::dhcp::IDhcpServer* dhcp) { dhcpServer_ = dhcp; }

    // ─── Time queries ───────────────────────────────
    /** @brief Current UTC time (Unix seconds, 0 if not available). */
    uint32_t nowUtcSec() const;
    /** @brief Current UTC time as "YYYY-MM-DD HH:MM:SS". */
    std::string nowUtcString() const;
    /** @brief Local time (UTC + offset) as "YYYY-MM-DD HH:MM:SS". */
    std::string nowLocalString() const;
    /** @brief Uptime in seconds since boot. */
    uint32_t uptimeSec() const;

    /** @brief Re-read the external NTP server/interval and restart SNTP. */
    void restartSync();

    // ─── The request log as a file (stage 191) ──────
    /**
     * @brief Where the file log lives, told once by whoever knows the volumes.
     *
     * @param fatMountPoint  e.g. `/fat` → the live file is `<it>/logs/Ntp.log`.
     * @param cardMountPoint e.g. `/sdcard` → where a full file is archived; ""
     *                       when the board has no card slot.
     * @param cardMounted    Asked at every rotation: a card can be inserted and
     *                       removed while the device runs.
     */
    void setFileLogVolumes(const std::string& fatMountPoint,
                           const std::string& cardMountPoint,
                           TimeLogger::CardMountedCheck cardMounted)
    {
        logger_.setFileLogVolumes(fatMountPoint, cardMountPoint, std::move(cardMounted));
    }

    /**
     * @brief Hand the configured zone to the C library, so that `localtime()` is
     *        the operator's clock and not UTC (stage 186).
     *
     * Until this stage nothing set `TZ` at all, and the offset the operator saved
     * was applied only where a string was built for a page (`nowLocalString()`).
     * The clocks therefore disagreed: a page said 01:34 while the log line for
     * the same event said 22:34, and the per-sweep log file would have carried
     * UTC in its name while the record lines inside carried… the same wrong hour.
     *
     * The value is derived from `utc_offset_hours` (whole hours, the setting the
     * page and the import clamp) by `time::posixTimezone`, and the zone *name* is
     * deliberately not used: it is free text for display, and newlib would need
     * the tzdata of a named zone, which this build does not carry.
     *
     * Called where the time settings are applied: at boot and whenever they
     * change. Daylight saving is not modelled — the saved offset is the offset in
     * force, all year.
     */
    void applyTimezone();

    // ─── Clock-set notification ─────────────────────
    /**
     * @brief What the callback of @ref setOnClockSet looks like.
     *
     * @warning It must not block: the SNTP notification runs in the context of
     * the network task, whose stack has no room for file I/O or a server start.
     * A listener that needs that work has to hand it to a task of its own.
     */
    using ClockSetCallback = std::function<void()>;

    /**
     * @brief Report that the device clock became a real date, not an epoch.
     *
     * Called once per successful synchronisation and once per manual clock
     * setting, because a certificate is a statement about time: whoever judges
     * a stored pair has to be told when the clock arrives instead of deciding
     * with the epoch it was born with.
     */
    void setOnClockSet(ClockSetCallback cb) { onClockSet_ = std::move(cb); }

    // ─── Access control (LAN hardening) ─────────────
    /**
     * @brief (Re)apply the two client filters and the per-client rate limit.
     *
     * Reads the on/off flags and the replies/second limit from the time config
     * and the network definition (device address + netmask) from the DHCP
     * settings, which own the subnet. Called from start() and whenever the time
     * or DHCP settings change. When the own-subnet flag is on but the DHCP subnet
     * cannot be parsed the filter is skipped with a warning (fail-open) — a broken
     * subnet must not cut the LAN off from time service.
     */
    void applyAccessFilter();

    /** @brief NTP requests dropped by the own-subnet filter. */
    uint32_t foreignDroppedCount() const { return foreignDropped_; }
    /** @brief NTP requests dropped by the per-client rate limit. */
    uint32_t rateLimitedCount() const { return rateLimited_; }
    /** @brief NTP requests dropped as "not an allowed computer" (stage 196). */
    uint32_t notAllowedDroppedCount() const { return notAllowedDropped_; }

    // ─── Manual clock setting ───────────────────────
    /**
     * @brief Set the device clock by hand (operator-supplied UTC time).
     *
     * The SNTP client is deliberately left untouched — a later successful
     * sync may overwrite this value. On success the clock counts as
     * synchronised, so the NTP server starts answering with LI=0 instead of
     * the "not synchronised" gate (LI=3 / stratum 16).
     *
     * @param unixUtc Seconds since 1970-01-01 UTC (must be > 0).
     * @return true when the system clock was updated.
     */
    bool setUtcTime(uint32_t unixUtc);

private:
    static void serverTask(void* arg);
    void serverLoop();

    /** @brief SNTP synchronisation-completed notification callback. */
    static void onSyncNotification(struct timeval* tv);
    /** @brief Shared instance used by the static SNTP callback. */
    static TimeServer* s_instance;
    /** @brief Tell the registered listener that the clock is usable now. */
    void notifyClockSet();

    /**
     * @brief Start counting the interval from this very moment (stage 190).
     *
     * Called wherever the SNTP client's schedule begins: a successful
     * synchronisation, and the start/restart of the client (poll mode asks the
     * server immediately, so the next poll is one interval away).
     */
    void markSyncBaseline();

    // ─── The device's own attempts in the log (stage 192) ───
    /** @brief Remember the clock as it is now, for the success line's estimate. */
    void rememberClockReading();
    /** @brief Arm (or re-arm) the "still not synchronised" line. */
    void armSyncStateTimer();
    /** @brief Stop watching for a reply (a success, a stop, or the clock set). */
    void stopSyncStateTimer();
    /**
     * @brief Stop the SNTP client without saying so in the journal.
     *
     * A restart and a settings change are one event: the "SNTP client started"
     * line that follows names the list and the interval, so a "stopped" line in
     * the same second would only make the file longer.
     */
    void stopSyncQuiet();
    /** @brief One tick of that timer: say so, then wait longer next time. */
    static void onSyncStateTimer(void* arg);

    // ─── Resolver and server diagnostics (stages 193, 195) ───
    /**
     * @brief Write the state line: how long the wait has been, and the DNS verdict.
     *
     * The verdict comes from the probe of the same beat, whose entries are handed
     * in - the first server's outcome is what the line reports. With no entries
     * (the probe task could not be started) the line says the lookup is pending
     * rather than inventing a result.
     */
    void writeSyncStateLine(const std::vector<NtpProbeEntry>& entries = {});
    /**
     * @brief Start the one-shot task that asks the configured servers who answers.
     *
     * The `esp_timer` callback cannot block for a resolution and a socket wait,
     * so the beat hands the work to a task which writes both lines (the probe's
     * and the state line) and re-arms the schedule when it is done.
     */
    void startProbeTask();
    /** @brief The probe task itself; it removes itself when the run is over. */
    static void probeTask(void* arg);
    /** @brief Log the resolver chain; read from the settings, no network needed. */
    void logResolverChain();

    static constexpr int kPort = kNtpPort;
    static constexpr uint8_t kStratum = 3;
    /** Stack of the probe task: a resolution and a socket, nothing recursive. */
    static constexpr uint32_t kProbeTaskStackBytes = 4096;
    /** Low priority: a diagnostic must never compete with the clients' replies. */
    static constexpr UBaseType_t kProbeTaskPriority = tskIDLE_PRIORITY + 1;
    static constexpr const char* kProbeTaskName = "ntp_probe";

    // Per-client sliding-window rate limiter. The set of NTP clients on a home
    // LAN is tiny, so a small fixed table of the most recent addresses is
    // enough (a new address evicts the oldest slot).
    static constexpr int kRateSlots = 8;
    struct RateSlot {
        uint32_t ip = 0;             // host byte order, 0 = free
        uint64_t windowStartMs = 0;  // start of the current 1 s window
        uint32_t count = 0;          // replies sent in that window
    };

    /** @brief True when this client exceeded the limit (also advances it). */
    bool rateLimited(uint32_t clientIp, uint64_t nowMs);

    TimeServerState state_ = TimeServerState::STOPPED;
    TaskHandle_t taskHandle_ = nullptr;
    int socketFd_ = -1;
    volatile bool stopRequested_ = false;
    volatile bool synced_ = false;

    bool sntpStarted_ = false;
    // Uptime seconds of the moment the interval started counting for the page's
    // "Next sync in" row (stage 190). Two words on purpose: a 64-bit field
    // written by the network task and read by the web task could tear, while a
    // 32-bit one cannot, and 32 bits of seconds outlive any power cycle.
    volatile uint32_t lastSyncUptimeSec_ = 0;
    volatile bool syncBaselineSet_ = false;

    // The device's own attempts (stage 192): the timer that keeps saying "still
    // not synchronised" while no reply arrives, how many such lines were already
    // written (the schedule grows), when the current client run started, and the
    // last reading of the clock the success line's estimate is built from.
    esp_timer_handle_t syncStateTimer_ = nullptr;
    size_t syncStateIndex_ = 0;
    uint32_t syncRunUptimeSec_ = 0;
    int64_t prevClockUtcSec_ = 0;
    uint32_t prevClockUptimeSec_ = 0;
    bool prevClockSet_ = false;

    // Server diagnostics (stage 195): the one-shot task asks each configured
    // server who answers, and its run is tied to the client run that started it -
    // `syncRunUptimeSec_` is what tells a late answer from a stale one.
    bool probeRunning_ = false;
    ClockSetCallback onClockSet_;
    std::vector<std::string> servers_ = core::ntpServersSplit(core::kNtpDefaultServers);
    uint32_t syncIntervalSec_ = 86400;  // 24 h
    std::string timezoneName_ = "Europe/Moscow"; // zone id (display only)
    int utcOffsetHours_ = 3;            // MSK (display only)
    uint8_t stratum_ = kStratum;

    // Access control (applyAccessFilter)
    // The "own subnet only" decision lives in `core::SubnetFilter`, shared with the
    // DNS server and the file manager (stage 200).
    core::SubnetFilter subnetFilter_;
    /** Drop clients the DHCP side would refuse as well (stage 196). */
    bool onlyAllowedComputers_ = false;
    ::dhcp::dhcp::IDhcpServer* dhcpServer_ = nullptr;
    uint32_t rateLimitPerSec_ = 5;     // replies/second/IP (0 = no limit)
    RateSlot rateSlots_[kRateSlots];
    uint32_t foreignDropped_ = 0;      // dropped by the subnet filter
    uint32_t notAllowedDropped_ = 0;   // dropped by the "allowed computers" filter
    uint32_t rateLimited_ = 0;         // dropped by the rate limit
    uint64_t lastDropLogMs_ = 0;       // throttles the drop log line

    TimeLogger logger_;
};

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_TIMESERVER_H
