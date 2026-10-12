/**
 * @file test_ntplogformat.cpp
 * @brief Unit tests for the paths, the range folder name and the carried tail of
 *        the NTP request log (stage 191).
 *
 * The operator asked for one growing file on the internal volume
 * (`/fat/logs/Ntp.log`) that is, when it is full, sent to the microSD card into
 * `log/Archive/Ntp/<first date>-<last date>/Ntp.log`, and is then recreated -
 * empty when the archive went through, with the last ten lines when it did not.
 *
 * Every rule in `NtpLogFormat.h` is a promise about files that one run on the
 * device cannot check, and two of them are the ones that quietly destroy data if
 * they are wrong: the range name decides which folder on the operator's card the
 * log lands in, and the carried tail is all that is left of a full file when no
 * card is there. So both are checked against the near-misses (a one-digit month, a
 * tail that is not newline-terminated, a text shorter than the tail).
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_ntplogformat.cpp -o test_ntplogformat
 */
#include "src/core/NtpLogFormat.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace std;

using dhcp::core::kNtpLogArchiveFolder;
using dhcp::core::kNtpLogFileName;
using dhcp::core::kNtpLogKeepLines;
using dhcp::core::kNtpLogMaxArchiveAttempts;
using dhcp::core::kNtpLogMaxBytes;
using dhcp::core::kNtpLogNoClockMark;
using dhcp::core::kNtpLogNoClockPrefix;
using dhcp::core::kTmMonthBase;
using dhcp::core::kTmYearBase;
using dhcp::core::NtpDnsVerdict;
using dhcp::core::NtpLogDate;
using dhcp::core::NtpLogStamp;
using dhcp::core::ntpDnsVerdictText;
using dhcp::core::ntpLogClockShiftText;
using dhcp::core::ntpLogEventLine;
using dhcp::core::ntpLogFolderChain;
using dhcp::core::ntpLogLine;
using dhcp::core::ntpLogRangeName;
using dhcp::core::ntpLogStampFromLocal;
using dhcp::core::ntpLogStampPrefix;
using dhcp::core::ntpLogTail;
using dhcp::core::ntpResolverChainText;
using dhcp::core::ntpSyncStateDelaySec;
using dhcp::core::ntpSyncStateText;

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
    check(kNtpLogMaxBytes == 64u * 1024u, "the cap is the 64 KB the other logs use");
    check(kNtpLogKeepLines == 10, "ten lines are carried over");
    check(kNtpLogMaxArchiveAttempts == 10, "ten archive attempts before giving up");
    check(string(kNtpLogFileName) == "Ntp.log", "the live file is Ntp.log");
    check(string(kNtpLogArchiveFolder) == "log/Archive/Ntp", "archives live in log/Archive/Ntp");
}

static void test_range_name()
{
    const NtpLogDate a{2026, 10, 12};
    const NtpLogDate b{2026, 10, 12};
    check(ntpLogRangeName(a, b) == "2026.10.12-2026.10.12", "one day: the range is the same date twice");
    check(ntpLogRangeName(NtpLogDate{2026, 10, 11}, b) == "2026.10.11-2026.10.12",
          "two days: first-last");
    check(ntpLogRangeName(NtpLogDate{2025, 12, 31}, NtpLogDate{2026, 1, 1}) ==
              "2025.12.31-2026.01.01",
          "across the year: both years are written in full");
    check(ntpLogRangeName(NtpLogDate{2026, 1, 5}, NtpLogDate{2026, 1, 5}) ==
              "2026.01.05-2026.01.05",
          "single-digit fields are zero-padded, so the names sort as they happened");
}

static void test_folder_chain()
{
    const auto chain = ntpLogFolderChain("/sdcard", "2026.10.11-2026.10.12");
    const vector<string> wanted{
        "/sdcard/log",
        "/sdcard/log/Archive",
        "/sdcard/log/Archive/Ntp",
        "/sdcard/log/Archive/Ntp/2026.10.11-2026.10.12",
    };
    check(chain == wanted, "the whole chain is built one level at a time: " + join(chain));

    check(ntpLogFolderChain("/sdcard/", "2026.10.11-2026.10.12") == wanted,
          "a trailing slash on the mount point changes nothing");
    check(ntpLogFolderChain("", "2026.10.11-2026.10.12").empty(),
          "no volume means no chain at all, not a chain starting at '/'");
    check(ntpLogFolderChain("/sdcard", "").empty(),
          "no range name means nowhere to put the file");
}

static void test_tail()
{
    check(ntpLogTail("", 10).empty(), "an empty file carries nothing");
    check(ntpLogTail("one\n", 10) == "one\n", "a short file is carried whole");
    check(ntpLogTail("a\nb\nc\n", 2) == "b\nc\n", "the last two of three lines");
    check(ntpLogTail("a\nb\nc\n", 3) == "a\nb\nc\n", "asking for exactly what is there");
    check(ntpLogTail("a\nb\nc", 2) == "b\nc\n",
          "a file whose last line has no newline still yields whole lines");
    check(ntpLogTail("a\n\nb\n", 2) == "\nb\n", "an empty line is a line");
    check(ntpLogTail("a\nb\n", 0).empty(), "zero means nothing is carried");
}

static void test_line_shape()
{
    const NtpLogStamp real{2026, 10, 12, 19, 22, 14, true};
    const NtpLogStamp noClock{1970, 1, 1, 0, 0, 5, false};

    check(ntpLogStampPrefix(real) == "[2026.10.12 19:22:14]",
          "a stamp is dotted, day-first and carries the seconds");
    check(ntpLogStampPrefix(noClock) == std::string(kNtpLogNoClockPrefix),
          "no clock means the placeholder, not a 1970 date that never happened");
    check(ntpLogStampPrefix(noClock) == "[----.--.-- --:--:--]",
          "the placeholder is the shape the operator asked for");
    check(ntpLogStampPrefix(real).size() == ntpLogStampPrefix(noClock).size(),
          "both are the same width, so a file lines up before and after the first sync");
    check(ntpLogStampPrefix(real).size() == 21, "and that width is 21 characters");

    check(ntpLogLine(real, "192.168.1.5", 3) ==
              "[2026.10.12 19:22:14] NTP request from 192.168.1.5 -> stratum 3",
          "the whole line reads as the operator wrote it");
    check(ntpLogLine(noClock, "10.0.0.7", 3) ==
              "[----.--.-- --:--:--] NTP request from 10.0.0.7 -> stratum 3",
          "a line written before the first sync says so in the same place");

    // A request answered with "the clock is not trustworthy" is logged all the
    // same — it is the proof that clients ask at all — and it carries both the
    // stratum the client was told (16) and the mark.
    check(ntpLogLine(noClock, "10.0.0.7", 16, false) ==
              "[----.--.-- --:--:--] NTP request from 10.0.0.7 -> stratum 16 (clock not verified)",
          "an unverified reply is logged with the stratum the client saw and a mark");
    check(ntpLogLine(real, "10.0.0.7", 3, false).find(kNtpLogNoClockMark) != string::npos,
          "the mark depends on the verdict, not on the stamp");
    check(ntpLogLine(real, "10.0.0.7", 3).find(kNtpLogNoClockMark) == string::npos,
          "and a verified reply is not marked");

    // The stamp is built from the local time the writer read, so the test checks
    // the conversion the device does and not only the printing.
    std::tm local{};
    local.tm_year = 2026 - kTmYearBase;
    local.tm_mon = 10 - kTmMonthBase;
    local.tm_mday = 12;
    local.tm_hour = 19;
    local.tm_min = 22;
    local.tm_sec = 14;
    check(ntpLogStampPrefix(ntpLogStampFromLocal(local, true)) == "[2026.10.12 19:22:14]",
          "a local time becomes the stamp of a line");
    check(ntpLogStampPrefix(ntpLogStampFromLocal(local, false)) ==
              std::string(kNtpLogNoClockPrefix),
          "and without a clock the same call yields the placeholder, whatever tm holds");
}

static void test_sync_events()
{
    const NtpLogStamp real{2026, 10, 12, 20, 14, 3, true};
    const NtpLogStamp noClock{1970, 1, 1, 0, 0, 5, false};

    // The schedule the operator chose for "still not synchronised": at most three
    // lines a minute apart, then 2, 5 and 10 minutes, and then every 10 minutes —
    // a device that never synchronises must keep saying so without filling the
    // volume it lives on.
    check(ntpSyncStateDelaySec(0) == 60, "the first line comes a minute after the client starts");
    check(ntpSyncStateDelaySec(1) == 60, "the second a minute later");
    check(ntpSyncStateDelaySec(2) == 60, "and the third: three lines a minute apart at most");
    check(ntpSyncStateDelaySec(3) == 120, "then every two minutes");
    check(ntpSyncStateDelaySec(4) == 300, "then every five");
    check(ntpSyncStateDelaySec(5) == 600, "then every ten");
    check(ntpSyncStateDelaySec(6) == 600, "and it stays at ten minutes");
    check(ntpSyncStateDelaySec(99) == 600, "however long the device waits for a reply");

    // The shift in the success line is an estimate from the last reading of the
    // clock, and both directions have to read the right way round: comparing them
    // the other way would tell the operator the opposite of what happened.
    check(ntpLogClockShiftText(1000, 10, 1008, 15, true) == "the clock was 3 s behind",
          "a clock that was slow is reported as behind");
    check(ntpLogClockShiftText(1000, 10, 1002, 15, true) == "the clock was 3 s ahead",
          "and a fast one as ahead");
    check(ntpLogClockShiftText(1000, 10, 1005, 15, true) == "the clock was already right",
          "a clock that agreed says so instead of claiming a shift of zero seconds");
    check(ntpLogClockShiftText(0, 10, 1789000000, 15, false) ==
              "the clock had no real time yet",
          "with no real clock to compare against there is no estimate to make");
    check(ntpLogClockShiftText(1000, 10, 1008, 15, true).find("behind") != string::npos,
          "the wording the log uses is the one the operator reads");

    // An event line is the same shape as a request line, minus the request.
    check(ntpLogEventLine(real, "SNTP synchronised: pool.ntp.org (the clock was 3 s behind)") ==
              "[2026.10.12 20:14:03] SNTP synchronised: pool.ntp.org (the clock was 3 s behind)",
          "an event is stamped exactly like a request");
    check(ntpLogEventLine(noClock, "SNTP client started: pool.ntp.org, interval 86400 s") ==
              "[----.--.-- --:--:--] SNTP client started: pool.ntp.org, interval 86400 s",
          "and an event before the first synchronisation carries the placeholder");
}

static void test_resolver_diagnostics()
{
    // The chain the device resolves through, said out loud at every client start:
    // the device asks itself, so the interesting part is what it forwards to.
    check(ntpResolverChainText("192.168.0.10", "192.168.1.1", "192.168.0.1") ==
              "resolver: this device (192.168.0.10), upstream 192.168.1.1, gateway 192.168.0.1",
          "the resolver chain names the device, its upstream and the gateway");
    check(ntpResolverChainText("192.168.0.10", "", "") ==
              "resolver: this device (192.168.0.10), upstream not set, gateway not set",
          "a setting that was never filled in says so instead of printing an empty slot");

    // The verdict of resolving the NTP server's own name — the step that, when it
    // fails, explains "no confirmed reply" without any further digging.
    check(ntpDnsVerdictText(NtpDnsVerdict::Ok, "5.61.32.14") == "DNS -> 5.61.32.14",
          "a resolved name is reported with the address the client would use");
    check(ntpDnsVerdictText(NtpDnsVerdict::Failed) == "DNS lookup failed",
          "and a name that did not resolve says exactly that");
    check(ntpDnsVerdictText(NtpDnsVerdict::Pending) == "DNS lookup still pending",
          "a resolution that is taking longer than the wait says so too");

    check(ntpSyncStateText(60, "pool.ntp.org", ntpDnsVerdictText(NtpDnsVerdict::Ok, "5.61.32.14")) ==
              "still not synchronised: 60 s since the client started, server=pool.ntp.org, "
              "no confirmed reply yet (DNS -> 5.61.32.14)",
          "the state line carries the verdict, so one line explains the whole situation");
    check(ntpSyncStateText(660, "pool.ntp.org", ntpDnsVerdictText(NtpDnsVerdict::Failed)) ==
              "still not synchronised: 660 s since the client started, server=pool.ntp.org, "
              "no confirmed reply yet (DNS lookup failed)",
          "and the same line is what a device with a broken upstream writes");
}

int main()
{
    printf("Running NtpLogFormat tests...\n");
    test_limits();
    test_range_name();
    test_folder_chain();
    test_tail();
    test_line_shape();
    test_sync_events();
    test_resolver_diagnostics();

    if (g_failed == 0) {
        printf("All %d NtpLogFormat checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d NtpLogFormat checks FAILED!\n", g_failed, g_checks);
    return 1;
}
