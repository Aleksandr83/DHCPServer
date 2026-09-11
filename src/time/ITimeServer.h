#ifndef DHCP_TIME_ITIMESERVER_H
#define DHCP_TIME_ITIMESERVER_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace time {

/**
 * @brief Time (NTP) server state.
 */
enum class TimeServerState {
    STOPPED,
    RUNNING,
    ERROR
};

/**
 * @brief Abstract NTP server interface.
 *
 * The service:
 *   1. Synchronises the device clock from an external NTP server (SNTP client)
 *   2. Serves UTC time to LAN clients over NTP (UDP port 123)
 *
 * Clients always receive UTC; the configured timezone offset is used only for
 * local display on the device itself.
 */
class ITimeServer {
public:
    virtual ~ITimeServer() = default;

    /** @brief Start the NTP server (and the SNTP client). @return true on success. */
    virtual bool start() = 0;

    /** @brief Stop the NTP server (and the SNTP client). */
    virtual void stop() = 0;

    /** @brief Get the current state. */
    virtual TimeServerState state() const = 0;

    /** @brief Check whether the server task is running. */
    virtual bool isRunning() const = 0;

    /** @brief Check whether the clock has been synchronised at least once. */
    virtual bool isSynced() const = 0;

    /** @brief Get the state as a human-readable string. */
    virtual std::string stateString() const = 0;
};

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_ITIMESERVER_H
