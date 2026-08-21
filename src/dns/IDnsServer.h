#ifndef DHCP_DNS_IDNSSERVER_H
#define DHCP_DNS_IDNSSERVER_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace dns {

/**
 * @brief DNS server state.
 */
enum class DnsServerState {
    STOPPED,
    RUNNING,
    ERROR
};

/**
 * @brief Abstract DNS server interface.
 *
 * Acts as a DNS proxy:
 *   1. Log request
 *   2. Search local hosts
 *   3. Search external cache (REST stub)
 *   4. Forward to external DNS
 */
class IDnsServer {
public:
    virtual ~IDnsServer() = default;

    /**
     * @brief Start the DNS server.
     * @return true on success.
     */
    virtual bool start() = 0;

    /**
     * @brief Stop the DNS server.
     */
    virtual void stop() = 0;

    /**
     * @brief Get current state.
     */
    virtual DnsServerState state() const = 0;

    /**
     * @brief Check if running.
     */
    virtual bool isRunning() const = 0;

    /**
     * @brief Get total queries handled.
     */
    virtual uint32_t queryCount() const = 0;

    /**
     * @brief Get state as string.
     */
    virtual std::string stateString() const = 0;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_IDNSSERVER_H
