#ifndef DHCP_NET_SSDPMESSAGES_H
#define DHCP_NET_SSDPMESSAGES_H

#include <cstdint>
#include <string>
#include <vector>

namespace dhcp {
namespace net {
namespace ssdp {

/**
 * @brief The SSDP/UPnP messages the device sends (stage 220).
 *
 * SSDP is a text protocol, and a Windows machine sees exactly these bytes. The
 * socket and the HTTP route need ESP-IDF; the messages, the search parsing, the
 * UUID derivation and the device description are pure, which is what lets a host
 * test keep the `ST`/`USN`/`LOCATION` lines honest.
 */

/// The SSDP multicast endpoint (IPv4).
constexpr uint16_t kPort = 1900;
constexpr const char* kMulticastIp = "239.255.255.250";

/// The one UPnP device type this device claims: a basic device, not a computer
/// (UPnP has no "computer" type; that word belongs to WS-Discovery).
constexpr const char* kDeviceType = "urn:schemas-upnp-org:device:Basic:1";

/// Every device also advertises the root device and its own UUID.
constexpr const char* kRootDevice = "upnp:rootdevice";

/// `CACHE-CONTROL: max-age`, seconds — the UPnP default.
constexpr uint32_t kMaxAgeSec = 1800;

/// Re-advertise before the cache expires: every 15 minutes (three times per
/// max-age), as the UPnP device architecture asks.
constexpr uint32_t kNotifyIntervalMs = 900000;

/// Where the description lives, relative to the device root.
constexpr const char* kDescriptionPath = "/upnp/device.xml";

/// @brief `uuid:` UDN derived from the MAC. Stable across reboots without
/// keeping anything in NVS; different for another device.
std::string uuidFromMac(const uint8_t mac[6]);

/// @brief `http://<ip>:80/upnp/device.xml` — the `LOCATION` every message names.
std::string locationFromIp(const std::string& ip);

/// @brief The three advertisements of one device.
std::vector<std::string> targets(const std::string& uuid);

/**
 * @brief Which targets an M-SEARCH asks for.
 *
 * `ssdp:all` asks for all three; any other value is answered only when it is one
 * of ours (an unknown target is not ours to answer).
 */
std::vector<std::string> targetsForSearch(const std::string& searchTarget,
                                          const std::string& uuid);

/// @brief The unicast answer to an M-SEARCH.
std::string searchResponse(const std::string& st, const std::string& uuid,
                           const std::string& location, const std::string& server);

/**
 * @brief A multicast NOTIFY.
 *
 * @param nts      `ssdp:alive` or `ssdp:byebye`.
 * @param nt       The target this notification is about.
 * @param uuid     The device's UDN (`uuid:` included).
 * @param location `LOCATION`; empty for `byebye` (it has nothing to point at).
 * @param server   `SERVER`; empty for `byebye`.
 */
std::string notifyMessage(const std::string& nts, const std::string& nt,
                          const std::string& uuid, const std::string& location,
                          const std::string& server);

/// @brief Is this datagram an `M-SEARCH * HTTP/1.1` request (case-insensitive)?
bool ssdpIsSearchRequest(const std::string& datagram);

/// @brief The `ST:` header of a search request, or `""` when it has none.
std::string ssdpSearchTarget(const std::string& datagram);

/// @brief The UPnP device description Windows fetches from `LOCATION`.
std::string deviceXml(const std::string& friendlyName, const std::string& modelName,
                      const std::string& modelNumber, const std::string& uuid,
                      const std::string& presentationUrl);

} // namespace ssdp
} // namespace net
} // namespace dhcp

#endif // DHCP_NET_SSDPMESSAGES_H
