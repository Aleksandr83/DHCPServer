#ifndef DHCP_CORE_DEVICEHOSTNAME_H
#define DHCP_CORE_DEVICEHOSTNAME_H

#include <cstddef>
#include <string>

namespace dhcp {
namespace core {

/**
 * @brief Rules of the device's own host name (stage 218).
 *
 * The name is typed on Settings ▸ General, stored in NVS and handed to the
 * network interface (`esp_netif_set_hostname`). It is an RFC 1123 label: letters,
 * digits and hyphens, no hyphen at either end, at most 63 characters. The
 * interface accepts whatever string it is given, so the check has to live here —
 * and it is pure, which is what makes it host-testable.
 */
constexpr std::size_t kDeviceHostNameMaxLen = 63;

/// The name a device that was never asked carries; also the value the page
/// offers when the field is empty and the value a stored invalid name falls back
/// to. Capitalised the way the product is written on its own page.
constexpr const char* kDefaultDeviceHostName = "DhcpServer";

/**
 * @brief Strip the spaces around the value.
 *
 * The operator's spaces belong to the form field, not to the name: a trailing
 * newline copied into the field must not turn a valid name into a refused one.
 * Spaces *inside* the value are not touched — an inner space makes the name
 * invalid rather than being silently removed.
 */
inline std::string trimmedDeviceHostName(const std::string& name)
{
    std::size_t begin = 0;
    std::size_t end = name.size();
    while (begin < end && (name[begin] == ' ' || name[begin] == '\t' ||
                           name[begin] == '\r' || name[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (name[end - 1] == ' ' || name[end - 1] == '\t' ||
                           name[end - 1] == '\r' || name[end - 1] == '\n')) {
        --end;
    }
    return name.substr(begin, end - begin);
}

/// @brief Is @p name one RFC 1123 label the interface can carry?
inline bool isValidDeviceHostName(const std::string& name)
{
    if (name.empty() || name.size() > kDeviceHostNameMaxLen) return false;
    if (name.front() == '-' || name.back() == '-') return false;
    for (const char c : name) {
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = (c >= '0' && c <= '9');
        if (!letter && !digit && c != '-') return false;
    }
    return true;
}

/// @brief The stored name while it still passes, the factory name otherwise.
inline std::string deviceHostNameOrDefault(const std::string& stored)
{
    return isValidDeviceHostName(stored) ? stored : std::string(kDefaultDeviceHostName);
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_DEVICEHOSTNAME_H
