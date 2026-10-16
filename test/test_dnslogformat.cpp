/**
 * @file test_dnslogformat.cpp
 * @brief Unit tests for the DNS server's file log: what a line says, and where a
 *        full file is archived (stage 204).
 *
 * The operator asked for the forwarded answers to be written to
 * `/fat/logs/Forwarded.log`, archived to the card the way the NTP request log
 * already is. A file like that is read when nobody is watching the terminal: it
 * has to be readable from the card, so a line carries the local time of the
 * answer, the record type, the name that was asked, what came back and who asked
 * (`[2026.10.12 19:22:14] DNS: [forward] A example.com -> 1.2.3.4 (from
 * 192.168.1.5 mac=aa:bb:cc:dd:ee:ff)`) - and it is exactly the sentence the
 * terminal shows, so the two destinations cannot drift apart.
 *
 * The last check is about the other half of the decision: which folder on the
 * operator's card a full file is copied into. That is one line of the subclass,
 * and it is the line that decides whether the archive is findable, so it is
 * driven here through real files in a throwaway folder.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_dnslogformat.cpp -o test_dnslogformat
 */
#include "src/dns/DnsLogFormat.h"
#include "src/dns/DnsForwardedLogWriter.h"

#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#endif

using namespace std;

using dhcp::core::kLogFileDefaultMaxBytes;
using dhcp::core::LogStamp;
using dhcp::dns::dnsForwardedLogLine;
using dhcp::dns::dnsLogQueryText;
using dhcp::dns::dnsLogOutcomeText;
using dhcp::dns::DnsQueryOutcome;
using dhcp::dns::dnsLogTypeText;
using dhcp::dns::DnsForwardedLogWriter;
using dhcp::dns::kDnsForwardedLogArchiveFolder;
using dhcp::dns::kDnsForwardedLogFileName;
using dhcp::dns::kDnsTypeA;
using dhcp::dns::kDnsTypeAaaa;
using dhcp::dns::kDnsTypeCname;
using dhcp::dns::kDnsTypeMx;

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

namespace {

const string kRoot = ".dnslogtest";
const string kLive = kRoot + "/fat/logs/Forwarded.log";
const string kArchive =
    kRoot + "/sdcard/log/Archive/Dns/2026.10.12-2026.10.12/Forwarded.log";
const LogStamp kRealStamp{2026, 10, 12, 19, 22, 14, true};
const LogStamp kNoClockStamp{1970, 1, 1, 0, 0, 5, false};

/** One record, 50 bytes, no newline: the writer adds the newline. */
string longLine(int i)
{
    char num[8];
    snprintf(num, sizeof(num), "%04d", i);
    return "line " + string(num) + " " + string(40, 'x');
}

string longRecord(int i) { return longLine(i) + "\n"; }

int makeDir(const string& path)
{
#if defined(_WIN32)
    return ::_mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0777);
#endif
}

void removeTree(const string& path)
{
    if (DIR* dir = opendir(path.c_str())) {
        vector<string> names;
        while (struct dirent* ent = readdir(dir)) {
            const string name = ent->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        closedir(dir);
        for (const auto& name : names) removeTree(path + "/" + name);
    }
    remove(path.c_str());
    rmdir(path.c_str());
}

string readFile(const string& path)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return string();
    string out;
    char buf[512];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), file)) > 0) out.append(buf, got);
    fclose(file);
    return out;
}

/** An empty live volume; the caller creates what else it needs. */
void volume()
{
    removeTree(kRoot);
    makeDir(kRoot);
    makeDir(kRoot + "/fat");
}

} // namespace

static void test_names()
{
    check(string(kDnsForwardedLogFileName) == "Forwarded.log",
          "the live file is Forwarded.log, as the operator named it");
    check(string(kDnsForwardedLogArchiveFolder) == "log/Archive/Dns",
          "and its archives live in log/Archive/Dns, beside the NTP log's");
}

static void test_type_column()
{
    check(dnsLogTypeText(kDnsTypeA) == "A", "an address record prints as A");
    check(dnsLogTypeText(kDnsTypeAaaa) == "AAAA", "an IPv6 address as AAAA");
    check(dnsLogTypeText(kDnsTypeMx) == "MX", "a mail exchanger as MX");
    check(dnsLogTypeText(kDnsTypeCname) == "CNAME", "an alias as CNAME");
    check(dnsLogTypeText(99) == "OTHER",
          "a type nobody named prints as OTHER instead of a number nobody reads");
}

static void test_the_terminal_sentence()
{
    // The body is built in one place for both destinations: the file line is this
    // sentence with a stamp in front of it.
    check(dnsLogQueryText("forward", kDnsTypeA, "example.com", DnsQueryOutcome::Answered,
                          "1.2.3.4", "192.168.1.5", "aa:bb:cc:dd:ee:ff") ==
              "[forward] A example.com -> 1.2.3.4 (from 192.168.1.5 mac=aa:bb:cc:dd:ee:ff)",
          "an answered query reads as the operator wrote it");
    check(dnsLogQueryText("forward", kDnsTypeAaaa, "example.com", DnsQueryOutcome::Answered,
                          "2001:db8::1", "192.168.1.5", "") ==
              "[forward] AAAA example.com -> 2001:db8::1 (from 192.168.1.5 mac=)",
          "an IPv6 answer travels whole, and an unknown MAC leaves the field empty");
    check(dnsLogQueryText("local", kDnsTypeA, "nas.lan", DnsQueryOutcome::Answered,
                          "192.168.0.5", "10.0.0.7", "") ==
              "[local] A nas.lan -> 192.168.0.5 (from 10.0.0.7 mac=)",
          "the tag still names where the answer came from");
}

/**
 * What the line says when there is no answer (16.10.2026). Every one of these
 * cases used to read `NXDOMAIN`, which is the one thing most of them are not: it
 * says "this name does not exist", and a client caches that for minutes. The
 * operator's log showed the cost - three queries answered `NXDOMAIN` by our own
 * forward timeout at 22:57, the same name answered with real addresses five
 * minutes later, and nothing in the file to tell the two apart.
 */
static void test_why_a_query_was_not_answered()
{
    // The one case that really is "no such name": the upstream said so, and the
    // client got that answer. It is logged as NXDOMAIN - and only now, because
    // until this stage a forwarded answer was logged as "(forwarded)" whatever its
    // return code, so a genuine NXDOMAIN was invisible.
    check(dnsLogOutcomeText(DnsQueryOutcome::Nxdomain, "") == "NXDOMAIN",
          "a name the upstream says does not exist says NXDOMAIN");
    check(dnsLogQueryText("forward", kDnsTypeA, "nowhere.lan", DnsQueryOutcome::Nxdomain,
                          "1.2.3.4", "10.0.0.7", "aa:bb:cc:dd:ee:ff") ==
              "[forward] A nowhere.lan -> NXDOMAIN (from 10.0.0.7 mac=aa:bb:cc:dd:ee:ff)",
          "and it is not the answer of the previous query");

    // Our own failures, each named for what it was - and each of the three that
    // ends with a fabricated NXDOMAIN says so as well, because that is what the
    // client was told.
    check(dnsLogOutcomeText(DnsQueryOutcome::Timeout, "") == "NXDOMAIN (timeout)",
          "an upstream that stayed silent says why the client got NXDOMAIN");
    check(dnsLogOutcomeText(DnsQueryOutcome::NoSlot, "") == "NXDOMAIN (no slot)",
          "a full forward table says so");
    check(dnsLogOutcomeText(DnsQueryOutcome::NoSocket, "") == "NXDOMAIN (no socket)",
          "a forward socket that would not open says so");
    check(dnsLogOutcomeText(DnsQueryOutcome::NoData, "") == "(no data)",
          "a name with no record of the asked type says what the client got (NODATA)");
    check(dnsLogOutcomeText(DnsQueryOutcome::Blocked, "") == "(blocked)",
          "a non-A/AAAA query answered NODATA on purpose says so");
    check(dnsLogQueryText("forward", kDnsTypeA, "nowhere.lan", DnsQueryOutcome::Timeout,
                          "", "10.0.0.7", "") ==
              "[forward] A nowhere.lan -> NXDOMAIN (timeout) (from 10.0.0.7 mac=)",
          "the sentence is built the same way for a failure");

    // A received answer keeps its source tag and its text: the marks above say why
    // there is no answer, they are not a replacement for the answer.
    check(dnsLogOutcomeText(DnsQueryOutcome::Answered, "(forwarded)") == "(forwarded)",
          "a forwarded answer still says (forwarded)");
}

static void test_the_file_line()
{
    const string body = dnsLogQueryText("forward", kDnsTypeA, "example.com",
                                        DnsQueryOutcome::Answered, "1.2.3.4",
                                        "192.168.1.5", "aa:bb:cc:dd:ee:ff");
    check(dnsForwardedLogLine(kRealStamp, body) ==
              "[2026.10.12 19:22:14] DNS: [forward] A example.com -> 1.2.3.4 "
              "(from 192.168.1.5 mac=aa:bb:cc:dd:ee:ff)",
          "the file line is the local time, then the same sentence the terminal shows");
    check(dnsForwardedLogLine(kNoClockStamp, body) ==
              "[----.--.-- --:--:--] DNS: [forward] A example.com -> 1.2.3.4 "
              "(from 192.168.1.5 mac=aa:bb:cc:dd:ee:ff)",
          "and a line written before the clock is set carries the placeholder");
    check(dnsForwardedLogLine(kRealStamp, body).find("DNS: ") != string::npos,
          "the destination is named in the line, so a file sent anywhere is readable");
}

static void test_a_full_file_is_archived_under_its_own_folder()
{
    volume();
    makeDir(kRoot + "/sdcard");

    // Cap 200: three 51-byte lines fit, the fourth does not.
    DnsForwardedLogWriter writer(kLive, kRoot + "/sdcard",
                                 [] { return true; }, {}, 200);
    for (int i = 1; i <= 3; ++i) writer.append(longLine(i), kRealStamp);
    check(writer.maxBytes() == 200, "the writer takes the cap it is built with");
    check(writer.archives() == 0, "nothing is archived while the lines fit");

    writer.append(longLine(4), kRealStamp);
    check(writer.archives() == 1, "the line that does not fit triggers the archive");
    check(readFile(kArchive) == longRecord(1) + longRecord(2) + longRecord(3),
          "and it lands in log/Archive/Dns, under the dates of its own lines");
    check(readFile(kLive) == longRecord(4), "while the live file starts again, empty");

    // The default cap of the combobox is what a writer gets when nobody chooses
    // one, and it is the same for this log as for the NTP log.
    DnsForwardedLogWriter defaulted(kLive, kRoot + "/sdcard", [] { return true; }, {});
    check(defaulted.maxBytes() == kLogFileDefaultMaxBytes,
          "a writer built without a size rotates at the default 64 KB");
}

int main()
{
    printf("Running DnsLogFormat tests...\n");
    test_names();
    test_type_column();
    test_the_terminal_sentence();
    test_why_a_query_was_not_answered();
    test_the_file_line();
    test_a_full_file_is_archived_under_its_own_folder();
    removeTree(kRoot);

    if (g_failed == 0) {
        printf("All %d DnsLogFormat checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d DnsLogFormat checks FAILED!\n", g_failed, g_checks);
    return 1;
}
