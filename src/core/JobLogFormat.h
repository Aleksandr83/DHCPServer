#ifndef DHCP_CORE_JOBLOGFORMAT_H
#define DHCP_CORE_JOBLOGFORMAT_H

#include <cstdint>
#include <cstdio>
#include <string>

namespace dhcp {
namespace core {

/**
 * @file JobLogFormat.h
 * @brief How a long-running operation reads in the job log (stage 173).
 *
 * Kept free of ESP-IDF — and of anything else — on purpose: the text of a log
 * line is a promise about what the file will say, and the host test is where a
 * promise is checked. The line without its stamp, so the two halves (the text
 * here, the timestamp in ErrorLogCore) can be tested apart.
 */

/// @brief Milliseconds in a second (rule 39: the unit, not a bare 1000).
inline constexpr uint32_t kMsPerSecond = 1000;

/**
 * @brief A duration as `12.3 s` — one decimal, integer arithmetic only.
 *
 * Floats are avoided deliberately: the value travels to a text file read by a
 * human, and a locale-dependent decimal point would be a bug nobody could see on
 * the device. A sub-second operation reads `0.4 s`, never `0 s`.
 */
inline std::string jobDurationText(uint32_t durationMs)
{
    char buf[24];
    const uint32_t whole = durationMs / kMsPerSecond;
    const uint32_t tenth = (durationMs % kMsPerSecond) / (kMsPerSecond / 10);
    // Statically cast to `unsigned`: on the target `uint32_t` is `unsigned long`,
    // and `-Werror=format` refuses `%u` for it (the host's is `unsigned int`).
    std::snprintf(buf, sizeof(buf), "%u.%u s", static_cast<unsigned>(whole),
                  static_cast<unsigned>(tenth));
    return buf;
}

/// @brief `file_check (/sdcard): started` (the argument is dropped when empty).
inline std::string jobStartedText(const std::string& id, const std::string& arg)
{
    std::string out = arg.empty() ? id : id + " (" + arg + ")";
    out += ": started";
    return out;
}

/**
 * @brief `file_check (/sdcard): finished (done, 12.3 s)`.
 *
 * @param stateText   `jobStateText()` of the end state (done/failed/cancelled).
 * @param detail      the operation's own last words, appended when not empty.
 */
inline std::string jobFinishedText(const std::string& id, const std::string& arg,
                                   const char* stateText, uint32_t durationMs,
                                   const std::string& detail)
{
    std::string out = arg.empty() ? id : id + " (" + arg + ")";
    out += ": finished (";
    out += stateText ? stateText : "unknown";
    out += ", ";
    out += jobDurationText(durationMs);
    out += ")";
    if (!detail.empty()) out += " — " + detail;
    return out;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_JOBLOGFORMAT_H
