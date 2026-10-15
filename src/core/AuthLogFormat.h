#ifndef DHCP_CORE_AUTHLOGFORMAT_H
#define DHCP_CORE_AUTHLOGFORMAT_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace core {

/**
 * @file AuthLogFormat.h
 * @brief How an authorisation attempt reads in `Auth.log` (stage 205).
 *
 * The operator asked for a file that holds the login attempts, and for three
 * things to be findable in it:
 *
 *   * **a refused password** — with the attempt number and the budget, so a
 *     reader can see a guest mistyping three times as easily as a machine working
 *     through a list;
 *   * **a lockout** — the moment the device stopped answering that address, and
 *     for how long;
 *   * **getting in after failing** — the line that answers the only question these
 *     three leave open: was it the same person, or somebody who guessed?
 *
 * A successful login with **no** failures before it is deliberately not written:
 * the interface asks for credentials on every request (HTTP Basic), so a page that
 * polls the device would produce a line every few seconds — thousands an hour —
 * and the file would stop being a record of attempts and become a record of
 * browsing. That is the operator's decision, and it is why only three sentences
 * exist here.
 *
 * The text is built without its timestamp — `ErrorLogCore` adds that, exactly as
 * it does for the journal — so the sentences can be checked on a host, which is
 * where a promise about a file is worth checking.
 */

/** @brief `failed login from 192.168.1.5 (3 of 5)`. */
inline std::string authFailedText(const std::string& clientIp, uint32_t attempt,
                                  uint32_t maxAttempts)
{
    return "failed login from " + clientIp + " (" + std::to_string(attempt) +
           " of " + std::to_string(maxAttempts) + ")";
}

/** @brief `locked out 192.168.1.5 for 300 s`. */
inline std::string authLockoutText(const std::string& clientIp, uint32_t lockoutSec)
{
    return "locked out " + clientIp + " for " + std::to_string(lockoutSec) + " s";
}

/**
 * @brief `logged in from 192.168.1.5 after 3 failed attempts`.
 *
 * @param failedBefore how many attempts that address had spent before it got in.
 *                     Zero (nothing to recover from) is said plainly rather than
 *                     dressed up as "after 0 failed attempts" — the caller does
 *                     not write that line at all today, and a sentence that cannot
 *                     be misread is worth the branch.
 */
inline std::string authLoginText(const std::string& clientIp, uint32_t failedBefore)
{
    if (failedBefore == 0) return "logged in from " + clientIp;

    return "logged in from " + clientIp + " after " + std::to_string(failedBefore) +
           (failedBefore == 1 ? " failed attempt" : " failed attempts");
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTHLOGFORMAT_H
