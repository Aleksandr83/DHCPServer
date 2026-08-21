#ifndef DHCP_WIFI_WIFIMANAGER_H
#define DHCP_WIFI_WIFIMANAGER_H

#include "IWiFiManager.h"
#include <string>
#include <functional>
#include "esp_event.h"
#include "esp_netif.h"

namespace dhcp {
namespace wifi {

/**
 * @brief ESP-IDF WiFi STA implementation of IWiFiManager.
 *
 * Uses static IP configuration:
 *   IPv4: 192.168.1.201
 *   IPv6: fd12:3456:789a:0001:021b:21ff:fe6b:8c4d
 *
 * On connect success, calls the onConnected callback.
 * On disconnect, calls onDisconnected.
 */
class WiFiManager : public IWiFiManager {
public:
    /**
     * @param ip4        Static IPv4 address (default "192.168.1.201")
     * @param gateway    Gateway address (default "192.168.1.1")
     * @param netmask    Subnet mask (default "255.255.255.0")
     * @param ip6        Static IPv6 global/ULA address (default "fd12:3456:789a:0001:021b:21ff:fe6b:8c4d")
     */
    WiFiManager(const std::string& ip4 = "192.168.1.201",
                const std::string& gateway = "192.168.1.1",
                const std::string& netmask = "255.255.255.0",
                const std::string& ip6 = "fd12:3456:789a:0001:021b:21ff:fe6b:8c4d");

    ~WiFiManager() override;

    // IWiFiManager interface
    void connect(const std::string& ssid, const std::string& password) override;
    void disconnect() override;
    WiFiStatus status() const override { return status_; }
    bool isConnected() const override { return status_ == WiFiStatus::CONNECTED; }
    std::string ip4() const override { return ip4_; }
    std::string ip6() const override { return ip6_; }
    std::string ssid() const override { return currentSsid_; }

    void setOnConnected(Callback cb) override { onConnected_ = std::move(cb); }
    void setOnDisconnected(Callback cb) override { onDisconnected_ = std::move(cb); }
    void setOnError(Callback cb) override { onError_ = std::move(cb); }

    /**
     * @brief Set/reconfigure static IPv4 address (applied on next init/connect).
     */
    void setStaticIp(const std::string& ip4, const std::string& gateway,
                     const std::string& netmask);

    /**
     * @brief Initialize the WiFi driver and netif.
     * Must be called once before any connect/disconnect.
     */
    void init();

    /**
     * @brief De-initialize and release resources.
     */
    void deinit();

private:
    // Internal event handler (registered with esp_event_loop)
    static void eventHandler(void* arg, esp_event_base_t eventBase,
                             int32_t eventId, void* eventData);

    void handleEvent(esp_event_base_t base, int32_t id, void* data);

    std::string staticIp4_;
    std::string gateway_;
    std::string netmask_;
    std::string staticIp6_;
    std::string currentSsid_;
    std::string ip4_;
    std::string ip6_;

    WiFiStatus status_ = WiFiStatus::DISCONNECTED;

    Callback onConnected_;
    Callback onDisconnected_;
    Callback onError_;

    bool initialized_ = false;
    bool dhcpRetry_ = false;
};

} // namespace wifi
} // namespace dhcp

#endif // DHCP_WIFI_WIFIMANAGER_H
