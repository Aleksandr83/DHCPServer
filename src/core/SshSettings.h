#ifndef DHCP_CORE_SSHSETTINGS_H
#define DHCP_CORE_SSHSETTINGS_H

#include <cstdint>

namespace dhcp {
namespace core {

/**
 * @brief The bounds of the SSH server's port (stage 222).
 *
 * The port comes from three places that can each be wrong — the page, NVS, and
 * an imported settings file — so the rule is one function rather than three
 * copies of `>= 1 && <= 65535`. A port outside the range is not "corrected":
 * the device keeps the port it has, the way every other numeric setting here
 * is handled.
 */
constexpr uint16_t kSshDefaultPort = 22;
constexpr int kSshMinPort = 1;
constexpr int kSshMaxPort = 65535;

/// @brief Is @p port a port a listener can be bound to?
inline bool sshPortValid(int port)
{
    return port >= kSshMinPort && port <= kSshMaxPort;
}

/// @brief @p port when it is valid, the default otherwise.
inline uint16_t sshPortOrDefault(int port)
{
    return sshPortValid(port) ? static_cast<uint16_t>(port) : kSshDefaultPort;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SSHSETTINGS_H
