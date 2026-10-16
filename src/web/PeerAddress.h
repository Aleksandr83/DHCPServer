#ifndef DHCP_WEB_PEERADDRESS_H
#define DHCP_WEB_PEERADDRESS_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/IpText.h"
#include "../core/Subnet.h"

// Like RouteTable.h and RedirectPolicy.h, this header is compiled on a host as
// well (see test/test_peeraddress.cpp), so it must not pull in ESP-IDF: what a
// peer address means is decided from its bytes, and RestApi.cpp is the only
// place that reads `getpeername` with it.

namespace dhcp {
namespace web {

/**
 * @brief The peer of a connection in the two forms this device needs it.
 *
 * The LAN filters ask an IPv4 address (@ref ipv4HostOrder, judged by `Subnet`)
 * and the failed-attempt counter of the login asks a name (@ref text, one client
 * being one key). Both come out of the same bytes, so a peer cannot be one
 * address for a filter and another for the lockout that follows it.
 */
struct PeerAddress {
    /** @brief True when four usable IPv4 bytes were read (a mapped IPv6 peer included). */
    bool ipv4 = false;
    /** @brief The IPv4 address in host byte order; meaningful only when @ref ipv4. */
    uint32_t ipv4HostOrder = 0;
    /**
     * @brief How the address is named in a log and used as a lockout key.
     *
     * Never empty, and never a **plausible** address when there was nothing to
     * read: the operator read `failed login from 0.0.0.0` in `Auth.log` and took
     * it for the client's address, which is what this word prevents.
     */
    std::string text = "unknown";
};

namespace detail {

/** @brief True for `::ffff:a.b.c.d` — an IPv4 address in an IPv6 coat. */
inline bool isV4Mapped(const uint8_t* bytes)
{
    for (int i = 0; i < 10; ++i) {
        if (bytes[i] != 0) return false;
    }
    return bytes[10] == 0xFF && bytes[11] == 0xFF;
}

} // namespace detail

/**
 * @brief Decides what a peer address means from its raw bytes (stage 207).
 *
 * Four bytes are an IPv4 address. Sixteen are an IPv6 address — **except** the
 * IPv4-mapped form `::ffff:a.b.c.d`, which is how an IPv4 client appears on the
 * socket this device's HTTP server listens on: `CONFIG_LWIP_IPV6` makes
 * `esp_http_server` create a `PF_INET6` listener and the stack then reports an
 * IPv4 peer in that form. It is read as the IPv4 address it stands for, because
 * that is the address the client really has and the only one the LAN filters can
 * judge.
 *
 * A real IPv6 peer is named as an IPv6 address and leaves @ref PeerAddress::ipv4
 * false: it is in no IPv4 subnet, so the filters keep their "cannot decide"
 * answer, while the login counter — the one thing that has to differ per client —
 * keys on its own address instead of sharing a key with every other client, which
 * is what a device whose HTTP server only understood `AF_INET` would do.
 *
 * Anything else is @c "unknown": a length that is neither 4 nor 16, and `0.0.0.0`
 * (`::ffff:0.0.0.0` as well), which no established connection can come from.
 *
 * @param[in] bytes  Address bytes as the socket reported them.
 * @param[in] length Number of bytes in @p bytes.
 */
inline PeerAddress peerAddressFromBytes(const uint8_t* bytes, size_t length)
{
    PeerAddress peer;
    if (bytes == nullptr) return peer;

    if (length == 4) {
        const uint32_t addr = core::ip4HostOrder(bytes);
        if (addr == 0) return peer;
        peer.ipv4 = true;
        peer.ipv4HostOrder = addr;
        peer.text = core::ip4Text(bytes);
        return peer;
    }

    if (length == 16) {
        if (detail::isV4Mapped(bytes)) {
            const uint32_t addr = core::ip4HostOrder(bytes + 12);
            if (addr == 0) return peer;
            peer.ipv4 = true;
            peer.ipv4HostOrder = addr;
            peer.text = core::ip4Text(bytes + 12);
        } else {
            peer.text = core::ip6Text(bytes);
        }
    }

    return peer;
}

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_PEERADDRESS_H
