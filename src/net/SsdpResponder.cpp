#include "SsdpResponder.h"

#include "SsdpMessages.h"
#include "../core/Version.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cerrno>

using namespace std;

static const char* TAG = "SsdpResponder";

// Rule 39: the task's shape and its one-second heartbeat.
constexpr uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = 4;
constexpr uint32_t kSelectTimeoutSec = 1;
constexpr uint32_t kStopWaitMs = 3000;

namespace dhcp {
namespace net {

SsdpResponder::~SsdpResponder()
{
    stop();
}

string SsdpResponder::deviceUuid()
{
    uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        // Still a well-formed UDN, just not a unique one — and the log says so.
        ESP_LOGW(TAG, "no factory MAC: the UDN will not be unique");
    }
    return ssdp::uuidFromMac(mac);
}

bool SsdpResponder::start(const string& ip)
{
    if (running_.load()) return true;

    ip_ = ip;
    uuid_ = deviceUuid();
    server_ = string("ESP32-P4/1.0 UPnP/1.1 DHCPServer/") +
              ::dhcp::core::Version::instance().toString();

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return false;
    }

    int yes = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ssdp::kPort);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(:%u) failed: errno %d",
                 static_cast<unsigned>(ssdp::kPort), errno);
        close(socket_);
        socket_ = -1;
        return false;
    }

    ip_mreq membership;
    memset(&membership, 0, sizeof(membership));
    inet_pton(AF_INET, ssdp::kMulticastIp, &membership.imr_multiaddr);
    inet_pton(AF_INET, ip.c_str(), &membership.imr_interface);
    if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &membership, sizeof(membership)) != 0) {
        ESP_LOGE(TAG, "IP_ADD_MEMBERSHIP(%s) failed: errno %d",
                 ssdp::kMulticastIp, errno);
        close(socket_);
        socket_ = -1;
        return false;
    }

    // Send from the device's own address, and do not receive our own multicast.
    in_addr outInterface;
    inet_pton(AF_INET, ip.c_str(), &outInterface);
    setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, &outInterface, sizeof(outInterface));
    int loop = 0;
    setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    running_.store(true);
    if (xTaskCreate(&SsdpResponder::taskEntry, "ssdp", kTaskStackBytes, this,
                    kTaskPriority, reinterpret_cast<TaskHandle_t*>(&taskHandle_)) != pdPASS) {
        ESP_LOGE(TAG, "could not create the SSDP task");
        running_.store(false);
        close(socket_);
        socket_ = -1;
        return false;
    }

    ESP_LOGI(TAG, "Advertising %s over SSDP on %s", uuid_.c_str(), ip_.c_str());
    return true;
}

void SsdpResponder::stop()
{
    if (!running_.exchange(false)) return;

    // A device that is leaving says so; otherwise Windows keeps the entry until
    // max-age runs out.
    if (socket_ >= 0) sendNotifies("ssdp:byebye");

    // The task owns the socket: it sees `running_` false, closes the socket and
    // deletes itself. Waiting here keeps the caller from pulling the socket out
    // from under a `recvfrom()`.
    for (uint32_t waited = 0; taskHandle_ != nullptr && waited < kStopWaitMs;
         waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (taskHandle_ != nullptr) {
        ESP_LOGW(TAG, "the SSDP task did not stop in time");
    }
    ESP_LOGI(TAG, "SSDP stopped");
}

void SsdpResponder::taskEntry(void* arg)
{
    static_cast<SsdpResponder*>(arg)->run();
}

void SsdpResponder::run()
{
    sendNotifies("ssdp:alive");
    uint32_t lastNotifyMs =
        xTaskGetTickCount() * static_cast<uint32_t>(portTICK_PERIOD_MS);

    while (running_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket_, &readSet);
        timeval timeout;
        timeout.tv_sec = kSelectTimeoutSec;
        timeout.tv_usec = 0;
        const int ready = select(socket_ + 1, &readSet, nullptr, nullptr, &timeout);

        if (ready > 0 && FD_ISSET(socket_, &readSet)) {
            char buffer[1024];
            sockaddr_in from;
            socklen_t fromLen = sizeof(from);
            memset(&from, 0, sizeof(from));
            const int len = recvfrom(socket_, buffer, sizeof(buffer) - 1, 0,
                                     reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (len > 0) {
                buffer[len] = '\0';
                handleDatagram(buffer, static_cast<size_t>(len), from);
            }
        }

        const uint32_t nowMs =
            xTaskGetTickCount() * static_cast<uint32_t>(portTICK_PERIOD_MS);
        if (nowMs - lastNotifyMs >= ssdp::kNotifyIntervalMs) {
            sendNotifies("ssdp:alive");
            lastNotifyMs = nowMs;
        }
    }

    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    taskHandle_ = nullptr;
    vTaskDelete(nullptr);
}

void SsdpResponder::sendNotifies(const char* nts)
{
    if (socket_ < 0) return;
    const bool alive = (strcmp(nts, "ssdp:alive") == 0);
    const string location = alive ? ssdp::locationFromIp(ip_) : string();

    sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(ssdp::kPort);
    inet_pton(AF_INET, ssdp::kMulticastIp, &destination.sin_addr);

    for (const string& target : ssdp::targets(uuid_)) {
        const string message = ssdp::notifyMessage(
            nts, target, uuid_, location, alive ? server_ : string());
        sendto(socket_, message.data(), message.size(), 0,
               reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    }
}

void SsdpResponder::handleDatagram(const char* data, size_t len,
                                   const sockaddr_in& from)
{
    const string request(data, len);
    if (!ssdp::ssdpIsSearchRequest(request)) return;

    const string searchTarget = ssdp::ssdpSearchTarget(request);
    if (searchTarget.empty()) return;

    const string location = ssdp::locationFromIp(ip_);
    for (const string& target : ssdp::targetsForSearch(searchTarget, uuid_)) {
        const string answer = ssdp::searchResponse(target, uuid_, location, server_);
        sendto(socket_, answer.data(), answer.size(), 0,
               reinterpret_cast<const sockaddr*>(&from), sizeof(from));
    }
    ESP_LOGD(TAG, "answered M-SEARCH for \"%s\"", searchTarget.c_str());
}

} // namespace net
} // namespace dhcp
