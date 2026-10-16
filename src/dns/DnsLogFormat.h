#ifndef DHCP_DNS_DNSLOGFORMAT_H
#define DHCP_DNS_DNSLOGFORMAT_H

#include <cstdint>
#include <string>

#include "core/LogFileFormat.h"

namespace dhcp {
namespace dns {

/**
 * @file DnsLogFormat.h
 * @brief What a DNS log line says and where the file log of forwarded answers
 *        lives (stage 204).
 *
 * The DNS logger writes its lines to three places: the terminal, an external REST
 * service, and - since this stage - a file on the internal volume,
 * `/fat/logs/Forwarded.log`, archived to the card as
 * `log/Archive/Dns/<first>-<last>/Forwarded.log` when it is full. The rotation
 * policy is not repeated here: it is `core::LogFileWriter`'s, and the writer this
 * log uses (`DnsForwardedLogWriter`) answers only *where* its archives go and how
 * it names itself in `Errors.log`.
 *
 * What is decided here is the sentence, and it is deliberately **one** sentence
 * for two destinations: the terminal shows `DNS: [forward] A example.com ->
 * 1.2.3.4 (from 192.168.1.5 mac=aa:bb:cc:dd:ee:ff)` and the file shows the same
 * with the local time in front of it (`[2026.10.12 19:22:14] DNS: …`). Two copies
 * of that shape would drift, and a log whose terminal half and file half disagree
 * is worse than either.
 *
 * Free of ESP-IDF on purpose - host-tested in `test/test_dnslogformat.cpp`.
 */

// Rule 39: the record types a log line names, instead of the numbers in place.
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeCname = 5;
constexpr uint16_t kDnsTypeMx = 15;
constexpr uint16_t kDnsTypeAaaa = 28;

/** @brief The live log of forwarded answers, inside its folder on the volume. */
constexpr const char* kDnsForwardedLogFileName = "Forwarded.log";

/** @brief Folder of its archives on the card, relative to the mount point. */
constexpr const char* kDnsForwardedLogArchiveFolder = "log/Archive/Dns";

/**
 * @brief The type column of a line: `A`, `AAAA`, `MX`, `CNAME`, `OTHER`.
 *
 * An unknown type prints as a word rather than a number: a reader of the file is
 * looking for what was asked, and `OTHER` says "something this device does not
 * name" without inviting him to decode 16 bits.
 */
inline std::string dnsLogTypeText(uint16_t type)
{
    if (type == kDnsTypeA) return "A";
    if (type == kDnsTypeAaaa) return "AAAA";
    if (type == kDnsTypeMx) return "MX";
    if (type == kDnsTypeCname) return "CNAME";
    return "OTHER";
}

/**
 * @brief How a query ended — what the sentence says after `->`.
 *
 * It is an enumeration and not a boolean because the difference matters to whoever
 * reads the file: `NXDOMAIN` is a statement about the **name** ("it does not
 * exist", which a client caches for minutes), while the other four are statements
 * about **us** (an upstream that stayed silent, a full table, a socket that would
 * not open, a query this device refuses to forward on purpose). Until 16.10.2026
 * every one of them printed `NXDOMAIN`, so a slow upstream read exactly like a
 * name that does not exist - the operator's log showed three of those at 22:57 and
 * the same name answered with addresses at 23:02, with nothing in the file to tell
 * the two apart. A genuine NXDOMAIN from the upstream was invisible for the
 * opposite reason: a forwarded answer was logged as `(forwarded)` whatever its
 * return code.
 */
enum class DnsQueryOutcome {
    Answered,   ///< an answer came back (a local host, a cache, or the upstream)
    Nxdomain,   ///< the upstream said the name does not exist, and that is what the client got
    Timeout,    ///< the upstream stayed silent: we stopped waiting and told the client NXDOMAIN
    NoSlot,     ///< every forward slot was busy: NXDOMAIN was the answer the client got
    NoSocket,   ///< the forward socket would not open: NXDOMAIN was the answer the client got
    NoData,     ///< the name is known but has no record of this type: NODATA was the answer
    Blocked,    ///< "block non-A/AAAA": NODATA was the answer, on purpose
};

/**
 * @brief What the sentence prints after `->`, from the outcome and the answer.
 *
 * The three failures that end with a **fabricated NXDOMAIN** say so, and say why:
 * `NXDOMAIN (timeout)` is a different fact from `NXDOMAIN`, and it is the one the
 * reader needs. (Those three are recorded here rather than fixed: the operator
 * asked for the log to tell the truth and left the behaviour alone, 16.10.2026.)
 */
inline std::string dnsLogOutcomeText(DnsQueryOutcome outcome,
                                     const std::string& answer)
{
    switch (outcome) {
    case DnsQueryOutcome::Answered: return answer;
    case DnsQueryOutcome::Nxdomain: return "NXDOMAIN";
    case DnsQueryOutcome::Timeout:  return "NXDOMAIN (timeout)";
    case DnsQueryOutcome::NoSlot:   return "NXDOMAIN (no slot)";
    case DnsQueryOutcome::NoSocket: return "NXDOMAIN (no socket)";
    case DnsQueryOutcome::NoData:   return "(no data)";
    case DnsQueryOutcome::Blocked:  return "(blocked)";
    }
    return answer;
}

/**
 * @brief The sentence both destinations show, without the stamp.
 *
 * @param tag        where the answer came from (`forward`, `local`, `cache`)
 * @param type       the query type
 * @param domain     the name that was asked
 * @param outcome    how the query ended — see @ref DnsQueryOutcome, and note that
 *                   only `Nxdomain` prints `NXDOMAIN`
 * @param answer     the address that came back (used by `Answered`)
 * @param clientAddr who asked
 * @param clientMac  its MAC, or "" when it is not known yet
 */
inline std::string dnsLogQueryText(const std::string& tag, uint16_t type,
                                   const std::string& domain,
                                   DnsQueryOutcome outcome,
                                   const std::string& answer,
                                   const std::string& clientAddr,
                                   const std::string& clientMac)
{
    std::string text = "[" + tag + "] " + dnsLogTypeText(type) + " " + domain + " -> ";
    text += dnsLogOutcomeText(outcome, answer);
    text += " (from " + clientAddr + " mac=" + clientMac + ")";
    return text;
}

/**
 * @brief One line of the file log: the local time, then the sentence.
 *
 * The stamp comes from `core/LogFileFormat.h`, so this file lines up with the NTP
 * log's lines and a line written before the clock was set shows the same
 * placeholder rather than a date that never happened.
 */
inline std::string dnsForwardedLogLine(const core::LogStamp& stamp,
                                       const std::string& body)
{
    return core::logStampPrefix(stamp) + " DNS: " + body;
}

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSLOGFORMAT_H
