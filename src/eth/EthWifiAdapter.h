#ifndef DHCP_ETH_ETHWIFIADAPTER_H
#define DHCP_ETH_ETHWIFIADAPTER_H

#include "../wifi/IWiFiManager.h"
#include "../eth/IEthManager.h"

namespace dhcp {
namespace eth {

/**
 * @brief Adapter that makes IEthManager look like IWiFiManager.
 *
 * Allows WebServer, TerminalMenu, and RestApi to work with Ethernet
 * without changing their interfaces. WiFi commands are no-ops.
 */
class EthWifiAdapter : public ::dhcp::wifi::IWiFiManager {
public:
    explicit EthWifiAdapter(IEthManager& eth)
        : eth_(eth) {}

    // IWiFiManager interface — delegate to EthManager where applicable
    void connect(const std::string& /*ssid*/, const std::string& /*password*/) override {
        // WiFi connect is not applicable for Ethernet — no-op
    }

    void disconnect() override {
        // No Ethernet disconnect
    }

    ::dhcp::wifi::WiFiStatus status() const override {
        switch (eth_.status()) {
            case EthStatus::DISCONNECTED: return ::dhcp::wifi::WiFiStatus::DISCONNECTED;
            case EthStatus::CONNECTING:   return ::dhcp::wifi::WiFiStatus::CONNECTING;
            case EthStatus::CONNECTED:    return ::dhcp::wifi::WiFiStatus::CONNECTED;
            case EthStatus::ERROR:        return ::dhcp::wifi::WiFiStatus::ERROR;
            default:                      return ::dhcp::wifi::WiFiStatus::DISCONNECTED;
        }
    }

    bool isConnected() const override { return eth_.isConnected(); }
    std::string ip4() const override { return eth_.ip4(); }
    std::string ip6() const override { return eth_.ip6(); }
    std::string ssid() const override { return "ENC28J60"; }

    void setOnConnected(Callback cb) override { eth_.setOnConnected(std::move(cb)); }
    void setOnDisconnected(Callback cb) override { eth_.setOnDisconnected(std::move(cb)); }
    void setOnError(Callback cb) override { eth_.setOnError(std::move(cb)); }

private:
    IEthManager& eth_;
};

} // namespace eth
} // namespace dhcp

#endif // DHCP_ETH_ETHWIFIADAPTER_H
