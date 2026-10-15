#ifndef DHCP_CORE_LOGFILEFORMAT_H
#define DHCP_CORE_LOGFILEFORMAT_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace dhcp {
namespace core {

/**
 * @file LogFileFormat.h
 * @brief Where a file log lives on the storage, how its archive is named, and
 *        what survives a rotation (stages 191 and 203).
 *
 * Every log this device keeps in a file follows the same rule, and the rule is the
 * operator's: one growing file per log on the internal volume, and a rotation that
 * is a small archive expedition — a copy to the card — rather than the `.1` rename
 * a log of one generation used to get:
 *
 *   * a live file is capped — 64 KB by default, or the size the operator picks on
 *     the page (see `LogFileSizes.h`), so "full" means one thing in the whole
 *     project;
 *   * a full file goes to the card inside `log/Archive/<log>/<first>-<last>/`,
 *     the folder named by the date of the first line's stamp and of the last one,
 *     and the file is **appended to** when that range is already there (one file
 *     per range, on purpose — the range *is* the name);
 *   * the live file then starts again **empty**;
 *   * with no card at all it starts again with the **last ten lines** of the old
 *     one, because those are what a diagnosis needs and 64 KB is not worth
 *     keeping where there is nowhere to keep it;
 *   * when the card is there but refuses the file, the full file is **kept** (it
 *     is the only copy of the log) and the attempt is repeated on the next
 *     record, up to ten times with a line in `Errors.log` for each failure —
 *     after the tenth the writer gives up and carries the tail.
 *
 * This header holds the parts of that policy that are addresses and text — the
 * range name, the folder chain, the carried tail and the stamp — and
 * `LogFileWriter.h` holds the one that touches the storage. Stage 203 split them
 * out of `NtpLogFormat.h` when the NTP log stopped being the only one; the logs
 * themselves keep what is about their own lines.
 *
 * Free of ESP-IDF on purpose — host-tested in `test/test_logfileformat.cpp`.
 */

/** @brief Size at which a live log rotates when nothing else is configured. */
constexpr size_t kLogFileDefaultMaxBytes = 64u * 1024u;

/**
 * @brief Folder the live logs share on the internal volume.
 *
 * `/fat/logs/...` for every file log the device keeps, because they are read
 * together — the NTP requests, the forwarded answers, the errors — and a reader
 * looking for one of them should not have to know which folder that particular
 * log chose.
 */
constexpr const char* kLogFileLiveFolder = "logs";

/** @brief Lines carried into the new file when the archive cannot be written. */
constexpr size_t kLogFileKeepLines = 10;

/** @brief Archive attempts before the writer gives up and carries the tail. */
constexpr size_t kLogFileMaxArchiveAttempts = 10;

/**
 * @brief A calendar date, the only part of a stamp this header needs.
 *
 * A range folder is named by days, not by seconds: the file it holds spans them
 * all, and a name that can be read at a glance on a card is worth more than the
 * exact minute.
 */
struct LogDate {
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
struct LogStamp {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool clockSet = false;
};

/** @brief The stamp shown when the device has no clock yet (21 characters). */
constexpr const char* kLogNoClockPrefix = "[----.--.-- --:--:--]";

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
inline LogStamp logStampFromLocal(const std::tm& local, bool clockSet)
{
    LogStamp stamp;
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
 * Dotted date and second precision — seconds are kept because a log is read to
 * see how a problem looked from the clients' side, and a minute is too coarse for
 * that — and deliberately the same width as @ref kLogNoClockPrefix: a log whose
 * first lines were written before the first synchronisation still lines up under
 * the rest of the file, and `----.--.--` says "there was no time to write"
 * instead of inventing `1970-01-01`, which is a date that never happened here.
 */
inline std::string logStampPrefix(const LogStamp& stamp)
{
    if (!stamp.clockSet) return std::string(kLogNoClockPrefix);

    char buf[32];
    snprintf(buf, sizeof(buf), "[%04d.%02d.%02d %02d:%02d:%02d]",
             stamp.year, stamp.month, stamp.day,
             stamp.hour, stamp.minute, stamp.second);
    return std::string(buf);
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
inline std::string logRangeName(const LogDate& first, const LogDate& last)
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
 * archive ends up missing with no error anywhere. The levels are the segments of
 * @p archiveFolder itself, so the second log's archives are created by the same
 * code the first one's are.
 *
 * @param mountPoint    Volume the archive lives on, e.g. `/sdcard`.
 * @param archiveFolder Folder of this log's archives on that volume, e.g.
 *                      `log/Archive/Ntp` — one of the two things a log names
 *                      about itself (see `LogFileWriter`).
 * @param rangeName     Folder name from @ref logRangeName.
 * @return e.g. `{"/sdcard/log", "/sdcard/log/Archive", "/sdcard/log/Archive/Ntp",
 *         "/sdcard/log/Archive/Ntp/2026.10.11-2026.10.12"}`; empty when there is
 *         no volume, no folder or no range, because an archive with nowhere to
 *         live is no archive.
 */
inline std::vector<std::string> logFolderChain(const std::string& mountPoint,
                                               const std::string& archiveFolder,
                                               const std::string& rangeName)
{
    std::vector<std::string> out;
    if (mountPoint.empty() || archiveFolder.empty() || rangeName.empty()) return out;

    std::string base = mountPoint;
    if (base.back() == '/') base.pop_back();
    if (base.empty()) return out;

    // The segments of the archive folder, one level per segment. An empty segment
    // (`log//Archive`, or a leading '/') is skipped rather than made into a level
    // of its own, which would be a path that resolves somewhere else.
    std::string path = base;
    std::size_t from = 0;
    while (from <= archiveFolder.size()) {
        const std::size_t slash = archiveFolder.find('/', from);
        const std::size_t end = (slash == std::string::npos) ? archiveFolder.size() : slash;
        if (end > from) {
            path += "/" + archiveFolder.substr(from, end - from);
            out.push_back(path);
        }
        if (slash == std::string::npos) break;
        from = slash + 1;
    }
    if (out.empty()) return out;

    out.push_back(path + "/" + rangeName);
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
inline std::string logTail(const std::string& text, size_t lines)
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

#endif // DHCP_CORE_LOGFILEFORMAT_H
