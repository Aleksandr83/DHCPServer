/**
 * @file test_archivefilelogtarget.cpp
 * @brief Unit tests for the seam that lets the error log and the journal use the
 *        common file-log policy (stage 206).
 *
 * `Errors.log` and `Jobs.log` are not written by a logger of their own: they come
 * out of `ErrorLogCore`, which formats a line and hands it to an
 * `IErrorLogTarget`. That seam already had two implementations (a file with a `.1`
 * rotation, and the in-memory list the host tests use), and it is the right place
 * to add the third: an **Adapter** that carries each line into a `core::LogFileWriter`,
 * so the two logs get the cap, the archive on the card and the carried tail the
 * NTP and DNS logs already have - and keep their queue, their task and their drop
 * counting untouched.
 *
 * Two things about the adapter are worth pinning here rather than on the device.
 * First, the line arrives **without** a stamp (the target is given text), while a
 * rotation needs the date of what is inside the file - so the adapter reads the
 * clock itself, at the moment of the write, and the test injects that clock: the
 * range folder of an archive is then a value the test knows, not a guess about
 * today. Second, a target of this kind must not swallow anything: a line that
 * cannot be stored has to come back as `false`, because that is what makes
 * `ErrorLogCore` count it as a drop and say so.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_archivefilelogtarget.cpp -o test_archivefilelogtarget
 */
#include "src/core/ArchiveFileLogTarget.h"
#include "src/core/ErrorsLogWriter.h"
#include "src/core/JobsLogWriter.h"

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
using dhcp::core::ErrorsLogWriter;
using dhcp::core::IErrorLogTarget;
using dhcp::core::JobsLogWriter;
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

const string kRoot = ".archivefilelogtest";
const string kFat = kRoot + "/fat";
const string kCard = kRoot + "/sdcard";
const string kLive = kFat + "/logs/Errors.log";
const string kArchive = kCard + "/log/Archive/Errors/2026.10.12-2026.10.12/Errors.log";

const LogStamp kStamp{2026, 10, 12, 19, 30, 0, true};
const LogStamp kNoClockStamp{1970, 1, 1, 0, 0, 5, false};

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

/** The text of one line: a fixed 50 bytes, no newline (the adapter's writer adds it). */
string longLine(int i)
{
    char num[8];
    snprintf(num, sizeof(num), "%04d", i);
    return "line " + string(num) + " " + string(40, 'x');
}

string longRecord(int i) { return longLine(i) + "\n"; }

/** A fixed 5-byte line, small enough that a tiny cap holds dozens of them. */
string shortLine(int i)
{
    char num[8];
    snprintf(num, sizeof(num), "%04d", i);
    return "l" + string(num);
}

string shortRecord(int i) { return shortLine(i) + "\n"; }

/** An empty internal volume; the caller creates the card when the test needs one. */
void volume()
{
    removeTree(kRoot);
    makeDir(kRoot);
    makeDir(kFat);
}

/** The adapter over the error log's writer, with a clock the test controls. */
unique_ptr<ArchiveFileLogTarget> makeTarget(size_t cap, bool cardThere,
                                            vector<string>* notices, LogStamp stamp,
                                            const string& cardRoot = string())
{
    const string card = cardRoot.empty() ? kCard : cardRoot;
    auto writer = make_unique<ErrorsLogWriter>(
        kLive, card,
        [cardThere] { return cardThere; },
        [notices](const string& text) {
            if (notices) notices->push_back(text);
        },
        cap);
    return make_unique<ArchiveFileLogTarget>(move(writer), [stamp] { return stamp; });
}

} // namespace

static void test_a_line_lands_in_the_live_file()
{
    volume();

    auto target = makeTarget(64u * 1024u, true, nullptr, kStamp);
    check(target->append(LogLevel::Error, "first line"), "a formatted line is stored");
    check(readFile(kLive) == "first line\n", "and it lands in the live file, with its newline");
    check(target->description() == kLive,
          "the target describes itself as the live file (the log asks it where it writes)");
    check(target->append(LogLevel::Warn, "second line"), "the next line is stored too");
    check(readFile(kLive) == "first line\nsecond line\n", "and the file grows, in order");
    check(target->writer().lines() == 2, "the writer counts what is in it");
}

static void test_a_full_file_is_archived_on_the_card()
{
    volume();
    makeDir(kCard);

    auto target = makeTarget(200, true, nullptr, kStamp);
    for (int i = 1; i <= 3; ++i) target->append(LogLevel::Error, longLine(i));
    check(target->writer().archives() == 0, "nothing is archived while the lines fit");

    target->append(LogLevel::Error, longLine(4));
    check(target->writer().archives() == 1,
          "the line that does not fit sends the file to the card");
    check(readFile(kArchive) == longRecord(1) + longRecord(2) + longRecord(3),
          "into log/Archive/Errors, under the dates of the lines inside it");
    check(readFile(kLive) == longRecord(4), "and the live file starts again, empty");
}

static void test_the_stamp_comes_from_the_clock_the_target_was_given()
{
    volume();
    makeDir(kCard);

    // The date of the range folder is the stamp the target asked for, and it is
    // the *writer* that decides a file with no real date may not name a folder:
    // a device whose clock was never set keeps growing its file instead.
    auto target = makeTarget(200, true, nullptr, kNoClockStamp);
    for (int i = 1; i <= 4; ++i) target->append(LogLevel::Error, longLine(i));

    check(target->writer().archives() == 0,
          "a file whose stamps carry no real date is not archived");
    check(readFile(kLive) ==
              longRecord(1) + longRecord(2) + longRecord(3) + longRecord(4),
          "and it keeps every line while it waits for a clock");
    check(readFile(kArchive).empty(), "nothing appeared on the card in the meantime");
}

static void test_no_card_keeps_the_newest_lines()
{
    volume();

    auto target = makeTarget(64u * 1024u, false, nullptr, kStamp);
    for (int i = 1; i <= 20; ++i) target->append(LogLevel::Error, shortLine(i));

    check(target->writer().maxBytes() == 64u * 1024u, "the cap is the one it was built with");
    target->writer().setMaxBytes(6);   // the operator picks a smaller size (stage 204)
    check(target->writer().carryOvers() == 1,
          "with no card, a size change that leaves the file over the cap is a rotation");
    check(readFile(kLive) ==
              shortRecord(11) + shortRecord(12) + shortRecord(13) + shortRecord(14) +
                  shortRecord(15) + shortRecord(16) + shortRecord(17) + shortRecord(18) +
                  shortRecord(19) + shortRecord(20),
          "and the ten newest lines are what is left of the error log");
}

static void test_a_refused_line_is_reported_upwards()
{
    volume();
    // The volume the live file lives on is a file, not a folder: every write has
    // to fail, and the caller has to hear about it - that is what makes
    // `ErrorLogCore` count the line as a drop instead of assuming it was stored.
    removeTree(kFat);
    FILE* stub = fopen(kFat.c_str(), "wb");
    check(stub != nullptr, "the test can make the log's volume unusable");
    if (stub) fclose(stub);

    auto target = makeTarget(64u * 1024u, true, nullptr, kStamp);
    check(!target->append(LogLevel::Error, "nowhere to put it"),
          "a line that cannot be stored comes back as a failure");
}

static void test_the_journal_names_itself_and_its_own_folder()
{
    volume();
    makeDir(kCard);

    const string live = kFat + "/logs/Jobs.log";
    const string jobsArchive = kCard + "/log/Archive/Jobs/2026.10.12-2026.10.12/Jobs.log";

    vector<string> notices;
    auto writer = make_unique<JobsLogWriter>(live, kCard, [] { return true; },
                                            [&notices](const string& text) {
                                                notices.push_back(text);
                                            },
                                            200);
    ArchiveFileLogTarget target(move(writer), [] { return kStamp; });
    for (int i = 1; i <= 4; ++i) target.append(LogLevel::Info, longLine(i));

    check(readFile(jobsArchive) == longRecord(1) + longRecord(2) + longRecord(3),
          "the journal's archive goes to log/Archive/Jobs, its own folder on the card");
    check(readFile(live) == longRecord(4), "and its live file is the one it was built with");

    // A card that refuses the file has to name the journal that failed: that line
    // is what the operator reads next to the error log's own.
    removeTree(kCard);
    FILE* stub = fopen(kCard.c_str(), "wb");
    if (stub) fclose(stub);
    for (int i = 5; i <= 8; ++i) target.append(LogLevel::Info, longLine(i));

    check(!notices.empty(), "a full file on a broken card is reported");
    if (!notices.empty()) {
        check(notices[0].find("Jobs log archive failed") == 0,
              "the line names the journal that failed: " + notices[0]);
        check(notices[0].find("log/Archive/Jobs/") != string::npos,
              "and says where it tried to put it");
    }
}

int main()
{
    printf("Running ArchiveFileLogTarget tests...\n");
    test_a_line_lands_in_the_live_file();
    test_a_full_file_is_archived_on_the_card();
    test_the_stamp_comes_from_the_clock_the_target_was_given();
    test_no_card_keeps_the_newest_lines();
    test_a_refused_line_is_reported_upwards();
    test_the_journal_names_itself_and_its_own_folder();
    removeTree(kRoot);

    if (g_failed == 0) {
        printf("All %d ArchiveFileLogTarget checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d ArchiveFileLogTarget checks FAILED!\n", g_failed, g_checks);
    return 1;
}
