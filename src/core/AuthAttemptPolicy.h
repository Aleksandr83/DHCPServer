#ifndef DHCP_CORE_AUTHATTEMPTPOLICY_H
#define DHCP_CORE_AUTHATTEMPTPOLICY_H

#include <string>

namespace dhcp {
namespace core {

/**
 * @brief True when this `Authorization` header is a guess at the password.
 *
 * The web interface authenticates with HTTP Basic, and Basic begins with a
 * request that carries **no** credentials at all: the browser asks for the page,
 * the server answers `401` with `WWW-Authenticate`, and only then does the
 * browser repeat the request with the header. A request without a header is
 * therefore not an attempt at anything — it is the protocol working as designed.
 *
 * Counting it as an attempt was a defect with a wide blast radius (04.10.2026):
 * the counter is kept per client, the client address was the same for every
 * client, and the interface's own `navigator.sendBeacon()` sends no header at
 * all — so five page switches locked the whole web interface out for five
 * minutes, and the serial log showed it as `Auth failed for 192.168.1.0 (5/5)`.
 *
 * A header that really does carry a `Basic` credential is still counted, so the
 * protection against guessing a password is unchanged; so is a `Basic` header
 * whose credential is empty or cannot be decoded, which is a guess too.
 *
 * Note what this deliberately does **not** open: a client that sends no header
 * is never authenticated, it only stops spending the attempt budget of others.
 */
inline bool countsAsFailedAttempt(const std::string& authHeader)
{
    // The scheme is compared exactly, because that is the test the credential
    // parser itself makes: a header the parser would refuse must not be the one
    // the counter treats differently.
    static const std::string kBasicPrefix = "Basic ";
    if (authHeader.size() <= kBasicPrefix.size()) return false;
    return authHeader.compare(0, kBasicPrefix.size(), kBasicPrefix) == 0;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTHATTEMPTPOLICY_H
