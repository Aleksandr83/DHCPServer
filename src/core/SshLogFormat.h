#ifndef DHCP_CORE_SSHLOGFORMAT_H
#define DHCP_CORE_SSHLOGFORMAT_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace core {

/**
 * @file SshLogFormat.h
 * @brief How an SSH session reads in `SSH.log` (stage 223).
 *
 * The operator asked for four kinds of line, and these are they:
 *
 *   * **a session opened** — from which address and as which name, which is the
 *     first question anyone asks about a shell that was used;
 *   * **a session closed** — the same address and how long it lasted;
 *   * **a refused password** — with the attempt and the budget, in the same words
 *     `Auth.log` uses, so a reader of both files learns one shape rather than two;
 *   * **a refusal that never reached a password** — the subnet filter, and the
 *     session limit; these are the ones that would otherwise be invisible, since
 *     a refused connection leaves no trace in the console;
 *   * **the server's own start and stop**, with the port — the first thing an
 *     operator looks for when nothing else was written;
 *   * **a session that died before the password** — a failed key exchange or a
 *     session task that could not be started, with the reason libssh gave.
 *     Until 19.10.2026 these went to the UART only, so a connection reset by the
 *     server left an empty journal and no way to tell why.
 *
 * The text is built without its timestamp — `ErrorLogCore` adds that — which is
 * what lets a host test check the sentences.
 */

/// @brief `5 s`, `1 min 30 s`, `1 h 2 min 5 s`.
inline std::string sshDurationText(uint32_t seconds)
{
    const uint32_t hours = seconds / 3600;
    const uint32_t minutes = (seconds % 3600) / 60;
    const uint32_t secs = seconds % 60;

    auto hhmmss = [](uint32_t h, uint32_t m, uint32_t s) {
        return std::to_string(h) + " h " + std::to_string(m) + " min " +
               std::to_string(s) + " s";
    };

    if (hours > 0) {
        // Whole hours and whole minutes are named alone: "1 h" reads better than
        // "1 h 0 min 0 s" and is what a reader is looking for.
        if (minutes == 0 && secs == 0) return std::to_string(hours) + " h";
        if (secs == 0) {
            return std::to_string(hours) + " h " + std::to_string(minutes) + " min";
        }
        return hhmmss(hours, minutes, secs);
    }
    if (minutes > 0) {
        if (secs == 0) return std::to_string(minutes) + " min";
        return std::to_string(minutes) + " min " + std::to_string(secs) + " s";
    }
    return std::to_string(secs) + " s";
}

/// @brief `session opened from 192.168.1.5 as admin`.
inline std::string sshSessionOpenText(const std::string& clientIp,
                                      const std::string& user)
{
    return "session opened from " + clientIp + " as " +
           (user.empty() ? std::string("(no name)") : user);
}

/// @brief `session closed from 192.168.1.5 after 1 h 2 min 5 s`.
inline std::string sshSessionCloseText(const std::string& clientIp,
                                       uint32_t durationSec)
{
    return "session closed from " + clientIp + " after " + sshDurationText(durationSec);
}

/// @brief `refused password from 192.168.1.5 (2 of 3)`.
inline std::string sshAuthFailedText(const std::string& clientIp,
                                     uint32_t attempt, uint32_t maxAttempts)
{
    return "refused password from " + clientIp + " (" + std::to_string(attempt) +
           " of " + std::to_string(maxAttempts) + ")";
}

/// @brief `refused a session from 10.0.0.7: outside the own subnet`.
inline std::string sshSubnetRefusedText(const std::string& clientIp)
{
    return "refused a session from " + clientIp + ": outside the own subnet";
}

/// @brief `refused a session from 192.168.1.9: 2 of 2 already open`.
inline std::string sshSessionLimitText(const std::string& clientIp, uint32_t limit)
{
    return "refused a session from " + clientIp + ": " + std::to_string(limit) +
           " of " + std::to_string(limit) + " already open";
}

/// @brief `server started on port 22`.
inline std::string sshServerStartText(uint16_t port)
{
    return "server started on port " + std::to_string(port);
}

/// @brief `server stopped`.
inline std::string sshServerStopText()
{
    return "server stopped";
}

/**
 * @brief `session from 192.168.1.5 accepted`.
 *
 * The breadcrumb before the password: a session task is running and the key
 * exchange is about to start. If the device dies in that window — a stack
 * overflow inside libssh, which is what the 19.10.2026 panic turned out to be —
 * this is the last line in the journal, and the line alone tells the reader that
 * the accept task got as far as starting a session.
 */
inline std::string sshSessionAcceptedText(const std::string& clientIp)
{
    const std::string who =
        clientIp.empty() ? std::string("(unknown address)") : clientIp;
    return "session from " + who + " accepted";
}

/**
 * @brief `session from 192.168.1.5 started`.
 *
 * The second breadcrumb: a task of its own is running for this client, so the
 * next thing that happens is the key exchange, and a crash from there on leaves
 * this line as the journal's last.
 */
inline std::string sshSessionStartedText(const std::string& clientIp)
{
    const std::string who =
        clientIp.empty() ? std::string("(unknown address)") : clientIp;
    return "session from " + who + " started";
}

/**
 * @brief `session from 192.168.1.5 failed before the password: <reason>`.
 *
 * The line that was missing while a connection died at the key exchange: the
 * operator saw `Connection reset` and an empty journal, because everything that
 * can go wrong between `accept()` and the banner was written to the UART only.
 * The reason is libssh's own sentence, so the file carries what the terminal
 * would have said.
 */
inline std::string sshSessionFaultText(const std::string& clientIp,
                                       const std::string& reason)
{
    const std::string who =
        clientIp.empty() ? std::string("(unknown address)") : clientIp;
    return "session from " + who + " failed before the password: " + reason;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SSHLOGFORMAT_H
