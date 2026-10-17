#ifndef DHCP_NET_MDNSRESPONDER_H
#define DHCP_NET_MDNSRESPONDER_H

#include <string>

namespace dhcp {
namespace net {

/**
 * @brief Publishes the device's host name over mDNS (stage 219).
 *
 * A thin wrapper around the `espressif/mdns` component: it owns the
 * init/free lifecycle and keeps the "is it running" answer, so the REST layer
 * does not have to know the component's state machine. The component is part
 * of the ESP32-P4 build only (see `src/CMakeLists.txt`); on any other target
 * @ref start reports that publishing is unavailable and the caller keeps the
 * setting as a wish.
 */
class MdnsResponder {
public:
    MdnsResponder() = default;
    ~MdnsResponder();
    MdnsResponder(const MdnsResponder&) = delete;
    MdnsResponder& operator=(const MdnsResponder&) = delete;

    /// @brief Is this build able to publish a name at all?
    static bool available();

    /**
     * @brief Start publishing @p hostName as `<hostname>.local`.
     *
     * Safe to call while already running: the name is re-applied. Returns
     * false when the component refuses to start (memory, an invalid name) —
     * the caller then reports the failure instead of pretending.
     */
    bool start(const std::string& hostName);

    /// @brief Stop publishing and release the component.
    void stop();

    /// @brief Rename a running responder; a no-op while stopped.
    bool setHostName(const std::string& hostName);

    bool isRunning() const { return running_; }

private:
    bool running_ = false;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_MDNSRESPONDER_H
