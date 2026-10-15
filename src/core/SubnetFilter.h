#ifndef DHCP_CORE_SUBNETFILTER_H
#define DHCP_CORE_SUBNETFILTER_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace core {

/**
 * @brief The "answer only clients of the device's own subnet" filter (stage 200).
 *
 * Three servers narrow who they answer the same way - the built-in DNS, the NTP
 * server and the file manager - and until stage 200 each of them carried its own
 * copy of that decision: read the device address and the netmask from the DHCP
 * settings, fall back to "no filter" when either cannot be parsed, and keep the
 * resulting network for the packet path. The copies had drifted apart in their
 * log wording, and they shared a defect: `Subnet::isValidMask(0)` answers true
 * while `Subnet::contains()` always answers false for a zero mask, so a subnet of
 * `0.0.0.0` switched the filter ON and dropped every client.
 *
 * This class is the single copy of those decisions, and it is deliberately pure:
 * no ESP-IDF, no config reading, no logging. The caller passes the three values it
 * owns - its own switch and the two DHCP settings - and keeps its own log tag,
 * while the wording of the two lines lives here so it cannot drift again.
 *
 * Fail-open is a guarantee and not a convention: whenever the subnet cannot be
 * determined - the switch is off, the address or the mask is missing or
 * unparsable, the mask is not contiguous or is zero, or the address is `0.0.0.0` -
 * @ref active is false **and** @ref contains answers true for every address, so a
 * caller that forgets to ask @ref active still cannot lock the LAN out of its own
 * device. The other danger is the opposite one (an open resolver or an open time
 * service), which is why a subnet that *can* be determined is applied strictly.
 *
 * Host-tested in `test/test_subnetfilter.cpp`.
 */
class SubnetFilter {
public:
    /**
     * @brief What the caller knows and this filter needs.
     *
     * `enabled` is the caller's own switch (it lives in three different configs:
     * `dns_allow_lan`, `time_allow_lan`, `files_allow_lan`); the two strings are
     * the DHCP settings `server_ip` and `subnet`, exactly as the operator typed
     * them (surrounding spaces are tolerated).
     */
    struct Settings {
        bool enabled = false;
        std::string deviceIp;
        std::string netmask;
    };

    /**
     * @brief (Re)read the settings and recompute the subnet.
     *
     * Called whenever the DHCP settings or the caller's switch may have changed.
     * Everything is recomputed, so a later unusable value cannot leave a stale
     * network behind.
     */
    void apply(const Settings& cfg);

    /** @brief True when the filter really narrows the answers right now. */
    bool active() const { return enabled_ && valid_; }

    /**
     * @brief True when @p clientIpHostOrder (host byte order) may be answered.
     *
     * Answers true whenever the filter is not active - see the class comment.
     */
    bool contains(uint32_t clientIpHostOrder) const;

    /** @brief Network address in host byte order (0 while the filter is not active). */
    uint32_t network() const { return network_; }

    /** @brief Netmask in host byte order (0 while the filter is not active). */
    uint32_t mask() const { return mask_; }

    /** @brief The "filter is on" line, e.g. `Own-subnet filter enabled: 192.168.1.0/24`. */
    std::string enabledText() const;

    /** @brief The fail-open line, naming both settings values that could not be used. */
    std::string skippedText() const;

private:
    bool enabled_ = false;   // the caller's switch
    bool valid_ = false;     // the subnet could be determined
    uint32_t network_ = 0;   // host byte order
    uint32_t mask_ = 0;      // host byte order
    std::string deviceIp_;   // kept for the warning text
    std::string netmask_;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SUBNETFILTER_H
