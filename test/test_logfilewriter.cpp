/**
 * @file test_logfilewriter.cpp
 * @brief Unit tests for the rotation every file log in this project uses, and for
 *        the two hooks that tell one log from another (stages 191 and 203).
 *
 * A live file on the internal volume, capped; when it is full, the operator's
 * rule is:
 *
 *   1. the file goes to the card as `<archive folder>/<first>-<last>/<file>`, and
 *      the live file starts again **empty**;
 *   2. with no card at all, the live file starts again with the **last ten
 *      lines** of the previous one;
 *   3. when the card is there but the write fails, the file is **kept** and the
 *      attempt is repeated on the next record, up to ten times with a line in
 *      `Errors.log` for each failure — after the tenth the writer gives up and
 *      carries the tail as if there were no card.
 *
 * Stage 203 moved that policy into the abstract `LogFileWriter`, with the NTP
 * request log and the DNS forwarded log as its two implementations. The first six
 * tests below are the ones this policy was accepted with and still drive it
 * through `NtpLogWriter` — unchanged expectations, which is what makes the
 * refactor provable. The last one drives it through a *second* subclass with
 * another archive folder and another name, so the two hooks are checked as hooks
 * and not merely as constants somebody reads in a header.
 *
 * All of that is file behaviour, so it is exercised here on real files in a
 * throwaway folder: the card is a second folder, its absence and its failure are
 * the two answers of the `cardMounted`/write callbacks the writer is built with.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_logfilewriter.cpp -o test_logfilewriter
 */
#include "src/core/NtpLogWriter.h"

#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#endif

using namespace std;

using dhcp::core::LogStamp;
using dhcp::core::NtpLogWriter;

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

const string kRoot = ".logfiletest";
const string kLive = kRoot + "/fat/logs/Ntp.log";
const string kArchive = kRoot + "/sdcard/log/Archive/Ntp/2026.10.12-2026.10.12/Ntp.log";

const LogStamp kRealStamp{2026, 10, 12, 19, 30, 0, true};
const LogStamp kNoClockStamp{1970, 1, 1, 0, 0, 5, false};

/**
 * @brief A second log of the same kind, as stage 205's DNS log will be one.
 *
 * It answers the two hooks differently and nothing else, which is the whole point
 * of the split: the rules about losing lines are not written twice.
 */
class OtherLogWriter : public dhcp::core::LogFileWriter {
public:
    using LogFileWriter::LogFileWriter;

protected:
    const char* archiveFolder() const override { return "log/Archive/Other"; }
    string logTitle() const override { return "Other log"; }
};

int makeDir(const string& path)
{
#if defined(_WIN32)
    return ::_mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0777);
#endif
}

bool isDirectory(const string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/** Remove a whole tree of files and folders (the test's own, nothing else). */
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

/** Read a file into a string; "" when it is not there (also answers the size). */
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

/** The text of one record: a fixed 50 bytes, no newline (the writer adds it). */
string longLine(int i)
{
    char num[8];
    snprintf(num, sizeof(num), "%04d", i);
    return "line " + string(num) + " " + string(40, 'x');
}

/** The same record as it appears in a file: 51 bytes with its newline. */
string longRecord(int i) { return longLine(i) + "\n"; }

/** A fixed 5-byte record, small enough that a tiny cap holds dozens of them. */
string shortLine(int i)
{
    char num[8];
    snprintf(num, sizeof(num), "%04d", i);
    return "l" + string(num);
}

/** The same short record as it appears in a file: 6 bytes with its newline. */
string shortRecord(int i) { return shortLine(i) + "\n"; }

/** The card as a folder that exists and accepts writes. */
string cardFolder() { return kRoot + "/sdcard"; }

/** The card as something that is there but cannot be written to (a plain file). */
string brokenCard() { return kRoot + "/card-that-is-a-file"; }

/** An empty live volume: the `/fat` mount point the caller is expected to create. */
void volume()
{
    removeTree(kRoot);
    makeDir(kRoot);
    makeDir(kRoot + "/fat");
}

NtpLogWriter makeWriter(size_t cap, bool cardThere, vector<string>* notices,
                        const string& cardRoot = string())
{
    const string card = cardRoot.empty() ? cardFolder() : cardRoot;
    return NtpLogWriter(kLive, card,
                        [cardThere] { return cardThere; },
                        [notices](const string& text) {
                            if (notices) notices->push_back(text);
                        },
                        cap);
}

} // namespace

static void test_writes_and_counts()
{
    volume();

    auto writer = makeWriter(64u * 1024u, true, nullptr);
    check(writer.append("first", kRealStamp), "a line goes into a fresh file");
    check(writer.append("second", kRealStamp), "and the next one after it");
    check(writer.lines() == 2, "the writer counts the lines of the live file");
    check(readFile(kLive) == "first\nsecond\n",
          "the live file holds the lines, one per line");
    check(isDirectory(kRoot + "/fat/logs"),
          "the log's folder was created inside the volume");
}

static void test_archives_to_the_card()
{
    volume();
    makeDir(cardFolder());

    vector<string> notices;
    // Cap 200: three 51-byte lines fit, the fourth does not.
    auto writer = makeWriter(200, true, &notices);

    writer.append(longLine(1), kRealStamp);
    writer.append(longLine(2), kRealStamp);
    writer.append(longLine(3), kRealStamp);
    const string before = readFile(kLive);
    check(before == longRecord(1) + longRecord(2) + longRecord(3),
          "the live file grows until the next line would not fit");
    check(writer.archives() == 0, "and nothing has been archived yet");

    writer.append(longLine(4), kRealStamp);
    check(writer.archives() == 1, "the line that does not fit triggers the archive");
    check(readFile(kArchive) == before,
          "the full file went to the card byte for byte, under its first-last date");
    check(readFile(kLive) == longRecord(4), "and the live file started again, empty");
    check(notices.empty(), "a successful archive needs no line in Errors.log");

    writer.append(longLine(5), kRealStamp);
    writer.append(longLine(6), kRealStamp);
    writer.append(longLine(7), kRealStamp);
    check(writer.archives() == 2, "a second rotation is a second archive");
    check(readFile(kArchive) == before + longRecord(4) + longRecord(5) + longRecord(6),
          "the second archive was appended to the file of the same range, not over it");
    check(readFile(kLive) == longRecord(7), "and the live file started again once more");
}

static void test_no_card_carries_the_tail()
{
    volume();

    vector<string> notices;
    // Cap 200 of 6-byte lines: 33 fit, the 34th rotates.
    auto writer = makeWriter(200, false, &notices);

    for (int i = 1; i <= 33; ++i) writer.append(shortLine(i), kRealStamp);
    const string before = readFile(kLive);
    check(before.size() == 33 * 6, "33 short lines fill the small cap");

    writer.append(shortLine(34), kRealStamp);
    const string after = readFile(kLive);

    check(writer.carryOvers() == 1,
          "the writer counts a rotation that could not be archived");
    check(after.size() < before.size(), "without a card the live file is recreated smaller");
    check(after == shortRecord(24) + shortRecord(25) + shortRecord(26) + shortRecord(27) +
                       shortRecord(28) + shortRecord(29) + shortRecord(30) + shortRecord(31) +
                       shortRecord(32) + shortRecord(33) + shortRecord(34),
          "exactly the last ten lines of the old file are carried, plus the new one");
    check(notices.empty(), "no card is not a failure, so nothing goes to Errors.log");
}

static void test_broken_card_keeps_the_file_then_gives_up()
{
    volume();
    FILE* stub = fopen(brokenCard().c_str(), "wb");
    check(stub != nullptr, "the test can create a card path that is not a folder");
    if (stub) fclose(stub);

    vector<string> notices;
    auto writer = makeWriter(200, true, &notices, brokenCard());

    for (int i = 1; i <= 3; ++i) writer.append(longLine(i), kRealStamp);
    writer.append(longLine(4), kRealStamp);   // the first failed attempt

    const string kept = readFile(kLive);
    check(kept == longRecord(1) + longRecord(2) + longRecord(3) + longRecord(4),
          "a failed archive keeps the file and everything in it");
    check(notices.size() == 1, "the first failure is reported once");

    for (int i = 5; i <= 12; ++i) writer.append(longLine(i), kRealStamp);
    const string beforeGiveUp = readFile(kLive);
    check(notices.size() == 9, "nine attempts so far, one per record");
    check(beforeGiveUp.find("line 0001") != string::npos,
          "and the file still holds every line, in order");

    writer.append(longLine(13), kRealStamp);   // the tenth failed attempt
    const string after = readFile(kLive);

    check(notices.size() == 10, "the tenth attempt is the last one");
    check(writer.carryOvers() == 1, "and it gives up, carrying the tail");
    check(writer.archives() == 0, "nothing was archived: the card never accepted the file");
    check(after.size() < beforeGiveUp.size(), "the live file was recreated after giving up");
    check(after.find("line 0001") == string::npos, "the lost part is really gone");
    check(after == longRecord(3) + longRecord(4) + longRecord(5) + longRecord(6) +
                      longRecord(7) + longRecord(8) + longRecord(9) + longRecord(10) +
                      longRecord(11) + longRecord(12) + longRecord(13),
          "the ten newest lines are carried into the new file, plus the line that got there");
}

static void test_a_working_card_later_is_used_again()
{
    volume();
    // The card's mount point is there, but as a file: every write to it fails,
    // which is what a card inserted the wrong way round looks like to the VFS.
    const string card = kRoot + "/card";
    FILE* stub = fopen(card.c_str(), "wb");
    if (stub) fclose(stub);

    vector<string> notices;
    auto writer = makeWriter(200, true, &notices, card);
    for (int i = 1; i <= 4; ++i) writer.append(longLine(i), kRealStamp);
    check(writer.archives() == 0, "nothing is archived while the card refuses the file");

    // The operator fixes the card: the same mount point becomes a working folder.
    remove(card.c_str());
    makeDir(card);
    check(writer.append(longLine(5), kRealStamp), "the writer keeps logging after a failure");
    check(writer.archives() == 1, "and the next full file is archived after all");
    check(readFile(card + "/log/Archive/Ntp/2026.10.12-2026.10.12/Ntp.log") ==
              longRecord(1) + longRecord(2) + longRecord(3) + longRecord(4),
          "with everything that was waiting in the live file");
}

static void test_wait_for_a_real_clock()
{
    volume();
    makeDir(cardFolder());

    vector<string> notices;
    // Cap 120: the third real line would not fit, so the rotation is due there.
    auto writer = makeWriter(120, true, &notices);

    // Lines written before the clock is set are logged (the operator's choice),
    // but a file of 1970 stamps must not name a folder on the card.
    writer.append(longLine(1), kNoClockStamp);
    writer.append(longLine(2), kNoClockStamp);
    writer.append(longLine(3), kNoClockStamp);
    check(writer.archives() == 0, "a file with no real time yet is not archived");
    check(readFile(kLive) == longRecord(1) + longRecord(2) + longRecord(3),
          "and it keeps every line while it waits");

    writer.append(longLine(4), kRealStamp);
    check(writer.archives() == 0, "the real line itself is written before the rotation is due");
    writer.append(longLine(5), kRealStamp);
    check(writer.archives() == 1, "once a real time is in the file, the next rotation archives it");
    check(readFile(kArchive) == longRecord(1) + longRecord(2) + longRecord(3) + longRecord(4),
          "the range folder is named after the first REAL date, and nothing earlier was dropped");
}

static void test_a_smaller_cap_rotates_there_and_then()
{
    volume();
    const string card = cardFolder();
    makeDir(card);

    auto writer = makeWriter(64u * 1024u, true, nullptr);
    for (int i = 1; i <= 5; ++i) writer.append(longLine(i), kRealStamp);
    const string before = readFile(kLive);
    check(writer.archives() == 0, "a file far below its cap has no archive yet");

    // The operator chooses a smaller size in the web interface (stage 204): the
    // file that is already there is over the new cap at that very moment.
    writer.setMaxBytes(100);
    check(writer.maxBytes() == 100, "the writer takes the chosen cap");
    check(writer.archives() == 1, "and the file that is already over it is archived right away");
    check(readFile(kArchive) == before, "the archive holds exactly what the file held");
    check(readFile(kLive).empty(), "and the live file starts again, empty");
}

static void test_a_larger_cap_keeps_the_file()
{
    volume();
    makeDir(cardFolder());

    // The operator asking for a bigger file is not asking to lose the one he has.
    auto writer = makeWriter(200, true, nullptr);
    for (int i = 1; i <= 3; ++i) writer.append(longLine(i), kRealStamp);
    const string kept = readFile(kLive);
    check(writer.archives() == 0, "a file below its cap has no archive yet");

    writer.setMaxBytes(64u * 1024u);
    check(writer.maxBytes() == 64u * 1024u, "the larger cap is taken");
    check(writer.archives() == 0 && writer.carryOvers() == 0,
          "and a larger cap rotates nothing");
    check(readFile(kLive) == kept, "the lines that were there are still there");

    // Saving the size that is already set is not a rotation either.
    writer.setMaxBytes(64u * 1024u);
    check(writer.maxBytes() == 64u * 1024u, "writing the same cap again keeps it");
    check(writer.archives() == 0 && writer.carryOvers() == 0,
          "and changes nothing about the file");
    check(readFile(kLive) == kept, "which still holds exactly its lines");
}

static void test_a_smaller_cap_with_no_card_keeps_the_newest_lines()
{
    volume();

    // No card: the operator's rule for a rotation that cannot be archived is the
    // newest ten lines, and it applies to a size change like to any other
    // rotation - the alternative would be a file that stays over its cap for
    // ever on a device with nothing to log.
    auto writer = makeWriter(64u * 1024u, false, nullptr);
    for (int i = 1; i <= 20; ++i) writer.append(shortLine(i), kRealStamp);
    writer.setMaxBytes(6);

    check(writer.carryOvers() == 1, "a size change with no card counts as a rotation");
    check(readFile(kLive) ==
              shortRecord(11) + shortRecord(12) + shortRecord(13) + shortRecord(14) +
                  shortRecord(15) + shortRecord(16) + shortRecord(17) + shortRecord(18) +
                  shortRecord(19) + shortRecord(20),
          "and the ten newest lines are what is left of the file");
}

static void test_the_subclass_names_its_own_log()
{
    volume();
    const string card = cardFolder();
    makeDir(card);

    const string live = kRoot + "/fat/logs/Other.log";
    const string archive = card + "/log/Archive/Other/2026.10.12-2026.10.12/Other.log";

    vector<string> notices;
    OtherLogWriter writer(live, card, [] { return true; },
                          [&notices](const string& text) { notices.push_back(text); },
                          200);
    for (int i = 1; i <= 4; ++i) writer.append(longLine(i), kRealStamp);

    check(readFile(archive) == longRecord(1) + longRecord(2) + longRecord(3),
          "a second subclass archives under its own folder on the card");
    check(readFile(live) == longRecord(4),
          "and its live file is the one it was built with, not the NTP one's");

    // The same file, now on a card that refuses writes: the failure line has to
    // name the log it belongs to, or the operator reads "NTP log archive failed"
    // about a file that is not Ntp.log.
    removeTree(card);
    FILE* stub = fopen(card.c_str(), "wb");
    if (stub) fclose(stub);

    for (int i = 5; i <= 8; ++i) writer.append(longLine(i), kRealStamp);
    check(!notices.empty(), "a full file on a broken card is reported");
    if (!notices.empty()) {
        check(notices[0].find("Other log archive failed") == 0,
              "the failure line names the log that failed: " + notices[0]);
        check(notices[0].find("log/Archive/Other/") != string::npos,
              "and says where it tried to put it");
    }
}

int main()
{
    printf("Running LogFileWriter tests...\n");
    test_writes_and_counts();
    test_archives_to_the_card();
    test_no_card_carries_the_tail();
    test_broken_card_keeps_the_file_then_gives_up();
    test_a_working_card_later_is_used_again();
    test_wait_for_a_real_clock();
    test_a_smaller_cap_rotates_there_and_then();
    test_a_larger_cap_keeps_the_file();
    test_a_smaller_cap_with_no_card_keeps_the_newest_lines();
    test_the_subclass_names_its_own_log();
    removeTree(kRoot);

    if (g_failed == 0) {
        printf("All %d LogFileWriter checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d LogFileWriter checks FAILED!\n", g_failed, g_checks);
    return 1;
}
