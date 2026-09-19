#ifndef DHCP_DHCP_DHCPCLIENTNAME_H
#define DHCP_DHCP_DHCPCLIENTNAME_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace dhcp {
namespace dhcp {

/**
 * @brief The name a DHCP client reports about itself.
 *
 * Two options carry it, and neither is mandatory — a device that sends nothing
 * is normal (phones usually do, Windows and macOS usually do not skip it):
 *
 *  - **option 12** (host name, RFC 2132): the short computer name, exactly what
 *    the router's client list shows.
 *  - **option 81** (client FQDN, RFC 4702): `flags`, `rcode1`, `rcode2` and a
 *    domain name that is either DNS-encoded (the E flag, bit 0x04) or plain
 *    text. A client MAY leave the host part out and send only the domain, which
 *    is why option 81 is the fallback and not the first choice.
 *
 * What arrives here is **untrusted input**: it ends up in a JSON answer, in an
 * input field and in NVS. So the text is filtered (see sanitize) rather than
 * trusted — a name with a quote or an invalid byte in it would break the page
 * that shows it, and one with `|` or a newline would break the allow-list blob
 * it may be copied into.
 */
class DhcpClientName {
public:
    /**
     * @brief Longest name kept, in BYTES (a UTF-8 sequence is never cut in half).
     *
     * A byte budget rather than a character count on purpose: this text is
     * copied into the allow-list blob, whose own limit is 20 bytes per name, so
     * the limit that matters is the one the storage has.
     */
    static constexpr size_t kMaxLen = 32;

    /**
     * @brief The best name the packet offers.
     *
     * Option 12 (already a host name) wins; otherwise the short label of the
     * option 81 FQDN; empty when the client sent neither.
     */
    static std::string fromOptions(const uint8_t* options, size_t len);

    /** @brief Option 12 host name, sanitized; empty when absent or unusable. */
    static std::string hostNameOption(const uint8_t* options, size_t len);

    /**
     * @brief Option 81 FQDN, sanitized, trailing dot removed.
     *        Empty when absent, malformed or unusable.
     */
    static std::string fqdnOption(const uint8_t* options, size_t len);

    /** @brief First label of a dotted name ("pc1.lan" -> "pc1"). */
    static std::string shortLabel(const std::string& name);

    /**
     * @brief Make a name safe to store, print and re-encode.
     *
     * Keeps printable ASCII and valid UTF-8 (so a name typed in Cyrillic is not
     * silently turned into an empty string), drops control characters, the
     * separators the allow-list format uses (`|`) and everything that is not
     * valid UTF-8, trims surrounding blanks and truncates to kMaxLen bytes,
     * ending on a character boundary.
     */
    static std::string sanitize(const std::string& name);

private:
    /**
     * @brief Walk the option list and return the payload of `code`.
     * @return nullptr when the option is absent; `*outLen` is its length.
     */
    static const uint8_t* findOption(const uint8_t* options, size_t len,
                                     uint8_t code, size_t* outLen);

    /** @brief Decode a DNS-encoded name (RFC 1035 labels) into dotted form. */
    static std::string decodeDnsName(const uint8_t* data, size_t len);
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DHCPCLIENTNAME_H
