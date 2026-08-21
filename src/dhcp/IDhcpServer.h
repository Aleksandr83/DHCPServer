#ifndef DHCP_DHCP_IDHCPSERVER_H
#define DHCP_DHCP_IDHCPSERVER_H

#include <string>
#include <cstdint>

namespace dhcp {
namespace dhcp {

/**
 * @brief DHCP server state.
 */
enum class DhcpServerState {
    STOPPED,
    RUNNING,
    ERROR
};

/**
 * @brief Abstract DHCP server interface.
 */
class IDhcpServer {
public:
    virtual ~IDhcpServer() = default;

    /**
     * @brief Start the DHCP server.
     * @return true if started successfully.
     */
    virtual bool start() = 0;

    /**
     * @brief Stop the DHCP server.
     */
    virtual void stop() = 0;

    /**
     * @brief Get current state.
     */
    virtual DhcpServerState state() const = 0;

    /**
     * @brief Check if server is running.
     */
    virtual bool isRunning() const = 0;

    /**
     * @brief Get number of active leases.
     */
    virtual uint32_t leaseCount() const = 0;

    /**
     * @brief Look up the MAC address for an IP in the active lease table.
     *
     * Used by the DNS server to resolve a client IP → MAC (DHCP fallback
     * when the ARP cache has no entry).
     *
     * @param ip   IPv4 address in network byte order.
     * @param mac  Output: 6-byte MAC address.
     * @return true if a lease for this IP exists.
     */
    virtual bool getMacByIp(uint32_t ip, uint8_t mac[6]) const = 0;

    /**
     * @brief Enable/disable terminal logging at runtime.
     */
    virtual void setLogTerminal(bool enabled) = 0;

    /**
     * @brief Configure external REST logging of DHCP events.
     *
     * @param enabled      Master switch (send events to the REST URL).
     * @param url          External REST logging URL.
     * @param authEnabled  Send HTTP Basic auth.
     * @param user         Basic auth username.
     * @param password     Basic auth password.
     */
    virtual void setRestLogging(bool enabled, const std::string& url,
                                bool authEnabled, const std::string& user,
                                const std::string& password) = 0;

    /**
     * @brief Tell the DHCP server whether the built-in DNS server is running.
     * Used to decide which DNS to advertise in "auto" mode.
     */
    virtual void setDnsServerRunning(bool running) = 0;

    /**
     * @brief Reload static bindings from NVS at runtime.
     * Called after the bindings are saved via the web UI so changes
     * (including per-host gateway / use_gateway) apply without a reboot.
     */
    virtual void reloadStaticBindings() = 0;

    /**
     * @brief Get state as string.
     */
    virtual std::string stateString() const = 0;
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_IDHCPSERVER_H
