/**
 * @file test_logfileformat.cpp
 * @brief Unit tests for what every file log in this project shares: its cap, the
 *        name of a date range, the folders of an archive and the carried tail
 *        (stages 191 and 203).
 *
 * Two logs live on the internal volume and are archived to the microSD card when
 * they are full: the NTP request log (`/fat/logs/Ntp.log` →
 * `log/Archive/Ntp/<first date>-<last date>/Ntp.log`) and the DNS forwarded log
 * (`/fat/logs/Forwarded.log` → `log/Archive/Dns/<first date>-<last date>/`,
 * stage 205). Stage 203 moved the rules they have in common into
 * `LogFileFormat.h` and the policy that uses them into the abstract
 * `LogFileWriter`; this file holds the checks that are true of *either* log, and
 * `test_ntplogformat.cpp` keeps the ones that are about the NTP lines.
 *
 * Two of these rules quietly destroy data if they are wrong: the range name
 * decides which folder on the operator's card the log lands in, and the carried
 * tail is all that is left of a full file when no card is there. So both are
 * checked against the near-misses (a one-digit month, a tail that is not
 * newline-terminated, a text shorter than the tail) and the folder chain is
 * checked for **two** different archive folders — the one thing a shared rule
 * must get right.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_logfileformat.cpp -o test_logfileformat
 */
#include "src/core/LogFileFormat.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace std;

using dhcp::core::kLogFileDefaultMaxBytes;
using dhcp::core::kLogFileKeepLines;
using dhcp::core::kLogFileLiveFolder;
using dhcp::core::kLogFileMaxArchiveAttempts;
using dhcp::core::kLogNoClockPrefix;
using dhcp::core::kTmMonthBase;
using dhcp::core::kTmYearBase;
using dhcp::core::LogDate;
using dhcp::core::logFolderChain;
using dhcp::core::logRangeName;
using dhcp::core::LogStamp;
using dhcp::core::logStampFromLocal;
using dhcp::core::logStampPrefix;
using dhcp::core::logTail;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const string& what)
{
    ++g_checks;
    if (ok) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        ++g_failed;
    }
}

static string join(const vector<string>& parts)
{
    string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += " | ";
        out += part;
    }
    return out;
}

static void test_limits()
{
    // The cap is the one Errors.log lives with, so "full" means one thing in the
    // whole project; the tail is the operator's ten lines, and ten archive
    // attempts is the point where the writer stops waiting for the card.
    check(kLogFileDefaultMaxBytes == 64u * 1024u, "the default cap is the 64 KB the other logs use");
    check(string(kLogFileLiveFolder) == "logs",
          "every live log sits in logs/ of its volume, so a reader finds them together");
    check(kLogFileKeepLines == 10, "ten lines are carried over");
    check(kLogFileMaxArchiveAttempts == 10, "ten archive attempts before giving up");
    check(string(kLogNoClockPrefix).size() == 21,
          "the placeholder is 21 characters wide, the width of a real stamp");
}

static void test_range_name()
{
    const LogDate a{2026, 10, 12};
    const LogDate b{2026, 10, 12};
    check(logRangeName(a, b) == "2026.10.12-2026.10.12", "one day: the range is the same date twice");
    check(logRangeName(LogDate{2026, 10, 11}, b) == "2026.10.11-2026.10.12",
          "two days: first-last");
    check(logRangeName(LogDate{2025, 12, 31}, LogDate{2026, 1, 1}) ==
              "2025.12.31-2026.01.01",
          "across the year: both years are written in full");
    check(logRangeName(LogDate{2026, 1, 5}, LogDate{2026, 1, 5}) ==
              "2026.01.05-2026.01.05",
          "single-digit fields are zero-padded, so the names sort as they happened");
}

static void test_folder_chain()
{
    // The NTP log's chain — byte for byte what the writer built for it before the
    // two logs shared this code, which is what makes the refactor of stage 203
    // provable rather than merely plausible.
    const auto chain = logFolderChain("/sdcard", "log/Archive/Ntp", "2026.10.11-2026.10.12");
    const vector<string> wanted{
        "/sdcard/log",
        "/sdcard/log/Archive",
        "/sdcard/log/Archive/Ntp",
        "/sdcard/log/Archive/Ntp/2026.10.11-2026.10.12",
    };
    check(chain == wanted, "the whole chain is built one level at a time: " + join(chain));

    // The second log's chain: the *same* code, a different folder. A shared rule
    // that only works for the folder it was written for is not shared.
    const auto dns = logFolderChain("/sdcard", "log/Archive/Dns", "2026.10.11-2026.10.12");
    const vector<string> dnsWanted{
        "/sdcard/log",
        "/sdcard/log/Archive",
        "/sdcard/log/Archive/Dns",
        "/sdcard/log/Archive/Dns/2026.10.11-2026.10.12",
    };
    check(dns == dnsWanted, "the second log's chain differs only in its folder: " + join(dns));

    const auto shallow = logFolderChain("/sdcard", "Archive", "2026.10.11-2026.10.12");
    const vector<string> shallowWanted{
        "/sdcard/Archive",
        "/sdcard/Archive/2026.10.11-2026.10.12",
    };
    check(shallow == shallowWanted,
          "a one-level archive folder makes a two-level chain, not a chain with empty levels");

    check(logFolderChain("/sdcard/", "log/Archive/Ntp", "2026.10.11-2026.10.12") == wanted,
          "a trailing slash on the mount point changes nothing");
    check(logFolderChain("", "log/Archive/Ntp", "2026.10.11-2026.10.12").empty(),
          "no volume means no chain at all, not a chain starting at '/'");
    check(logFolderChain("/sdcard", "log/Archive/Ntp", "").empty(),
          "no range name means nowhere to put the file");
    check(logFolderChain("/sdcard", "", "2026.10.11-2026.10.12").empty(),
          "and no archive folder means there is no folder to create either");
}

static void test_tail()
{
    check(logTail("", 10).empty(), "an empty file carries nothing");
    check(logTail("one\n", 10) == "one\n", "a short file is carried whole");
    check(logTail("a\nb\nc\n", 2) == "b\nc\n", "the last two of three lines");
    check(logTail("a\nb\nc\n", 3) == "a\nb\nc\n", "asking for exactly what is there");
    check(logTail("a\nb\nc", 2) == "b\nc\n",
          "a file whose last line has no newline still yields whole lines");
    check(logTail("a\n\nb\n", 2) == "\nb\n", "an empty line is a line");
    check(logTail("a\nb\n", 0).empty(), "zero means nothing is carried");
}

static void test_stamp()
{
    const LogStamp real{2026, 10, 12, 19, 22, 14, true};
    const LogStamp noClock{1970, 1, 1, 0, 0, 5, false};

    check(logStampPrefix(real) == "[2026.10.12 19:22:14]",
          "a stamp is dotted, day-first and carries the seconds");
    check(logStampPrefix(noClock) == string(kLogNoClockPrefix),
          "no clock means the placeholder, not a 1970 date that never happened");
    check(logStampPrefix(noClock) == "[----.--.-- --:--:--]",
          "the placeholder is the shape the operator asked for");
    check(logStampPrefix(real).size() == logStampPrefix(noClock).size(),
          "both are the same width, so a file lines up before and after the first sync");

    // The stamp is built from the local time the writer read, so the test checks
    // the conversion the device does and not only the printing.
    std::tm local{};
    local.tm_year = 2026 - kTmYearBase;
    local.tm_mon = 10 - kTmMonthBase;
    local.tm_mday = 12;
    local.tm_hour = 19;
    local.tm_min = 22;
    local.tm_sec = 14;
    check(logStampPrefix(logStampFromLocal(local, true)) == "[2026.10.12 19:22:14]",
          "a local time becomes the stamp of a line");
    check(logStampPrefix(logStampFromLocal(local, false)) == string(kLogNoClockPrefix),
          "and without a clock the same call yields the placeholder, whatever tm holds");
}

int main()
{
    printf("Running LogFileFormat tests...\n");
    test_limits();
    test_range_name();
    test_folder_chain();
    test_tail();
    test_stamp();

    if (g_failed == 0) {
        printf("All %d LogFileFormat checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d LogFileFormat checks FAILED!\n", g_failed, g_checks);
    return 1;
}
