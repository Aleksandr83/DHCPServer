#ifndef DHCP_WEB_REDIRECTPOLICY_H
#define DHCP_WEB_REDIRECTPOLICY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Like RouteTable.h, this header is compiled on a host as well (see
// test/test_redirectpolicy.cpp), so it must not pull in ESP-IDF: the decision,
// the status line and the address are plain C++, and WebServer.cpp is the only
// place that touches an httpd request with them.

namespace dhcp {
namespace web {

/**
 * @brief The rules behind "plain HTTP answers with the TLS address" (stage 167).
 *
 * While the TLS server is up and the pair it serves has not expired, every
 * request that arrives on the plain port is answered with a permanent redirect
 * to the same address over `https://`. The rules live here, away from the
 * request that carries them, for the same reason the route-table rules do: the
 * address a redirect builds is written from a client-supplied header, and that
 * is exactly the kind of code that has to be run — not looked at — before a
 * device offers it to a browser.
 *
 * The three answers the gate in WebServer::httpsRedirectGate needs are
 * @ref needed, @ref statusLineForMethod and @ref targetFrom.
 */
namespace redirect {

/** @brief The port a browser assumes behind `https://` when the URL names none. */
constexpr uint16_t kDefaultHttpsPort = 443;

/**
 * @brief Status line for a request that carries no body.
 *
 * A redirected GET repeats itself: the same bytes over TLS mean the same thing.
 */
constexpr const char* kStatusMovedPermanently = "301 Moved Permanently";

/**
 * @brief Status line for a request whose method has to survive the redirect.
 *
 * A browser follows a 301 with a GET, whatever the original method was, so a
 * POST answered with 301 arrives at the TLS server as a GET with its body gone.
 * 308 exists for that request, and it is still permanent for the caching the
 * operator asked for.
 */
constexpr const char* kStatusPermanentRedirect = "308 Permanent Redirect";

/**
 * @brief Should the plain server answer with a redirect right now?
 *
 * Both halves have to hold, and they answer two different questions: whether
 * anything is listening on the TLS port at all, and whether the pair it serves
 * is still inside its validity. The second one is compared against the clock on
 * every request, so a certificate that runs out while the device is up turns
 * the redirect off by itself — an address that no browser can open is worse than
 * no redirect.
 *
 * A missing date (`notAfterEpoch` 0) means the pair was never judged usable, and
 * the answer is then no.
 */
constexpr bool needed(bool tlsServing, int64_t notAfterEpoch, int64_t nowSec)
{
    if (!tlsServing) return false;
    if (notAfterEpoch <= 0) return false;
    return nowSec < notAfterEpoch;
}

/** @brief Which of the two permanent codes a request of @p method gets. */
inline const char* statusLineForMethod(std::string_view method)
{
    return (method == "GET" || method == "HEAD") ? kStatusMovedPermanently
                                                : kStatusPermanentRedirect;
}

/**
 * @brief Drop the characters that must not reach a header value or an attribute.
 *
 * Spaces and control characters cannot appear in a header value at all, and the
 * three that end an HTML attribute are dropped because the same text is handed
 * to the browser both ways. What is left of a client-supplied header is what a
 * URL can carry.
 */
inline std::string keepSafe(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value <= 0x20 || value == 0x7f) continue;
        if (c == '<' || c == '>' || c == '"') continue;
        out.push_back(c);
    }
    return out;
}

/**
 * @brief Could @p name be the host of a URL?
 *
 * A client-supplied `Host` is either used whole or not at all. Cleaning it
 * character by character is the one thing this must not do: `dns.lo` with
 * anything glued to it is a different name, and sending a browser there would
 * be a redirect to somewhere the visitor never asked for. So the shape is
 * checked instead, and a name that fails it is refused — the caller then answers
 * the request exactly as it would have without the redirect.
 *
 * What is allowed is what a host name, an IPv4 address, an IPv6 literal in
 * brackets and a port in any of them are made of.
 */
inline bool isUsableHost(std::string_view name)
{
    if (name.empty()) return false;
    for (const char c : name) {
        const bool usable = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') ||
                            c == '.' || c == '-' || c == '_' || c == ':' ||
                            c == '[' || c == ']';
        if (!usable) return false;
    }
    return true;
}

/**
 * @brief The address a request on the plain port is sent to.
 *
 * The name is the one the client used (its own `Host`), not the one in the
 * certificate: a browser that arrives under a name reaches the device under that
 * name again, and redirecting to a name the client cannot resolve would end the
 * visit at a DNS failure instead of at the address it already had.
 *
 * A port in `Host` is dropped and the TLS port takes its place, so a client that
 * named `:80` and one that named nothing get the same address. An IPv6 literal
 * keeps its brackets — they are what makes it a host in a URL. An empty answer
 * means there is no address to build (a request without a usable `Host`), and
 * the caller then answers the request as it always did.
 */
inline std::string targetFrom(std::string_view host, std::string_view uri, uint16_t httpsPort)
{
    // Space around the value belongs to the header, not to the name in it, so it
    // is trimmed; anything else unusual is not a name and is left for the caller
    // to answer the plain way.
    const std::size_t first = host.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const std::size_t last = host.find_last_not_of(" \t");
    std::string name(host.substr(first, last - first + 1));
    if (!isUsableHost(name)) return {};

    if (name.front() == '[') {
        const std::size_t close = name.find(']');
        // A literal that never closes, or one that opens twice, is not one.
        if (close == std::string::npos || name.find('[', 1) < close) return {};
        name.resize(close + 1);
    } else {
        if (name.find('[') != std::string::npos) return {};
        const std::size_t colon = name.rfind(':');
        if (colon != std::string::npos &&
            name.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
            name.resize(colon);
        }
    }
    if (name.empty()) return {};

    std::string out = "https://";
    out += name;
    if (httpsPort != kDefaultHttpsPort) {
        out += ':';
        out += std::to_string(httpsPort);
    }
    const std::string path = keepSafe(uri);
    out += path.empty() ? "/" : path;
    return out;
}

/** @brief @p text with the four characters that would break the HTML around it. */
inline std::string htmlEscaped(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        default:   out.push_back(c); break;
        }
    }
    return out;
}

} // namespace redirect
} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_REDIRECTPOLICY_H
