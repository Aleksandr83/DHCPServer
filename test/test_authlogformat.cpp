/**
 * @file test_authlogformat.cpp
 * @brief Unit tests for the authorisation log: what a line says, and where its
 *        archives go (stage 205).
 *
 * The operator asked for a switch on the Security page that keeps the login
 * attempts in a file, and for the three things that matter about them to be in it:
 * a refused password, a client that was then locked out, and the moment somebody
 * got in **after** failing — the last one being the line that answers "was that a
 * guest who forgot the password, or somebody who guessed it?".
 *
 * The sentences are built here, without the timestamp (which `ErrorLogCore` adds,
 * exactly as it does for the journal), because the text of a log line is a promise
 * about what the file will say and a host test is where a promise is checked. Two
 * of the checks are about arithmetic rather than words: the attempt counter names
 * the attempt the device counted (not the one before it), and the lockout line
 * carries the period the device will actually wait for.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_authlogformat.cpp -o test_authlogformat
 */
#include "src/core/AuthLogFormat.h"
#include "src/core/AuthLogWriter.h"
#include "src/core/ArchiveFileLogTarget.h"

#include <cstdio>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#endif

using namespace std;

using dhcp::core::ArchiveFileLogTarget;
using dhcp::core::authFailedText;
using dhcp::core::authLockoutText;
using dhcp::core::authLoginText;
using dhcp::core::authLogWritesSuccess;
using dhcp::core::AuthLogWriter;
using dhcp::core::LogLevel;
using dhcp::core::LogStamp;

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

const string kRoot = ".authlogtest";
const string kLive = kRoot + "/fat/logs/Auth.log";
const string kArchive = kRoot + "/sdcard/log/Archive/Auth/2026.10.12-2026.10.12/Auth.log";
const LogStamp kStamp{2026, 10, 12, 19, 22, 14, true};

/** One record, 50 bytes: the writer adds the newline, as it does for every log. */
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

} // namespace

static void test_a_refused_attempt()
{
    check(authFailedText("192.168.1.5", 1, 5) ==
              "failed login from 192.168.1.5 (1 of 5)",
          "the first refused password is logged with the attempt and the budget");
    check(authFailedText("192.168.1.5", 3, 5) ==
              "failed login from 192.168.1.5 (3 of 5)",
          "and the counter names the attempt the device counted, not the one before it");
    check(authFailedText("fe80::1", 5, 5) == "failed login from fe80::1 (5 of 5)",
          "an IPv6 client is named the way the socket reported it");
    check(authFailedText("192.168.1.5", 2, 1) == "failed login from 192.168.1.5 (2 of 1)",
          "a budget the operator lowered below the count prints as it is, without arithmetic");
}

static void test_a_lockout()
{
    check(authLockoutText("192.168.1.5", 300) == "locked out 192.168.1.5 for 300 s",
          "a lockout says who and for how long");
    check(authLockoutText("192.168.1.5", 1) == "locked out 192.168.1.5 for 1 s",
          "a one-second period is a period, not a plural or a silent zero");
    check(authLockoutText("10.0.0.7", 3600) == "locked out 10.0.0.7 for 3600 s",
          "the seconds are the plain number the setting holds");
}

static void test_getting_in_after_failing()
{
    check(authLoginText("192.168.1.5", 1) ==
              "logged in from 192.168.1.5 after 1 failed attempt",
          "one failed attempt before a success is one attempt");
    check(authLoginText("192.168.1.5", 4) ==
              "logged in from 192.168.1.5 after 4 failed attempts",
          "and more than one is attempts, plural");
    check(authLoginText("192.168.1.5", 0) == "logged in from 192.168.1.5",
          "a login with nothing before it is not dressed up as a recovery");
}

/**
 * Which successes reach the file (16.10.2026). The operator pointed out that a
 * password typed correctly on the first try into the login form was silent, and
 * the reason it was silent is the reason the rule needs a name: at the HTTP layer
 * every request carries the same credentials, so "the password was right" is the
 * normal state of a browsing session, not an event.
 */
static void test_which_successes_are_written()
{
    check(authLogWritesSuccess(true, 0),
          "a login through the form is written even when nothing failed before it");
    check(authLogWritesSuccess(true, 3),
          "and it is written after failures too - there is one line per login, "
          "not two");
    check(authLogWritesSuccess(false, 1),
          "an address that got in after failing is written, as stage 205 promised");
    check(authLogWritesSuccess(false, 5),
          "however many attempts it spent");

    // The line that must stay absent: every other request with correct credentials
    // - a page load is dozens of them, and a file of those is not a log of logins.
    check(!authLogWritesSuccess(false, 0),
          "a request that merely carries the password is not a login");
}

static void test_a_full_file_is_archived_under_its_own_folder()
{
    removeTree(kRoot);
    makeDir(kRoot);
    makeDir(kRoot + "/fat");
    makeDir(kRoot + "/sdcard");

    // Cap 200: three 51-byte lines fit, the fourth does not.
    auto writer = make_unique<AuthLogWriter>(kLive, kRoot + "/sdcard",
                                             [] { return true; }, nullptr, 200);
    ArchiveFileLogTarget target(move(writer), [] { return kStamp; });
    for (int i = 1; i <= 4; ++i) target.append(LogLevel::Warn, longLine(i));

    check(readFile(kArchive) == longRecord(1) + longRecord(2) + longRecord(3),
          "the auth log's archives live in log/Archive/Auth, its own folder on the card");
    check(readFile(kLive) == longRecord(4), "and its live file is the one it was built with");
    check(target.description() == kLive,
          "the target describes itself as the auth log's live file, not the error log's");
    removeTree(kRoot);
}

int main()
{
    printf("Running AuthLogFormat tests...\n");
    test_a_refused_attempt();
    test_a_lockout();
    test_getting_in_after_failing();
    test_which_successes_are_written();
    test_a_full_file_is_archived_under_its_own_folder();

    if (g_failed == 0) {
        printf("All %d AuthLogFormat checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d AuthLogFormat checks FAILED!\n", g_failed, g_checks);
    return 1;
}
