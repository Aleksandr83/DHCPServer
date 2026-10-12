#ifndef DHCP_CORE_NTPSERVERS_H
#define DHCP_CORE_NTPSERVERS_H

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace dhcp {
namespace core {

/**
 * @brief The servers the device asks for the time, as one setting (stage 195).
 *
 * The device used to hold a single external NTP server, and that turned out to
 * be the whole fault behind a device that never synchronised: `pool.ntp.org`
 * answered with four addresses here, the first of them a node that replies to
 * nothing, and lwIP hands a name's addresses back in the order the resolver sent
 * them and never rotates them - so the SNTP client asked that one dead node for
 * ever while three healthy ones sat in the same answer. The client now gets a
 * short ordered list instead (lwIP moves to the next server when one stops
 * answering), and this header is how the single text field becomes that list and
 * back, so the page, the settings file and the firmware all mean the same thing.
 *
 * Both separators are accepted: a comma (as the page's hint says) and a
 * semicolon, because a semicolon is what people type when a comma had a meaning
 * somewhere else. Names are trimmed, empty entries are dropped - a settings file
 * that ends with a separator is not an error - and the list is capped, since the
 * HTTP server and the resolver are not the place to find out that somebody
 * pasted a whole page.
 */

/// The separator the setting is stored and shown with.
constexpr const char* kNtpServerSeparator = ", ";

/// Whitespace the setting tolerates around a name.
constexpr const char* kNtpServerTrimChars = " \t\r\n";

/// Separators inside the setting: both mean "and the next server".
constexpr char kNtpServerComma = ',';
constexpr char kNtpServerSemicolon = ';';

/**
 * @brief How many servers the client is given, in order, before the clock is on
 *        its own again.
 *
 * Three is the point where a second dead node stops being plausible and a
 * mistake in the list becomes more likely than a bad network; it is also the
 * count `CONFIG_LWIP_SNTP_MAX_SERVERS` is set to, because lwIP keeps the
 * failover only for the indices its own limit covers.
 */
constexpr size_t kNtpMaxServers = 3;

/// Longest text the setting accepts: three real names fit, a runaway paste does not.
constexpr size_t kNtpServersTextMax = 120;

/**
 * @brief The servers a device asks when nobody has chosen any (stage 195).
 *
 * Three, because one is what the device had when a single dead node in a public
 * pool cost it the clock: the second and third are what make that impossible.
 * They are also three different operators' time services rather than one pool, so
 * a broken answer cannot take all of them down at once - and all three are known
 * to answer from a plain home LAN.
 */
constexpr const char* kNtpDefaultServers = "pool.ntp.org, time.google.com, time.cloudflare.com";

/** @brief One name, without the whitespace around it. */
inline std::string ntpServerTrim(const std::string& name)
{
    const size_t first = name.find_first_not_of(kNtpServerTrimChars);
    if (first == std::string::npos) return std::string();
    const size_t last = name.find_last_not_of(kNtpServerTrimChars);
    return name.substr(first, last - first + 1);
}

/**
 * @brief The setting text → the ordered list of servers.
 *
 * Empty entries are dropped rather than kept as an empty server, so a trailing
 * separator, a doubled one or a line broken by hand all mean the same list.
 */
inline std::vector<std::string> ntpServersSplit(const std::string& text)
{
    std::vector<std::string> servers;
    std::string current;
    // One lambda for both ends of the text: the last name would otherwise need
    // the same four lines a second time.
    const auto take = [&servers, &current]() {
        if (servers.size() >= kNtpMaxServers) {
            current.clear();
            return;
        }
        const std::string name = ntpServerTrim(current);
        if (!name.empty()) servers.push_back(name);
        current.clear();
    };

    for (const char ch : text) {
        if (ch == kNtpServerComma || ch == kNtpServerSemicolon) {
            take();
        } else {
            current += ch;
        }
    }
    take();
    return servers;
}

/**
 * @brief The ordered list of servers → the canonical setting text.
 *
 * The device stores what this returns, so the page shows back the list it
 * actually uses: one comma, one space, no padding, and at most
 * @ref kNtpMaxServers names. What does not fit either limit is left out rather
 * than written half-way - a truncated host name is a name that never resolves,
 * and the journal's "SNTP client started" line then shows what was kept.
 */
inline std::string ntpServersText(const std::vector<std::string>& servers)
{
    std::string out;
    size_t kept = 0;
    for (const std::string& raw : servers) {
        const std::string name = ntpServerTrim(raw);
        if (name.empty()) continue;
        if (kept >= kNtpMaxServers) break;

        if (out.empty()) {
            out = name;
        } else {
            const size_t extra = std::strlen(kNtpServerSeparator) + name.size();
            if (out.size() + extra > kNtpServersTextMax) break;
            out += kNtpServerSeparator;
            out += name;
        }
        // A single name longer than the whole setting is still cut: the setting
        // is what the device keeps and what it shows, so it cannot be the one
        // place that holds more than the field.
        if (out.size() > kNtpServersTextMax) out.resize(kNtpServersTextMax);
        ++kept;
    }
    return out;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_NTPSERVERS_H
