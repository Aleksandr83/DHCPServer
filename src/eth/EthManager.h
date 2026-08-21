#ifndef DHCP_ETH_ETHMANAGER_H
#define DHCP_ETH_ETHMANAGER_H

#include "IEthManager.h"
#include <string>
#include <functional>

#include "esp_event.h"
#include "esp_eth.h"

namespace dhcp {
namespace eth {

/**
 * @brief ENC28J60 Ethernet manager implementation.
 *
 * Uses ESP-IDF esp_eth component with ENC28J60 over SPI.
 * Static IP is configured on init (DHCP client not used for the Ethernet interface).
 *
 * Hardware wiring (see Docs/ENC28J60.md):
 *   SCK  → D18  (GPIO18)
 *   MOSI → D23  (GPIO23)
 *   MISO → D19  (GPIO19)
 *   CS   → D5   (GPIO5)
 *   INT  → D4   (GPIO4)
 *   RST  → D16  (GPIO16)
 */
class EthManager : public IEthManager {
public:
    EthManager(const std::string& ip4 = "192.168.1.201",
               const std::string& gateway = "192.168.1.1",
               const std::string& netmask = "255.255.255.0",
               const std::string& ip6 = "fd12:3456:789a:0001:021b:21ff:fe6b:8c4d");
    ~EthManager() override;

    // IEthManager interface
    void init() override;
    EthStatus status() const override { return status_; }
    bool isConnected() const override { return status_ == EthStatus::CONNECTED; }
    std::string ip4() const override { return ip4_; }
    std::string ip6() const override { return ip6_; }
    void setStaticIp(const std::string& ip4, const std::string& gateway,
                     const std::string& netmask) override;

    void setOnConnected(Callback cb) override { onConnected_ = std::move(cb); }
    void setOnDisconnected(Callback cb) override { onDisconnected_ = std::move(cb); }
    void setOnError(Callback cb) override { onError_ = std::move(cb); }

private:
    // Event handler (registered with esp_event_loop)
    static void eventHandler(void* arg, esp_event_base_t eventBase,
                             int32_t eventId, void* eventData);
    void handleEvent(esp_event_base_t base, int32_t id, void* data);

    std::string staticIp4_;
    std::string gateway_;
    std::string netmask_;
    std::string staticIp6_;
    std::string ip4_;
    std::string ip6_;

    EthStatus status_ = EthStatus::DISCONNECTED;

    Callback onConnected_;
    Callback onDisconnected_;
    Callback onError_;

    esp_eth_handle_t ethHandle_ = nullptr;
    bool initialized_ = false;
};

} // namespace eth
} // namespace dhcp

#endif // DHCP_ETH_ETHMANAGER_H
