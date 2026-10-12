#ifndef DHCP_CORE_NTPLOGFORMAT_H
#define DHCP_CORE_NTPLOGFORMAT_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace dhcp {
namespace core {

/**
 * @file NtpLogFormat.h
 * @brief Where the NTP request log lives and how it rotates (stage 191).
 *
 * The operator asked for one growing file on the internal volume and a rotation
 * that is a small archive expedition rather than the `.1` rename the other logs
 * use:
 *
 *   * the live file is `/fat/logs/Ntp.log`, capped at 64 KB — the same cap
 *     `Errors.log` lives with, so "full" means one thing in the whole project;
 *   * a full file goes to the card as `log/Archive/Ntp/<first>-<last>/Ntp.log`,
 *     the folder named by the date of the first line's stamp and of the last
 *     one, and the file is **appended to** when that range is already there (one
 *     file per range, on purpose — the range *is* the name);
 *   * the live file then starts again **empty**;
 *   * with no card at all it starts again with the **last ten lines** of the old
 *     one, because those are what a diagnosis needs and 64 KB is not worth
 *     keeping where there is nowhere to keep it;
 *   * when the card is there but refuses the file, the full file is **kept** (it
 *     is the only copy of the log) and the attempt is repeated on the next
 *     record, up to ten times with a line in `Errors.log` for each failure —
 *     after the tenth the writer gives up and carries the tail.
 *
 * Three rules sit in this header because they are promises about files that one
 * run on the device cannot check, and they are the ones a host test can hold
 * still: the range name, the folder chain (a typo here and the log silently
 * lands elsewhere), and the carried tail (all that is left of a full file when
 * there is no card).
 *
 * Free of ESP-IDF on purpose — host-tested in `test/test_ntplogformat.cpp`.
 */

/** @brief Name of the live log inside its folder on the internal volume. */
constexpr const char* kNtpLogFileName = "Ntp.log";

/** @brief Folder of the live log, relative to its mount point (`/fat/logs/...`). */
constexpr const char* kNtpLogFolder = "logs";

/** @brief Size at which the live log rotates — the cap `Errors.log` uses too. */
constexpr size_t kNtpLogMaxBytes = 64u * 1024u;

/** @brief Lines carried into the new file when the archive cannot be written. */
constexpr size_t kNtpLogKeepLines = 10;

/** @brief Archive attempts before the writer gives up and carries the tail. */
constexpr size_t kNtpLogMaxArchiveAttempts = 10;

/** @brief Folder of the archives on the card, relative to its mount point. */
constexpr const char* kNtpLogArchiveFolder = "log/Archive/Ntp";

/**
 * @brief A calendar date, the only part of a stamp this header needs.
 *
 * A range folder is named by days, not by seconds: the file it holds spans them
 * all, and a name that can be read at a glance on a card is worth more than the
 * exact minute.
 */
struct NtpLogDate {
    int year = 1970;
    int month = 1;
    int day = 1;
};

/**
 * @brief A line's local time, as the line itself carries it.
 *
 * `clockSet` is the writer's only notion of "the device knows what time it is":
 * the file task reads the clock once per record and says so, the writer keeps a
 * file of unstamped lines from naming a folder on the operator's card, and the
 * line itself shows the placeholder instead of a date that never happened.
 */
struct NtpLogStamp {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool clockSet = false;
};

/** @brief The stamp shown when the device has no clock yet (21 characters). */
constexpr const char* kNtpLogNoClockPrefix = "[----.--.-- --:--:--]";

/**
 * @brief What a line says when the reply admitted the clock was not verified.
 *
 * LI = 3 / stratum 16 is what the client receives in that case, and this mark is
 * what the *log* adds to it: a file whose whole purpose is to explain a device
 * that will not synchronise must not be silent about the requests it did answer.
 */
constexpr const char* kNtpLogNoClockMark = " (clock not verified)";

// Rule 39: the field bases of struct tm — the year counts from 1900, the month
// from 0 — the same two the time server names for the same reason.
constexpr int kTmYearBase = 1900;
constexpr int kTmMonthBase = 1;

/**
 * @brief The stamp of a line, from a local time and whether the clock is real.
 *
 * @param local    The device's local time (already the operator's zone: the C
 *                 library is pointed at it when the time settings are applied).
 * @param clockSet False while the clock has never been set, in which case the
 *                 fields are left alone and the placeholder is what gets printed.
 */
inline NtpLogStamp ntpLogStampFromLocal(const std::tm& local, bool clockSet)
{
    NtpLogStamp stamp;
    if (!clockSet) return stamp;

    stamp.year = local.tm_year + kTmYearBase;
    stamp.month = local.tm_mon + kTmMonthBase;
    stamp.day = local.tm_mday;
    stamp.hour = local.tm_hour;
    stamp.minute = local.tm_min;
    stamp.second = local.tm_sec;
    stamp.clockSet = true;
    return stamp;
}

/**
 * @brief `[2026.10.12 19:22:14]`, or the placeholder while the clock is unset.
 *
 * Dotted date and second precision — seconds are kept because this log is read to
 * see how a synchronisation problem looked from the clients' side, and a minute
 * is too coarse for that — and deliberately the same width as
 * @ref kNtpLogNoClockPrefix: a log whose first lines were written before the first
 * synchronisation still lines up under the rest of the file, and `----.--.--`
 * says "there was no time to write" instead of inventing `1970-01-01`, which is a
 * date that never happened on this device.
 */
inline std::string ntpLogStampPrefix(const NtpLogStamp& stamp)
{
    if (!stamp.clockSet) return std::string(kNtpLogNoClockPrefix);

    char buf[32];
    snprintf(buf, sizeof(buf), "[%04d.%02d.%02d %02d:%02d:%02d]",
             stamp.year, stamp.month, stamp.day,
             stamp.hour, stamp.minute, stamp.second);
    return std::string(buf);
}

/**
 * @brief What the state line says about resolving the NTP server's own name.
 *
 * `Failed` is the one that matters: the device asks its own DNS server (see
 * `EthManager`), which forwards to the configured upstream — so a wrong or
 * unreachable upstream means the NTP client never gets an address, and "no
 * confirmed reply" follows from that alone.
 */
enum class NtpDnsVerdict {
    Ok,       ///< the name resolved; the address follows
    Failed,   ///< it did not resolve
    Pending,  ///< the answer had not arrived by the time the line was due
};

/** @brief `DNS -> 5.61.32.14`, `DNS lookup failed`, `DNS lookup still pending`. */
inline std::string ntpDnsVerdictText(NtpDnsVerdict verdict,
                                     const std::string& resolvedIp = std::string())
{
    switch (verdict) {
        case NtpDnsVerdict::Ok:     return "DNS -> " + resolvedIp;
        case NtpDnsVerdict::Failed: return "DNS lookup failed";
        case NtpDnsVerdict::Pending: break;
    }
    return "DNS lookup still pending";
}

/**
 * @brief The resolver chain, said out loud when the client starts (stage 193).
 *
 * The device resolves through itself — `EthManager` points the system resolver at
 * its own address so that `.lo` names work — and the built-in DNS server forwards
 * external names to the upstream below. That makes the upstream a single point of
 * failure for the device's own name resolution, and this line is what turns a
 * silent "no confirmed reply" into something a reader can check.
 *
 * @param deviceIp    The device's own address (the resolver it asks).
 * @param upstreamDns What that server forwards to (`DnsConfig::externalDns`).
 * @param gateway     The route out of the LAN, i.e. what makes the request leave.
 */
inline std::string ntpResolverChainText(const std::string& deviceIp,
                                        const std::string& upstreamDns,
                                        const std::string& gateway)
{
    const std::string device = deviceIp.empty() ? std::string("not set") : deviceIp;
    const std::string upstream = upstreamDns.empty() ? std::string("not set") : upstreamDns;
    const std::string route = gateway.empty() ? std::string("not set") : gateway;
    return "resolver: this device (" + device + "), upstream " + upstream +
           ", gateway " + route;
}

/**
 * @brief The periodic "still not synchronised" line, with the DNS verdict on it.
 *
 * One line has to carry the whole situation, because the reader has no console
 * and one line is what he gets: how long the device has been waiting, which
 * server it is waiting for, and whether it can even resolve that server's name.
 */
inline std::string ntpSyncStateText(uint32_t waitedSec, const std::string& server,
                                    const std::string& dnsVerdict)
{
    return "still not synchronised: " + std::to_string(waitedSec) +
           " s since the client started, server=" + server +
           ", no confirmed reply yet (" + dnsVerdict + ")";
}

/**
 * @brief The wait before the next "still not synchronised" line (stage 192).
 *
 * The operator's schedule: at most three lines a minute apart — a synchronisation
 * that will happen usually happens in seconds, and a minute is enough to see that
 * it did not — then every 2, 5 and 10 minutes, and 10 minutes from then on. The
 * point of the growth is the filesystem: at a line a minute a device that can
 * never synchronise would write 1440 lines a day into the log that is meant to
 * describe the problem, and every one of them would say the same thing.
 *
 * @param index How many such lines have already been written since the client
 *              started (0 for the first one).
 */
inline uint32_t ntpSyncStateDelaySec(std::size_t index)
{
    // Rule 39: the steps of the schedule, named where they are used.
    constexpr uint32_t kQuickSec = 60;      // the first three lines
    constexpr std::size_t kQuickCount = 3;
    constexpr uint32_t kSlowSec = 120;
    constexpr uint32_t kSlowerSec = 300;
    constexpr uint32_t kSlowestSec = 600;   // and the ceiling, for as long as it takes

    if (index < kQuickCount) return kQuickSec;
    if (index == kQuickCount) return kSlowSec;
    if (index == kQuickCount + 1) return kSlowerSec;
    return kSlowestSec;
}

/**
 * @brief What the success line says about how far the clock was off.
 *
 * The device cannot ask the server what the offset was — ESP-IDF's SNTP sets the
 * clock before telling us — so this is an **estimate** from the last reading of
 * the clock that the writer took: what the clock said then, advanced by the time
 * that has passed since, against what it says now. It is honest about its own
 * limits: with no real time in the earlier reading (a device that never had a
 * clock) there is nothing to compare with, and the line says that instead of
 * printing a shift of half a century.
 *
 * @param prevUtcSec    The earlier reading, in UTC epoch seconds.
 * @param prevUptimeSec Uptime seconds at that reading.
 * @param nowUtcSec     The clock as it is now (after the synchronisation).
 * @param nowUptimeSec  Uptime seconds now.
 * @param prevClockSet  False when that earlier reading was not a real date.
 */
inline std::string ntpLogClockShiftText(int64_t prevUtcSec, uint32_t prevUptimeSec,
                                        int64_t nowUtcSec, uint32_t nowUptimeSec,
                                        bool prevClockSet)
{
    if (!prevClockSet) return "the clock had no real time yet";

    const int64_t elapsed = static_cast<int64_t>(nowUptimeSec) -
                            static_cast<int64_t>(prevUptimeSec);
    const int64_t shift = nowUtcSec - (prevUtcSec + elapsed);
    if (shift > 0) return "the clock was " + std::to_string(shift) + " s behind";
    if (shift < 0) return "the clock was " + std::to_string(-shift) + " s ahead";
    return "the clock was already right";
}

/**
 * @brief One event line of the log (the client starting, a synchronisation…).
 *
 * Same shape as a request line — stamp, then the sentence — so that a file whose
 * reader is looking for "what happened at 20:14" does not have two formats to
 * parse.
 */
inline std::string ntpLogEventLine(const NtpLogStamp& stamp, const std::string& text)
{
    return ntpLogStampPrefix(stamp) + " " + text;
}

/**
 * @brief One line of the NTP request log.
 *
 * The whole line is built here, so the shape a reader sees on the card is decided
 * in one place that is tested: `[2026.10.12 19:22:14] NTP request from 192.168.1.5
 * -> stratum 3`.
 *
 * @param clockVerified False for a request the server answered with "the clock is
 *        not trustworthy" (LI = 3, stratum 16). Those lines are the interesting
 *        ones when the device cannot synchronise — they are the proof that a
 *        client is asking at all — so they are written with @ref kNtpLogNoClockMark
 *        rather than dropped. The stratum passed in is the one the reply carried,
 *        so the line never claims a stratum the client never saw.
 */
inline std::string ntpLogLine(const NtpLogStamp& stamp, const std::string& client,
                              unsigned stratum, bool clockVerified = true)
{
    return ntpLogStampPrefix(stamp) + " NTP request from " + client +
           " -> stratum " + std::to_string(stratum) +
           (clockVerified ? std::string() : std::string(kNtpLogNoClockMark));
}

/**
 * @brief `2026.10.11-2026.10.12` — the folder name for a range of days.
 *
 * Zero-padded, largest field first, so the names sort on the card as they
 * happened and a reader can see the span of a file without opening it.
 *
 * @param first Day of the first line of the archive.
 * @param last  Day of its last line.
 */
inline std::string ntpLogRangeName(const NtpLogDate& first, const NtpLogDate& last)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d.%02d.%02d-%04d.%02d.%02d",
             first.year, first.month, first.day, last.year, last.month, last.day);
    return std::string(buf);
}

/**
 * @brief The directories to create for one archive, outermost first.
 *
 * One level at a time: `mkdir` refuses a path whose parent is missing, and a
 * single call for `log/Archive/Ntp` would create nothing at all — which is how an
 * archive ends up missing with no error anywhere.
 *
 * @param mountPoint Volume the archive lives on, e.g. `/sdcard`.
 * @param rangeName  Folder name from @ref ntpLogRangeName.
 * @return e.g. `{"/sdcard/log", "/sdcard/log/Archive", "/sdcard/log/Archive/Ntp",
 *         "/sdcard/log/Archive/Ntp/2026.10.11-2026.10.12"}`; empty when there is
 *         no volume or no range, because an archive with nowhere to live is no
 *         archive.
 */
inline std::vector<std::string> ntpLogFolderChain(const std::string& mountPoint,
                                                  const std::string& rangeName)
{
    std::vector<std::string> out;
    if (mountPoint.empty() || rangeName.empty()) return out;

    std::string base = mountPoint;
    if (base.back() == '/') base.pop_back();
    if (base.empty()) return out;

    out.push_back(base + "/log");
    out.push_back(base + "/log/Archive");
    out.push_back(base + "/" + kNtpLogArchiveFolder);
    out.push_back(base + "/" + kNtpLogArchiveFolder + "/" + rangeName);
    return out;
}

/**
 * @brief The last @p lines of @p text, newline-terminated.
 *
 * This is what survives a rotation with no card to archive to, so it is worth
 * being exact about the two cases that look like mistakes: a file that ends
 * without a newline (its last line is a line all the same) and a file shorter
 * than the tail (the whole file comes back — losing ten lines of a twenty-line
 * file to "the last ten" would be an arithmetic accident, not a decision).
 *
 * @param text  The file as it was read.
 * @param lines How many lines to keep. Zero keeps nothing.
 * @return The tail, always newline-terminated, or "" when @p text is empty.
 */
inline std::string ntpLogTail(const std::string& text, size_t lines)
{
    if (lines == 0 || text.empty()) return std::string();

    // A trailing newline terminates the last line; it does not start an empty
    // one.
    size_t end = text.size();
    if (text[end - 1] == '\n') --end;

    size_t start = end;
    size_t found = 0;
    while (start > 0 && found < lines) {
        --start;
        if (text[start] == '\n') ++found;
    }
    if (found == lines) ++start;   // step over the newline that opens the tail

    return text.substr(start, end - start) + "\n";
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_NTPLOGFORMAT_H
