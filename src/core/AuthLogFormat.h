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
 * The operator asked for a file that holds the login attempts, and for four things
 * to be findable in it:
 *
 *   * **a refused password** — with the attempt number and the budget, so a
 *     reader can see a guest mistyping three times as easily as a machine working
 *     through a list;
 *   * **a lockout** — the moment the device stopped answering that address, and
 *     for how long;
 *   * **getting in after failing** — the line that answers the only question these
 *     leave open: was it the same person, or somebody who guessed?
 *   * **a login through the form** — added 16.10.2026, when the operator pointed
 *     out that a correct password typed into the login form was silent: he had
 *     asked for a record of logins, and the file held everything about a login
 *     except the one that went right.
 *
 * Only `authLogWritesSuccess()` decides which of the successes are written, and it
 * exists because a correct password is **not** an event at the HTTP layer: the
 * interface authenticates with Basic on every request, so a page load is dozens of
 * requests carrying the same credentials. Writing each of them would turn the file
 * into a record of browsing (thousands of lines an hour), which is what this file
 * is deliberately not. A login form is an event, so the page announces it with one
 * request of its own (`POST /api/auth/login`), and that request — and only that
 * request — writes the line even when nothing failed before it.
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
 *                     Zero (nothing to recover from) says the login plainly —
 *                     `logged in from 192.168.1.5` — because "after 0 failed
 *                     attempts" would be a sentence nobody should have to read
 *                     twice. Since 16.10.2026 that plain form is written as well:
 *                     it is what the login form reports when the password was
 *                     right the first time.
 */
inline std::string authLoginText(const std::string& clientIp, uint32_t failedBefore)
{
    if (failedBefore == 0) return "logged in from " + clientIp;

    return "logged in from " + clientIp + " after " + std::to_string(failedBefore) +
           (failedBefore == 1 ? " failed attempt" : " failed attempts");
}

/**
 * @brief Does this successful authentication belong in `Auth.log`?
 *
 * Two things are events, and this is the whole rule:
 *
 *   * **the login form was submitted** (`fromLoginForm`) — a person arriving. The
 *     page says so in a request of its own (`POST /api/auth/login`); nothing else
 *     claims to be a login, so the credentials the browser attaches to every other
 *     request cannot pretend to be one.
 *   * **the address had spent attempts before it got in** (`failedBefore > 0`) —
 *     the line stage 205 added, which is what tells a guest who mistyped from
 *     somebody working through a list.
 *
 * @param fromLoginForm the request came from the login form, not from a page.
 * @param failedBefore  attempts that address had spent before this success.
 */
inline bool authLogWritesSuccess(bool fromLoginForm, uint32_t failedBefore)
{
    return fromLoginForm || failedBefore > 0;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_AUTHLOGFORMAT_H
