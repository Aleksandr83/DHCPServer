#ifndef DHCP_TIME_TIMESERVER_H
#define DHCP_TIME_TIMESERVER_H

#include "ITimeServer.h"
#include "TimeLogger.h"

#include <functional>
#include <string>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Forward declaration of the SNTP notification callback parameter type
// (defined in <sys/time.h>); avoids pulling the header into every includer.
struct timeval;

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

    // ─── Configuration ──────────────────────────────
    /** @brief External NTP server used for synchronisation (SNTP). */
    void setServerName(const std::string& name) { serverName_ = name; }
    const std::string& serverName() const { return serverName_; }

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
     * @brief (Re)apply the "own subnet only" filter and the per-client rate
     * limit.
     *
     * Reads the on/off flag and the replies/second limit from the time config
     * and the network definition (device address + netmask) from the DHCP
     * settings, which own the subnet. Called from start() and whenever the time
     * or DHCP settings change. When the flag is on but the DHCP subnet cannot
     * be parsed the filter is skipped with a warning (fail-open) — a broken
     * subnet must not cut the LAN off from time service.
     */
    void applyAccessFilter();

    /** @brief NTP requests dropped by the own-subnet filter. */
    uint32_t foreignDroppedCount() const { return foreignDropped_; }
    /** @brief NTP requests dropped by the per-client rate limit. */
    uint32_t rateLimitedCount() const { return rateLimited_; }

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

    static constexpr int kPort = 123;
    static constexpr uint8_t kStratum = 3;

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
    ClockSetCallback onClockSet_;
    std::string serverName_ = "pool.ntp.org";
    uint32_t syncIntervalSec_ = 86400;  // 24 h
    std::string timezoneName_ = "Europe/Moscow"; // zone id (display only)
    int utcOffsetHours_ = 3;            // MSK (display only)
    uint8_t stratum_ = kStratum;

    // Access control (applyAccessFilter)
    bool allowOwnSubnet_ = false;      // drop clients outside subnetNet_/Mask_
    uint32_t subnetNet_ = 0;           // host byte order
    uint32_t subnetMask_ = 0;          // host byte order (0 = not configured)
    uint32_t rateLimitPerSec_ = 5;     // replies/second/IP (0 = no limit)
    RateSlot rateSlots_[kRateSlots];
    uint32_t foreignDropped_ = 0;      // dropped by the subnet filter
    uint32_t rateLimited_ = 0;         // dropped by the rate limit
    uint64_t lastDropLogMs_ = 0;       // throttles the drop log line

    TimeLogger logger_;
};

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_TIMESERVER_H
