#include "TimeServer.h"
#include "NtpMessage.h"
#include "NtpProbe.h"
#include "PosixTimezone.h"
#include "TimeMath.h"
#include "core/ErrorLogCore.h"
#include "../core/Config.h"
#include "../core/Subnet.h"
#include "../dhcp/IDhcpServer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "sys/time.h"
// `CONFIG_LWIP_SNTP_MAX_SERVERS` below is the build's limit on configured
// servers, which lwipopts.h turns into lwIP's own SNTP_MAX_SERVERS. lwIP's
// `lwip/apps/sntp.h` is deliberately NOT included here: it redeclares the same
// API `esp_sntp.h` declares, and the two do not compile together.
#include "sdkconfig.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

using namespace std;
// Rule 40/42 note: `time` is a namespace here, so the log helpers and the clock
// are named in full — `core::ntpSyncStateDelaySec`, `std::time`.
using dhcp::core::NtpDnsVerdict;
using dhcp::core::ntpDnsVerdictText;
using dhcp::core::ntpLogClockShiftText;
using dhcp::core::ntpResolverChainText;
using dhcp::core::ntpSyncStateDelaySec;
using dhcp::core::ntpSyncStateText;

namespace {

// Rule 39: the field bases of struct tm — the year counts from 1900, the month
// from 0 — and the scale that turns a whole-hour offset into seconds.
constexpr int kTmYearBase = 1900;
constexpr int kTmMonthBase = 1;
constexpr int64_t kSecondsPerHour = 3600;

// Rule 39: the scale `esp_timer` counts in, which is how a delay in seconds
// becomes the delay the timer wants.
constexpr uint64_t kMicrosecondsPerSecond = 1000000ULL;

// Rule 39: the NTP task formats replies and touches no large buffer.
constexpr uint32_t kServerTaskStackBytes = 4096;

// Rule 39: "255.255.255.255" plus the NUL, the room a client address needs.
constexpr size_t kIp4TextLen = 16;

// How many servers lwIP can be given at all: its own failover covers the indices
// this build's limit holds, so a name handed over beyond it would be stored and
// never asked - the list would look longer than it is.
constexpr size_t kServersLwipAccepts = CONFIG_LWIP_SNTP_MAX_SERVERS;

} // namespace

static const char* TAG = "TimeServer";

namespace dhcp {
namespace time {

TimeServer* TimeServer::s_instance = nullptr;

// ─────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────

TimeServer::TimeServer()
{
}

TimeServer::~TimeServer()
{
    stop();
    stopSyncStateTimer();
    if (syncStateTimer_ != nullptr) {
        esp_timer_delete(syncStateTimer_);
        syncStateTimer_ = nullptr;
    }
    logger_.stopRestSender();
}

// ─────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────

bool TimeServer::start()
{
    if (state_ == TimeServerState::RUNNING) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    s_instance = this;
    stopRequested_ = false;

    applyAccessFilter();

    BaseType_t res = xTaskCreatePinnedToCore(
        serverTask, "ntp_server", kServerTaskStackBytes, this,
        configMAX_PRIORITIES - 3, &taskHandle_, 0);
    if (res != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create NTP server task");
        state_ = TimeServerState::ERROR;
        return false;
    }
    state_ = TimeServerState::RUNNING;
    ESP_LOGI(TAG, "NTP server started (UDP %d, external NTP: %s)",
             kPort, serverText().c_str());
    return true;
}

void TimeServer::stop()
{
    if (state_ == TimeServerState::STOPPED) return;
    stopRequested_ = true;
    // The task wakes up on the select() timeout and closes the socket.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    while (taskHandle_ != nullptr && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    state_ = TimeServerState::STOPPED;
    ESP_LOGI(TAG, "NTP server stopped");
}

void TimeServer::startSync()
{
    if (sntpStarted_) return;
    s_instance = this;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    // The whole list goes to lwIP, which keeps one name per index and hands over
    // to the next one when a server stops answering (SNTP_RECV_TIMEOUT, 15 s):
    // that failover is the reason for the list at all, since the client used to
    // be pinned to the first address of its resolver's answer (stage 195). The
    // loop stops at the number of servers this build can hold.
    const size_t asked = servers_.size() < kServersLwipAccepts
                             ? servers_.size()
                             : kServersLwipAccepts;
    for (size_t i = 0; i < asked; ++i) {
        esp_sntp_setservername(static_cast<u8_t>(i), servers_[i].c_str());
    }
    // The indices a shorter list no longer uses are cleared: lwIP keeps them for
    // the life of the process, and a name left there would still be asked.
    for (size_t i = asked; i < kServersLwipAccepts; ++i) {
        esp_sntp_setservername(static_cast<u8_t>(i), nullptr);
    }
    esp_sntp_set_time_sync_notification_cb(&TimeServer::onSyncNotification);
    esp_sntp_set_sync_interval(syncIntervalSec_ * 1000u);
    esp_sntp_init();
    sntpStarted_ = true;
    // Poll mode asks the server right away, so the next poll is one interval
    // from this moment — that is what the page counts down (stage 190).
    markSyncBaseline();
    ESP_LOGI(TAG, "SNTP client started (servers=%s, interval=%us)",
             serverText().c_str(), (unsigned)syncIntervalSec_);

    // The device's own attempts go into the log as well (stage 192): a device
    // that cannot synchronise has to say so itself, or the only thing in the file
    // is what its clients asked.
    syncRunUptimeSec_ = uptimeSec();
    syncStateIndex_ = 0;
    probeRunning_ = false;
    rememberClockReading();
    logger_.logEvent("SNTP client started: " + serverText() + ", interval " +
                     to_string(syncIntervalSec_) + " s");
    logResolverChain();
    armSyncStateTimer();
}

void TimeServer::setServers(const vector<string>& names)
{
    // A settings save that did not touch the list must not stir the client: the
    // clock-sync switch is one of those saves, and its "off" has to reach the
    // journal as the stop it is - the stop path writes that line, not this one.
    if (names == servers_) return;
    // lwIP keeps the POINTER to each name, not a copy, so the storage has to
    // outlive the client's use of it: replacing the list under a running client
    // would free the strings the resolver is still reading. Stopping it here
    // means any caller order is safe - the apply that follows starts it again.
    stopSyncQuiet();
    servers_ = names;
}

string TimeServer::serverText() const
{
    string out;
    for (const string& name : servers_) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

void TimeServer::rememberClockReading()
{
    prevClockUtcSec_ = static_cast<int64_t>(nowUtcSec());
    prevClockUptimeSec_ = uptimeSec();
    // A clock that has never been set is not a reading to compare against: the
    // success line says so instead of reporting a shift of half a century.
    prevClockSet_ = core::ErrorLogCore::clockIsSet(static_cast<uint64_t>(prevClockUtcSec_));
}

void TimeServer::armSyncStateTimer()
{
    if (syncStateTimer_ == nullptr) {
        esp_timer_create_args_t cfg = {};
        cfg.callback = &TimeServer::onSyncStateTimer;
        cfg.arg = this;
        cfg.name = "ntp_syncstate";
        if (esp_timer_create(&cfg, &syncStateTimer_) != ESP_OK) {
            syncStateTimer_ = nullptr;   // the log then simply has no state lines
            return;
        }
    }
    esp_timer_stop(syncStateTimer_);   // harmless when it is not running
    const uint64_t delayUs = static_cast<uint64_t>(ntpSyncStateDelaySec(syncStateIndex_)) *
                             kMicrosecondsPerSecond;
    esp_timer_start_once(syncStateTimer_, delayUs);
}

void TimeServer::stopSyncStateTimer()
{
    if (syncStateTimer_ != nullptr) esp_timer_stop(syncStateTimer_);
    syncStateIndex_ = 0;
}

void TimeServer::onSyncStateTimer(void* arg)
{
    auto* self = static_cast<TimeServer*>(arg);
    if (self == nullptr) return;

    // A reply arrived, or the client was stopped: the schedule is over. No line
    // is written here — the success line is the notification's job (and a manual
    // clock set lifts the same flag, which is why this test is the flag and not
    // the client's state).
    if (self->synced_ || !self->sntpStarted_) {
        self->stopSyncStateTimer();
        return;
    }

    // The beat itself is the probe's, because the probe must not run here: this
    // is an esp_timer callback, and a resolution plus a socket wait would block
    // every other timer on the device. It hands the work to a task that writes
    // both lines and re-arms this schedule when it is done, so the two lines of
    // one beat stay in order and the next beat is measured from the last one.
    if (self->probeRunning_) {
        // Cannot normally happen: the probe re-arms this timer only after it has
        // finished. If it does, the probe is the owner of the schedule.
        return;
    }
    self->startProbeTask();
}

void TimeServer::startProbeTask()
{
    if (servers_.empty()) {
        // Nothing configured: there is no name to ask and no address to report,
        // so the state line goes out on its own.
        writeSyncStateLine();
        return;
    }
    probeRunning_ = true;
    if (xTaskCreate(&TimeServer::probeTask, kProbeTaskName, kProbeTaskStackBytes, this,
                    kProbeTaskPriority, nullptr) != pdPASS) {
        // Out of task memory: the diagnostic is the first thing to lose, and the
        // line says the lookup has no result rather than inventing one.
        probeRunning_ = false;
        writeSyncStateLine();
    }
}

void TimeServer::probeTask(void* arg)
{
    auto* self = static_cast<TimeServer*>(arg);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    // The run this probe belongs to: a settings save restarts the client, and a
    // probe that outlives its run must not write lines for the new one.
    const uint32_t runUptime = self->syncRunUptimeSec_;
    const vector<string> servers = self->servers_;

    const NtpProbe probe;
    const vector<NtpProbeEntry> entries = probe.run(servers);

    const bool currentRun = (self->syncRunUptimeSec_ == runUptime);
    if (currentRun) {
        // A reply arrived (or the clock was set by hand) while the probe ran: the
        // verdicts are stale and "still not synchronised" would be a lie.
        if (self->sntpStarted_ && !self->synced_) {
            self->logger_.logEvent(ntpProbeLine(entries, kNtpProbeTimeoutMs));
            self->writeSyncStateLine(entries);
        }
        self->probeRunning_ = false;
    }
    vTaskDelete(nullptr);
}

void TimeServer::writeSyncStateLine(const vector<NtpProbeEntry>& entries)
{
    const uint32_t waited = uptimeSec() - syncRunUptimeSec_;

    // The verdict is the first server's - the one the client asks first. With no
    // probe result at all the line says the lookup is pending instead of claiming
    // a failure it never observed.
    NtpDnsVerdict verdict = NtpDnsVerdict::Pending;
    string resolvedIp;
    if (!entries.empty()) {
        resolvedIp = entries.front().ip;
        switch (entries.front().verdict) {
            case NtpProbeVerdict::Replied:
            case NtpProbeVerdict::NoReply:   verdict = NtpDnsVerdict::Ok; break;
            case NtpProbeVerdict::Unresolved: verdict = NtpDnsVerdict::Failed; break;
            case NtpProbeVerdict::NotAsked:  verdict = NtpDnsVerdict::Pending; break;
        }
    }

    logger_.logEvent(ntpSyncStateText(waited, serverText(),
                                      ntpDnsVerdictText(verdict, resolvedIp)));
    rememberClockReading();
    ++syncStateIndex_;
    armSyncStateTimer();
}

void TimeServer::logResolverChain()
{
    // Read from the settings, not from the network: the point is to show what the
    // device *would* ask, next to the verdict of asking it.
    auto dhcp = core::Config::instance().getDhcp();
    auto dns = core::Config::instance().getDns();
    logger_.logEvent(ntpResolverChainText(dhcp.serverIp, dns.externalDns, dhcp.gateway));
}

void TimeServer::markSyncBaseline()
{
    lastSyncUptimeSec_ = uptimeSec();
    syncBaselineSet_ = true;
}

TimeServer::SyncStatus TimeServer::syncStatus() const
{
    SyncStatus status;
    status.periodSec = syncIntervalSec_;
    if (!sntpStarted_ || !syncBaselineSet_) return status;   // nothing to count
    status.counting = true;
    status.remainingSec = TimeMath::remainingSec(uptimeSec(), lastSyncUptimeSec_,
                                                 syncIntervalSec_);
    return status;
}

void TimeServer::stopSyncQuiet()
{
    // The stop half with neither the journal line nor the terminal log: a restart
    // and a settings change are one event, and the "started" line that follows
    // says everything the pair would (the same shape restartSync() always had).
    if (sntpStarted_) {
        esp_sntp_stop();
        sntpStarted_ = false;
        stopSyncStateTimer();
    }
}

void TimeServer::stopSync()
{
    if (sntpStarted_) {
        stopSyncQuiet();
        ESP_LOGI(TAG, "SNTP client stopped");
        logger_.logEvent("SNTP client stopped");
    }
}

void TimeServer::restartSync()
{
    stopSyncQuiet();
    startSync();
    ESP_LOGI(TAG, "SNTP restarted (servers=%s, interval=%us)",
             serverText().c_str(), (unsigned)syncIntervalSec_);
}

// ─────────────────────────────────────────────────────
// Access control
// ─────────────────────────────────────────────────────

void TimeServer::applyTimezone()
{
    // Read from the settings, like applyAccessFilter() above: this is called when
    // they change and at boot, and a copy in a member would be one more thing that
    // can disagree with what the operator saved.
    const auto timeCfg = core::Config::instance().getTime();
    utcOffsetHours_ = timeCfg.utcOffsetHours;   // nowLocalString() reads this one

    const std::string tz = time::posixTimezone(utcOffsetHours_);
#if defined(_WIN32)
    // The host tests never come here: TZ in the tests would make them depend on
    // the machine they run on. Kept honest for a host build of the server anyway.
    _putenv_s("TZ", tz.c_str());
    _tzset();
#else
    setenv("TZ", tz.c_str(), 1);
    tzset();
#endif
    ESP_LOGI(TAG, "timezone applied: TZ=%s (UTC%+d), localtime() follows it",
             tz.c_str(), utcOffsetHours_);
}

void TimeServer::applyAccessFilter()
{
    const auto timeCfg = core::Config::instance().getTime();
    allowOwnSubnet_ = timeCfg.allowOwnSubnet;
    // The allowed-computers filter is the DHCP side's list and policy (stage
    // 196); it needs the DHCP server to resolve an address to its owner.
    onlyAllowedComputers_ = timeCfg.onlyAllowedComputers;
    rateLimitPerSec_ = timeCfg.rateLimitPerSec;
    ESP_LOGI(TAG, "Client filters: own subnet %s, allowed computers %s",
             allowOwnSubnet_ ? "on" : "off", onlyAllowedComputers_ ? "on" : "off");

    if (!allowOwnSubnet_) {
        subnetNet_ = 0;
        subnetMask_ = 0;
        ESP_LOGI(TAG, "Own-subnet filter disabled (rate limit %u/s per IP)",
                 (unsigned)rateLimitPerSec_);
        return;
    }

    // The subnet is owned by the DHCP page (device address + netmask).
    const auto dhcpCfg = core::Config::instance().getDhcp();
    uint32_t ip = 0, mask = 0;
    const bool ipOk = core::Subnet::parseIp4(dhcpCfg.serverIp, ip);
    const bool maskOk = core::Subnet::parseIp4(dhcpCfg.subnet, mask) &&
                        core::Subnet::isValidMask(mask);
    if (!ipOk || !maskOk) {
        // Fail-open with a loud warning: a broken subnet must not stop the
        // LAN from getting time.
        allowOwnSubnet_ = false;
        subnetNet_ = 0;
        subnetMask_ = 0;
        ESP_LOGW(TAG, "Own-subnet filter requested but the DHCP subnet is "
                      "invalid (server_ip='%s', subnet='%s') — filter SKIPPED",
                 dhcpCfg.serverIp.c_str(), dhcpCfg.subnet.c_str());
        return;
    }

    subnetNet_ = core::Subnet::network(ip, mask);
    subnetMask_ = mask;
    ESP_LOGI(TAG, "Own-subnet filter enabled: %s/%d, rate limit %u/s per IP",
             core::Subnet::toString(subnetNet_).c_str(),
             core::Subnet::prefixLength(subnetMask_),
             (unsigned)rateLimitPerSec_);
}

bool TimeServer::rateLimited(uint32_t clientIp, uint64_t nowMs)
{
    if (rateLimitPerSec_ == 0) return false;

    RateSlot* oldest = &rateSlots_[0];
    for (int i = 0; i < kRateSlots; ++i) {
        RateSlot& slot = rateSlots_[i];
        if (slot.ip == clientIp) {
            // 1 s sliding window: past it the counter starts over.
            if (nowMs - slot.windowStartMs >= 1000) {
                slot.windowStartMs = nowMs;
                slot.count = 0;
            }
            if (slot.count >= rateLimitPerSec_) return true;
            ++slot.count;
            return false;
        }
        if (slot.ip == 0) {
            slot.ip = clientIp;
            slot.windowStartMs = nowMs;
            slot.count = 1;
            return false;
        }
        if (slot.windowStartMs < oldest->windowStartMs) oldest = &slot;
    }

    // Table full — evict the client whose window is the oldest one.
    oldest->ip = clientIp;
    oldest->windowStartMs = nowMs;
    oldest->count = 1;
    return false;
}

void TimeServer::onSyncNotification(struct timeval* tv)
{
    if (!s_instance) return;
    s_instance->synced_ = true;
    // A confirmed answer is the other moment the interval starts counting from
    // (stage 190): the next poll is one interval away, and that is what the
    // page's "Next sync in" row shows.
    s_instance->markSyncBaseline();

    // The answer itself goes into the log (stage 192), with an estimate of how far
    // the clock was off: ESP-IDF sets the clock before calling us, so the shift is
    // what the clock says now against what the last reading said it would be by
    // now. A device that had no clock at all says that instead of a number.
    s_instance->stopSyncStateTimer();
    const int64_t nowUtc = static_cast<int64_t>(s_instance->nowUtcSec());
    const string shift = ntpLogClockShiftText(s_instance->prevClockUtcSec_,
                                              s_instance->prevClockUptimeSec_,
                                              nowUtc, s_instance->uptimeSec(),
                                              s_instance->prevClockSet_);
    s_instance->logger_.logEvent("SNTP synchronised: " + s_instance->serverText() +
                                 " (" + shift + ")");
    s_instance->rememberClockReading();

    if (tv) {
        ESP_LOGI(TAG, "SNTP synchronised: %ld.%06ld",
                 (long)tv->tv_sec, (long)tv->tv_usec);
    } else {
        ESP_LOGI(TAG, "SNTP synchronised");
    }
    s_instance->notifyClockSet();
}

void TimeServer::notifyClockSet()
{
    // Nothing starts here: the listener decides what a clock is worth to it, and
    // this may run in the network task (see ClockSetCallback).
    if (onClockSet_) onClockSet_();
}

// ─────────────────────────────────────────────────────
// Time queries
// ─────────────────────────────────────────────────────

uint32_t TimeServer::nowUtcSec() const
{
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) return 0;
    return static_cast<uint32_t>(tv.tv_sec);
}

uint32_t TimeServer::uptimeSec() const
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

string TimeServer::nowUtcString() const
{
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) return "";
    time_t secs = tv.tv_sec;
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    char buf[80];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + kTmYearBase, tmv.tm_mon + kTmMonthBase, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return string(buf);
}

string TimeServer::nowLocalString() const
{
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) return "";
    time_t secs = tv.tv_sec + static_cast<time_t>(utcOffsetHours_) * kSecondsPerHour;
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    char buf[80];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + kTmYearBase, tmv.tm_mon + kTmMonthBase, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return string(buf);
}

string TimeServer::stateString() const
{
    switch (state_) {
    case TimeServerState::RUNNING: return "running";
    case TimeServerState::ERROR:   return "error";
    default:                       return "stopped";
    }
}

// ─────────────────────────────────────────────────────
// Manual clock setting
// ─────────────────────────────────────────────────────

bool TimeServer::setUtcTime(uint32_t unixUtc)
{
    if (unixUtc == 0) {
        ESP_LOGW(TAG, "Manual time rejected (epoch/unset value)");
        return false;
    }

    struct timeval tv;
    tv.tv_sec  = static_cast<time_t>(unixUtc);
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGE(TAG, "Failed to set the system clock to %u", (unsigned)unixUtc);
        return false;
    }

    // A hand-set clock is a valid time source: lift the "not synchronised"
    // gate so the NTP server starts serving it (the SNTP client itself is
    // deliberately left running — a later sync may correct this value).
    synced_ = true;
    ESP_LOGI(TAG, "System clock set manually to %s UTC", nowUtcString().c_str());
    // A hand-set clock is worth a line of its own in the log (stage 192): it
    // explains a device that serves time (and stops waiting for a reply) without
    // any NTP answer having arrived.
    stopSyncStateTimer();
    rememberClockReading();
    logger_.logEvent("clock set by hand: " + nowLocalString());
    notifyClockSet();
    return true;
}

// ─────────────────────────────────────────────────────
// Server task
// ─────────────────────────────────────────────────────

void TimeServer::serverTask(void* arg)
{
    auto* self = static_cast<TimeServer*>(arg);
    self->serverLoop();
    self->taskHandle_ = nullptr;
    vTaskDelete(nullptr);
}

void TimeServer::serverLoop()
{
    socketFd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketFd_ < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        state_ = TimeServerState::ERROR;
        return;
    }
    int reuse = 1;
    setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(socketFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed on UDP %d", kPort);
        close(socketFd_);
        socketFd_ = -1;
        state_ = TimeServerState::ERROR;
        return;
    }
    ESP_LOGI(TAG, "NTP server listening on UDP %d", kPort);

    while (!stopRequested_) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socketFd_, &readfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int sel = select(socketFd_ + 1, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        uint8_t buf[128];
        ssize_t n = recvfrom(socketFd_, buf, sizeof(buf), 0,
                             (struct sockaddr*)&from, &fromLen);
        if (n < static_cast<ssize_t>(sizeof(NtpPacket))) continue;

        NtpPacket req;
        memcpy(&req, buf, sizeof(NtpPacket));
        const uint8_t mode = ntpMode(req);
        // Accept client (3) and symmetric-active (1) requests.
        if (mode != kNtpModeClient && mode != kNtpModeSymmetricActive) {
            continue;
        }

        // ─── Access control ──────────────────────────
        // Both checks drop the request WITHOUT a reply: no reply means the
        // device is neither an open time service nor a reflection amplifier.
        const uint32_t clientAddr = ntohl(from.sin_addr.s_addr);
        const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
        const char* dropReason = nullptr;
        if (allowOwnSubnet_ &&
            !core::Subnet::contains(subnetNet_, subnetMask_, clientAddr)) {
            ++foreignDropped_;
            dropReason = "outside the local subnet";
        } else if (dhcpServer_ != nullptr &&
                   !dhcpServer_->isClientAddressAllowed(from.sin_addr.s_addr,
                                                        onlyAllowedComputers_)) {
            // The address, not the MAC: an NTP request carries no hardware
            // address, and the DHCP side answers who owns the one it came from
            // - by lease, by the ARP cache, or by a static binding. Nobody
            // owning it counts as "not allowed" (stage 196).
            ++notAllowedDropped_;
            dropReason = "not an allowed computer";
        } else if (rateLimited(clientAddr, nowMs)) {
            ++rateLimited_;
            dropReason = "rate limit exceeded";
        }
        if (dropReason) {
            // Throttle the log: a flood must not flood the console as well.
            if (nowMs - lastDropLogMs_ >= 5000) {
                lastDropLogMs_ = nowMs;
                char clientIpStr[kIp4TextLen];
                inet_ntop(AF_INET, &from.sin_addr, clientIpStr, sizeof(clientIpStr));
                ESP_LOGW(TAG, "NTP request from %s dropped (%s; totals: %u subnet, "
                              "%u not allowed, %u rate)",
                         clientIpStr, dropReason,
                         static_cast<unsigned>(foreignDropped_),
                         static_cast<unsigned>(notAllowedDropped_),
                         static_cast<unsigned>(rateLimited_));
            }
            continue;
        }

        // ─── Build the reply ─────────────────────────
        // Unsynced gate: while the clock has never been synchronised, do NOT
        // serve a (wrong) time. Answer with LI=3 ("clock not synchronized")
        // and stratum 16 (RFC 5905) — compliant clients ignore such a reply,
        // so they never set their clock from an unsynchronised server.
        const bool unsynced = !synced_;

        NtpPacket resp;
        memset(&resp, 0, sizeof(resp));
        uint8_t vn = ntpVersion(req);
        if (vn < kNtpVersionMin) vn = kNtpVersionFallback;
        resp.liVnMode = ntpMakeLiVnMode(
            unsynced ? kNtpLeapNotSynchronized : kNtpLeapNoWarning, vn, kNtpModeServer);
        resp.stratum = unsynced ? kNtpStratumUnsynchronized : stratum_;
        resp.poll = req.poll;
        resp.precision = kNtpPrecisionUs;
        resp.rootDelay = toBe32(kNtpOneSecond16_16);       // 1 s (16.16)
        resp.rootDispersion = toBe32(kNtpOneSecond16_16);  // 1 s (16.16)
        resp.refId = toBe32(kNtpRefIdLocal);               // "LOCL"

        // Reference = moment of the last successful sync (or now if never).
        struct timeval now;
        gettimeofday(&now, nullptr);
        const uint32_t nowSec = static_cast<uint32_t>(now.tv_sec);
        const uint32_t nowUsec = static_cast<uint32_t>(now.tv_usec);
        uint32_t refNtpSec = 0, refNtpFrac = 0;
        unixToNtp(nowSec, 0, refNtpSec, refNtpFrac);
        resp.refTsSec = toBe32(refNtpSec);
        resp.refTsFrac = toBe32(refNtpFrac);

        // Originate = client's transmit timestamp (copied verbatim).
        resp.origTsSec = req.txTsSec;
        resp.origTsFrac = req.txTsFrac;

        // Receive = Transmit = now.
        uint32_t recvSec = 0, recvFrac = 0;
        unixToNtp(nowSec, nowUsec, recvSec, recvFrac);
        resp.recvTsSec = toBe32(recvSec);
        resp.recvTsFrac = toBe32(recvFrac);
        resp.txTsSec = toBe32(recvSec);
        resp.txTsFrac = toBe32(recvFrac);

        sendto(socketFd_, &resp, sizeof(resp), 0,
               (struct sockaddr*)&from, fromLen);

        char clientIp[kIp4TextLen];
        inet_ntop(AF_INET, &from.sin_addr, clientIp, sizeof(clientIp));
        // The reply's own stratum is what the client saw, and it is the honest
        // number for the log: 16 when the clock is not verified.
        logger_.logRequest(clientIp, resp.stratum, !unsynced);
        if (unsynced) {
            // Kept at debug level for the serial console; the request itself is no
            // longer dropped from the log just because the clock is not set.
            ESP_LOGD(TAG, "NTP request from %s answered unsynchronised (clock not verified)",
                     clientIp);
        }
    }

    if (socketFd_ >= 0) {
        close(socketFd_);
        socketFd_ = -1;
    }
}

} // namespace time
} // namespace dhcp
