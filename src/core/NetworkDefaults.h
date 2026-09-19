#ifndef DHCP_CORE_NETWORKDEFAULTS_H
#define DHCP_CORE_NETWORKDEFAULTS_H

/**
 * @brief The LAN numbers the whole device has to agree on.
 *
 * The address the device gives itself, the pool it hands out, the gateway and
 * mask it advertises, the one-day lease, and the bounds of the lease table were
 * written out as string and number literals in six files — `Config.h` and
 * `Config.cpp` (the stored defaults), `RestApi.cpp` (the defaults it fills in
 * when a request leaves a field empty, and the bounds it validates against),
 * `EthManager.h` and `WiFiManager` (the static address they fall back to). Six
 * copies of one decision is five chances to change it in the wrong place, which
 * is what rule 39 asks us to remove.
 *
 * The external resolver default happens to carry the same text as the gateway
 * default, and it is a **separate constant on purpose**: the router is where DNS
 * queries go, not what the gateway is, and the two may differ one day.
 *
 * Nothing here is ESP-IDF specific, so a host test may include it.
 */
#include <cstdint>

namespace dhcp {
namespace core {

/// The address the device gives itself when nothing else is configured.
inline constexpr const char* kDefaultServerIp = "192.168.1.201";

/// First address of the pool handed out to clients.
inline constexpr const char* kDefaultPoolStart = "192.168.1.100";

/// Last address of that pool.
inline constexpr const char* kDefaultPoolEnd = "192.168.1.200";

/// Netmask advertised to clients (and used for the "own subnet" checks).
inline constexpr const char* kDefaultSubnetMask = "255.255.255.0";

/// Router advertised to clients — and where the device sends its own traffic.
inline constexpr const char* kDefaultGateway = "192.168.1.1";

/// Upstream resolver used when no other one is configured. Same text as the
/// gateway default today, deliberately a constant of its own.
inline constexpr const char* kDefaultExternalDns = "192.168.1.1";

/// Lease time offered to clients: one day.
inline constexpr uint32_t kDefaultLeaseSec = 86400;

/// Bounds of the lease table. `0` in the settings means "auto" (twice the pool),
/// and whatever comes out of that is clamped into this range — by the server,
/// by the settings loader and by the REST API, which must not disagree.
inline constexpr uint32_t kMinLeaseEntries = 8;
inline constexpr uint32_t kMaxLeaseEntries = 512;

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_NETWORKDEFAULTS_H
