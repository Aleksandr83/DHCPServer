#include "TimeServer.h"
#include "NtpMessage.h"
#include "../core/Config.h"
#include "../core/Subnet.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "sys/time.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

using namespace std;

namespace {

// Rule 39: the field bases of struct tm — the year counts from 1900, the month
// from 0 — and the scale that turns a whole-hour offset into seconds.
constexpr int kTmYearBase = 1900;
constexpr int kTmMonthBase = 1;
constexpr int64_t kSecondsPerHour = 3600;

// Rule 39: the NTP task formats replies and touches no large buffer.
constexpr uint32_t kServerTaskStackBytes = 4096;

// Rule 39: "255.255.255.255" plus the NUL, the room a client address needs.
constexpr size_t kIp4TextLen = 16;

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
             kPort, serverName_.c_str());
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
    esp_sntp_setservername(0, serverName_.c_str());
    esp_sntp_set_time_sync_notification_cb(&TimeServer::onSyncNotification);
    esp_sntp_set_sync_interval(syncIntervalSec_ * 1000u);
    esp_sntp_init();
    sntpStarted_ = true;
    ESP_LOGI(TAG, "SNTP client started (server=%s, interval=%us)",
             serverName_.c_str(), (unsigned)syncIntervalSec_);
}

void TimeServer::stopSync()
{
    if (sntpStarted_) {
        esp_sntp_stop();
        sntpStarted_ = false;
        ESP_LOGI(TAG, "SNTP client stopped");
    }
}

void TimeServer::restartSync()
{
    if (sntpStarted_) {
        esp_sntp_stop();
        sntpStarted_ = false;
    }
    startSync();
    ESP_LOGI(TAG, "SNTP restarted (server=%s, interval=%us)",
             serverName_.c_str(), (unsigned)syncIntervalSec_);
}

// ─────────────────────────────────────────────────────
// Access control
// ─────────────────────────────────────────────────────

void TimeServer::applyAccessFilter()
{
    const auto timeCfg = core::Config::instance().getTime();
    allowOwnSubnet_ = timeCfg.allowOwnSubnet;
    rateLimitPerSec_ = timeCfg.rateLimitPerSec;

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
                ESP_LOGW(TAG, "NTP request from %s dropped (%s; totals: %u subnet, %u rate)",
                         clientIpStr, dropReason,
                         static_cast<unsigned>(foreignDropped_),
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
        if (unsynced) {
            // Not counted/logged as a served request — the time was not
            // trustworthy (the client is expected to ignore the reply).
            ESP_LOGD(TAG, "NTP request from %s ignored (clock not synchronised)",
                     clientIp);
        } else {
            logger_.logRequest(clientIp, stratum_);
        }
    }

    if (socketFd_ >= 0) {
        close(socketFd_);
        socketFd_ = -1;
    }
}

} // namespace time
} // namespace dhcp
