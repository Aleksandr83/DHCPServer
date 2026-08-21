#ifndef DHCP_ETH_IETHMANAGER_H
#define DHCP_ETH_IETHMANAGER_H

#include <string>
#include <functional>

namespace dhcp {
namespace eth {

/**
 * @brief Ethernet connection status.
 */
enum class EthStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

/**
 * @brief Abstract Ethernet manager interface for ENC28J60.
 *
 * Initializes SPI + esp_eth, sets static IP, and provides
 * connection status callbacks.
 */
class IEthManager {
public:
    virtual ~IEthManager() = default;

    /**
     * @brief Initialize SPI bus and ENC28J60 Ethernet.
     * Must be called once before any other operations.
     */
    virtual void init() = 0;

    /**
     * @brief Get current Ethernet link status.
     */
    virtual EthStatus status() const = 0;

    /**
     * @brief Check if Ethernet link is up and IP is configured.
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
     * @brief Reconfigure static IP addresses (applied on next init/connect).
     */
    virtual void setStaticIp(const std::string& ip4, const std::string& gateway,
                             const std::string& netmask) = 0;

    // ─── Callbacks ─────────────────────────────────
    using Callback = std::function<void()>;

    virtual void setOnConnected(Callback cb) = 0;
    virtual void setOnDisconnected(Callback cb) = 0;
    virtual void setOnError(Callback cb) = 0;
};

} // namespace eth
} // namespace dhcp

#endif // DHCP_ETH_IETHMANAGER_H
