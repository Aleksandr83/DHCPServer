#ifndef DHCP_CORE_SUBNET_H
#define DHCP_CORE_SUBNET_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace core {

/**
 * @brief IPv4 subnet arithmetic for the LAN access filters (DNS / NTP).
 *
 * "Own subnet" is the device address plus the netmask taken from the DHCP
 * settings (`server_ip` / `subnet`) — no extra configuration field. Addresses
 * are **host byte order** `uint32_t`: `parseIp4()`, `toString()` and
 * `maskFromPrefix()` all work on that form, and socket addresses must be
 * converted with `ntohl()` before being passed in. Comparison itself
 * (`contains()`) is byte-order agnostic — masking works the same either way —
 * but keeping every value in one order avoids surprises.
 *
 * Deliberately free of ESP-IDF dependencies: the class is unit-tested on the
 * host (see `test/test_subnet.cpp`), like `time::TimeMath`.
 */
class Subnet {
public:
    /**
     * @brief Parse a dotted-quad IPv4 address.
     *
     * Surrounding spaces are tolerated (the DHCP settings are free-form text),
     * everything else is strict: exactly four decimal parts, 1–3 digits each,
     * no leading `+`/`-`, each part ≤ 255.
     *
     * @return true on success; @p out is untouched on failure.
     */
    static bool parseIp4(const std::string& text, uint32_t& out);

    /** @brief True when the set bits of @p mask are contiguous from the MSB. */
    static bool isValidMask(uint32_t mask);

    /** @brief Network address of @p addr under @p mask. */
    static uint32_t network(uint32_t addr, uint32_t mask);

    /**
     * @brief True when @p addr belongs to the network @p net / @p mask.
     *
     * A zero mask (no subnet configured) or a non-contiguous mask always
     * returns false — callers treat that as "the filter cannot decide" and skip
     * filtering with a warning instead of blocking every client. Use
     * @ref isValidMask to tell the two cases apart.
     */
    static bool contains(uint32_t net, uint32_t mask, uint32_t addr);

    /** @brief Number of host bits implied by @p mask (0 for `0.0.0.0`). */
    static int hostBits(uint32_t mask);

    /** @brief Build a netmask from a prefix length (0–32); 0 when out of range. */
    static uint32_t maskFromPrefix(int prefixLen);

    /** @brief Prefix length of @p mask (`0`–`32`), or -1 when not contiguous. */
    static int prefixLength(uint32_t mask);

    /** @brief Dotted-quad text of @p addr (for logging). */
    static std::string toString(uint32_t addr);
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SUBNET_H
