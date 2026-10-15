/**
 * @file test_ntplogformat.cpp
 * @brief Unit tests for what the NTP log says and for the names of its files
 *        (stages 191–193).
 *
 * The log is `/fat/logs/Ntp.log` on the internal volume, archived to the card as
 * `log/Archive/Ntp/<first date>-<last date>/Ntp.log`. What the file *does* when it
 * is full — the cap, the rotation, the carried tail — is shared with the DNS
 * forwarded log and is checked in `test_logfileformat.cpp` and
 * `test_logfilewriter.cpp`; this file checks what is about NTP itself: the names
 * the log uses, and the sentences a reader finds in it.
 *
 * Those sentences are the whole point of the file. A device that will not
 * synchronise has no console and often nobody standing next to it, so the lines
 * have to say which server it is waiting for, whether that server's name resolves
 * at all, and — when a client asks and the clock cannot be trusted — that it was
 * asked. Each of those is a promise about text the operator reads at a distance,
 * which is exactly what a host test can hold still.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_ntplogformat.cpp -o test_ntplogformat
 */
#include "src/core/NtpLogFormat.h"

#include <cstdio>
#include <string>

using namespace std;

using dhcp::core::kNtpLogArchiveFolder;
using dhcp::core::kNtpLogFileName;
using dhcp::core::kNtpLogNoClockMark;
using dhcp::core::LogStamp;
using dhcp::core::logStampPrefix;
using dhcp::core::NtpDnsVerdict;
using dhcp::core::ntpDnsVerdictText;
using dhcp::core::ntpLogClockShiftText;
using dhcp::core::ntpLogEventLine;
using dhcp::core::ntpLogLine;
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

static void test_names()
{
    check(string(kNtpLogFileName) == "Ntp.log", "the live file is Ntp.log");
    check(string(kNtpLogArchiveFolder) == "log/Archive/Ntp",
          "archives live in log/Archive/Ntp");
}

static void test_line_shape()
{
    const LogStamp real{2026, 10, 12, 19, 22, 14, true};
    const LogStamp noClock{1970, 1, 1, 0, 0, 5, false};

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

    // An event line is the same shape as a request line: the stamp comes from the
    // one shared rule, so the file has one format for its reader.
    check(ntpLogEventLine(real, "SNTP synchronised: pool.ntp.org (the clock was 3 s behind)") ==
              "[2026.10.12 19:22:14] SNTP synchronised: pool.ntp.org (the clock was 3 s behind)",
          "an event line is stamped exactly like a request, with the same prefix");
    check(ntpLogEventLine(real, "SNTP client started").find(logStampPrefix(real)) == 0,
          "and the prefix it uses is the shared one, not a copy of it");
}

static void test_sync_events()
{
    const LogStamp real{2026, 10, 12, 20, 14, 3, true};
    const LogStamp noClock{1970, 1, 1, 0, 0, 5, false};

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
    test_names();
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
