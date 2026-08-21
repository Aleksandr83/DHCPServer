#ifndef DHCP_WIFI_IWIFIMANAGER_H
#define DHCP_WIFI_IWIFIMANAGER_H

#include <string>
#include <functional>

namespace dhcp {
namespace wifi {

/**
 * @brief WiFi connection status.
 */
enum class WiFiStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

/**
 * @brief Abstract WiFi manager interface.
 */
class IWiFiManager {
public:
    virtual ~IWiFiManager() = default;

    /**
     * @brief Connect to WiFi as STA with the given credentials.
     * Calls the onConnected callback when done (or onError on failure).
     */
    virtual void connect(const std::string& ssid, const std::string& password) = 0;

    /**
     * @brief Disconnect from WiFi.
     */
    virtual void disconnect() = 0;

    /**
     * @brief Get current connection status.
     */
    virtual WiFiStatus status() const = 0;

    /**
     * @brief Check if WiFi is connected and has an IP.
     */
    virtual bool isConnected() const = 0;

    /**
     * @brief Get the IPv4 address as string (e.g. "192.168.1.201").
     * Returns empty string if not connected.
     */
    virtual std::string ip4() const = 0;

    /**
     * @brief Get the IPv6 address as string.
     * Returns empty string if not connected.
     */
    virtual std::string ip6() const = 0;

    /**
     * @brief Get the current SSID.
     */
    virtual std::string ssid() const = 0;

    // ─── Callbacks ─────────────────────────────────
    using Callback = std::function<void()>;

    virtual void setOnConnected(Callback cb) = 0;
    virtual void setOnDisconnected(Callback cb) = 0;
    virtual void setOnError(Callback cb) = 0;
};

} // namespace wifi
} // namespace dhcp

#endif // DHCP_WIFI_IWIFIMANAGER_H
