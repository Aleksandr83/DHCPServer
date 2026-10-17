#ifndef DHCP_CORE_LOCALNAME_H
#define DHCP_CORE_LOCALNAME_H

#include <string>

namespace dhcp {
namespace core {

/// mDNS reserves the `.local` domain (RFC 6762, RFC 6761). It is the only
/// suffix the device publishes itself under, so the rule below is written
/// against this one constant rather than against a literal in two files.
constexpr const char* kLocalNameSuffix = ".local";

/**
 * @brief Does @p query ask for the device's own name?
 *
 * The device answers two names about itself: the bare host name — through its
 * own DNS server — and `<hostname>.local`, through that server and through
 * mDNS. DNS is case-insensitive and a question may carry the root dot, so both
 * are folded here. An empty host name matches nothing (a device that was never
 * given one must not claim the root).
 *
 * The decision is pure, which is what makes it host-testable: the DNS task it
 * serves cannot be compiled without ESP-IDF.
 */
inline bool localNameMatches(const std::string& query, const std::string& hostName)
{
    if (hostName.empty()) return false;

    auto normalized = [](std::string text) {
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        while (!text.empty() && text.back() == '.') text.pop_back();
        return text;
    };

    const std::string asked = normalized(query);
    if (asked.empty()) return false;
    const std::string own = normalized(hostName);
    return asked == own || asked == own + kLocalNameSuffix;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_LOCALNAME_H
