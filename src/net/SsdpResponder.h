#ifndef DHCP_NET_SSDPRESPONDER_H
#define DHCP_NET_SSDPRESPONDER_H

#include <atomic>
#include <string>

struct sockaddr_in;

namespace dhcp {
namespace net {

/**
 * @brief Makes the device discoverable over SSDP/UPnP (stage 220).
 *
 * One UDP socket on the SSDP multicast group answers `M-SEARCH` requests and
 * advertises the device with `ssdp:alive` every 15 minutes; `stop()` says
 * `ssdp:byebye`. Windows fetches the UPnP description from the `LOCATION` URL,
 * which the web server serves at `/upnp/device.xml`.
 *
 * The switch that owns it is the same "publish the name in the local network"
 * as the DNS and mDNS halves; the device type is a basic UPnP device, because
 * UPnP has no "computer" type — that word belongs to WS-Discovery.
 */
class SsdpResponder {
public:
    SsdpResponder() = default;
    ~SsdpResponder();
    SsdpResponder(const SsdpResponder&) = delete;
    SsdpResponder& operator=(const SsdpResponder&) = delete;

    /**
     * @brief Start answering and advertising.
     *
     * @param ip The device's IPv4 address, used for the `LOCATION` URL and as
     *           the outgoing multicast interface.
     */
    bool start(const std::string& ip);

    /// @brief Say goodbye, and close the socket and the task.
    void stop();

    bool isRunning() const { return running_.load(); }

    /// @brief The UDN this device uses, derived from its factory MAC. Stable
    /// across reboots; the same value the description carries.
    static std::string deviceUuid();

private:
    static void taskEntry(void* arg);
    void run();
    void sendNotifies(const char* nts);
    void handleDatagram(const char* data, size_t len, const sockaddr_in& from);

    int socket_ = -1;
    std::string ip_;
    std::string uuid_;
    std::string server_;
    std::atomic<bool> running_{false};
    /// FreeRTOS task handle, kept as `void*` so this header stays IDF-free.
    void* taskHandle_ = nullptr;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_SSDPRESPONDER_H
