#ifndef DHCP_CORE_SWEEPLOGFORMAT_H
#define DHCP_CORE_SWEEPLOGFORMAT_H

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace dhcp {
namespace core {

/**
 * @file SweepLogFormat.h
 * @brief Where a cache-auto-update sweep writes its record-by-record log, and
 *        what the folder looks like afterwards (stage 186).
 *
 * The operator asked for the detailed log of a sweep to be one file per sweep,
 * on the microSD card, in `log/AutoUpdate/`, named after the moment the sweep
 * started — `2026.10.11_01.34.log` — so that a whole sweep (tens of thousands of
 * lines) survives in one piece instead of being cut down to the last few hundred
 * lines by the 64 KB cap the other logs live with.
 *
 * Three rules here are the ones worth a test, because each is a promise about
 * files that no single device run can check:
 *
 *   * **the name is local time** (`yyyy.mm.dd_hh.mm.log`), so it agrees with the
 *     stamps inside the file and with the clock on the device's own pages;
 *   * **the folder chain is created by the writer**, and the names of the
 *     directories are the ones in this header — a typo here and the file silently
 *     goes nowhere;
 *   * **only the newest @ref kSweepLogKeep files are kept**, and a name that is
 *     not a sweep log (something the operator copied into the folder, a stray file
 *     from another feature) is never deleted.
 *
 * The order of the names is chronologically meaningful without parsing: this
 * format sorts lexicographically exactly as it sorts in time (zero-padded fields,
 * largest first), which is what makes "keep the newest N" a comparison of names.
 *
 * Free of ESP-IDF on purpose — host-tested in `test/test_sweeplogformat.cpp`.
 */

/** @brief Folder of the per-sweep logs, relative to the mount point. */
constexpr const char* kSweepLogFolder = "log/AutoUpdate";

/** @brief How many per-sweep files are kept; the oldest go at the next sweep. */
constexpr size_t kSweepLogKeep = 10;

/** @brief Suffix of every file this feature writes and owns. */
constexpr const char* kSweepLogSuffix = ".log";

/**
 * @brief `2026.10.11_01.34.log` from the local time a sweep started.
 *
 * The fields are zero-padded so that the names sort as they happened — the
 * retention rule below depends on it and does not parse anything.
 *
 * @param local Local time of the start (already the operator's zone: the sweep
 *              refuses to write a file at all when the clock is not set).
 */
inline std::string sweepLogName(const std::tm& local)
{
    // 80 bytes, the size the time server uses for the same job: the format's own
    // fields need 21, and the compiler's warning about `%04d` is about an `int`
    // that would never be a year — a buffer that cannot be overrun is cheaper than
    // an argument about it (the build treats this warning as an error).
    char buf[80];
    snprintf(buf, sizeof(buf), "%04d.%02d.%02d_%02d.%02d%s",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, kSweepLogSuffix);
    return std::string(buf);
}

/**
 * @brief True when @p name is a file of this feature.
 *
 * Strict on the shape (`2026.10.11_01.34.log`) rather than on a prefix: the file
 * name carries no feature word — the folder does — so the only thing that says
 * "this is mine" is the pattern. Anything else in the folder belongs to the
 * operator or to another feature, and the retention rule must not touch it.
 */
inline bool isSweepLogName(const std::string& name)
{
    // 4 digits, '.', 2, '.', 2, '_', 2, '.', 2, then ".log"
    const size_t digits[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15};
    const size_t expected = 20;
    if (name.size() != expected) return false;
    if (name[4] != '.' || name[7] != '.' || name[10] != '_' || name[13] != '.') {
        return false;
    }
    for (size_t i = 0; i < sizeof(digits) / sizeof(digits[0]); ++i) {
        const char c = name[digits[i]];
        if (c < '0' || c > '9') return false;
    }
    return name.compare(16, 4, kSweepLogSuffix) == 0;
}

/**
 * @brief The directories to create for the sweep logs, outermost first.
 *
 * One level at a time: `mkdir` in the VFS (and on the device) refuses a path
 * whose parent is missing, and a single call for `log/AutoUpdate` would quietly
 * create nothing — which is how a log ends up missing with no error anywhere.
 *
 * @param mountPoint e.g. `/sdcard` (a trailing slash is ignored).
 * @return e.g. `{"/sdcard/log", "/sdcard/log/AutoUpdate"}`; empty for an empty
 *         mount point, because a log with no volume to live on is no log.
 */
inline std::vector<std::string> sweepLogFolderChain(const std::string& mountPoint)
{
    std::vector<std::string> out;
    if (mountPoint.empty()) return out;

    std::string base = mountPoint;
    if (base.back() == '/') base.pop_back();
    if (base.empty()) return out;

    out.push_back(base + "/log");
    out.push_back(base + "/" + kSweepLogFolder);
    return out;
}

/**
 * @brief Which of the names in the folder have to go, oldest first.
 *
 * @param names The folder's entries, as the volume reports them.
 * @param keep  How many of the newest sweep logs are kept.
 * @return The names to remove. A name that is not a sweep log is never returned,
 *         and a folder that holds fewer than @p keep of them has nothing to
 *         remove.
 */
inline std::vector<std::string> sweepLogFilesToRemove(
    const std::vector<std::string>& names, size_t keep)
{
    std::vector<std::string> ours;
    for (const auto& name : names) {
        if (isSweepLogName(name)) ours.push_back(name);
    }
    // The format sorts as it happened, so the oldest are at the front.
    std::sort(ours.begin(), ours.end());

    std::vector<std::string> out;
    if (ours.size() <= keep) return out;
    const size_t removing = ours.size() - keep;
    for (size_t i = 0; i < removing; ++i) out.push_back(ours[i]);
    return out;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_SWEEPLOGFORMAT_H
