/**
 * @file test_sweepfilelogtarget.cpp
 * @brief Unit tests for the per-sweep auto-update log file (stage 186).
 *
 * `SweepFileLogTarget` creates `log/AutoUpdate/` on a volume, keeps the newest
 * files and writes one sweep into one file of its own — against the real host
 * filesystem, in a temporary directory of this test's own (the same approach as
 * `test_transferengine`, which also checks real files rather than a mock: the
 * properties that matter here are exactly the ones a mock would define away).
 *
 * What is checked:
 *   * the folder chain is created one level at a time (a single `mkdir` of
 *     `log/AutoUpdate` fails and would leave the log missing with no error);
 *   * lines written are in the file, in order, and flushed often enough that a
 *     restart cannot lose more than the batch in flight;
 *   * closing flushes the tail — the last lines of a sweep are the ones that say
 *     how it ended;
 *   * the retention rule removes the oldest **sweep** files and never a file of
 *     the operator's;
 *   * a name that already exists is appended to, not overwritten.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_sweepfilelogtarget.cpp src/core/SweepFileLogTarget.cpp \
 *       -o test_sweepfilelogtarget
 */
#include "src/core/SweepFileLogTarget.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <string>
#if defined(_WIN32)
#include <direct.h>   // _mkdir on MinGW
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "src/core/SweepLogFormat.h"

using namespace std;

using dhcp::core::kSweepLogKeep;
using dhcp::core::SweepFileLogTarget;
using dhcp::core::sweepLogName;

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

// ─── a temporary "volume" of our own ────────────────────────────────────────

static string tempBase()
{
    const char* env = getenv("TEMP");
    if (env == nullptr) env = getenv("TMP");
    const string base = (env != nullptr) ? env : ".";
    return base + "/dhcpserver_sweeplog_test";
}

static int makeDir(const string& path)
{
#if defined(_WIN32)
    return ::_mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0777);
#endif
}

static bool isDirectory(const string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/** Remove a tree of ours: only files and one level of folders are ever created. */
static void removeTree(const string& root)
{
    DIR* dir = opendir(root.c_str());
    if (dir != nullptr) {
        while (struct dirent* ent = readdir(dir)) {
            const string name = ent->d_name;
            if (name == "." || name == "..") continue;
            const string path = root + "/" + name;
            if (isDirectory(path)) removeTree(path);
            else remove(path.c_str());
        }
        closedir(dir);
    }
    remove(root.c_str());
}

static string readFile(const string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return {};
    string out;
    char buf[512];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    fclose(f);
    return out;
}

static vector<string> listDir(const string& path)
{
    vector<string> out;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) return out;
    while (struct dirent* ent = readdir(dir)) {
        const string name = ent->d_name;
        if (name != "." && name != "..") out.push_back(name);
    }
    closedir(dir);
    return out;
}

static bool contains(const vector<string>& names, const string& what)
{
    for (const auto& n : names) {
        if (n == what) return true;
    }
    return false;
}

static tm moment(int hour, int min)
{
    tm tmv{};
    tmv.tm_year = 2026 - 1900;
    tmv.tm_mon = 10 - 1;
    tmv.tm_mday = 11;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    return tmv;
}

// ─── tests ──────────────────────────────────────────────────────────────────

/** The folders are created one level at a time, on a volume that has none. */
static void test_open_creates_the_folder_chain()
{
    printf("the folders\n");
    const string root = tempBase();
    const string name = sweepLogName(moment(1, 34));

    SweepFileLogTarget target;
    check(target.open(root, name, kSweepLogKeep), "the file opens on an empty volume");
    check(isDirectory(root + "/log"), "the first level is there: <volume>/log");
    check(isDirectory(root + "/log/AutoUpdate"),
          "and the second: <volume>/log/AutoUpdate");
    check(target.description() == root + "/log/AutoUpdate/" + name,
          "the description is the file the operator will look for: " +
              target.description());
    target.close();
}

/** What is appended is in the file, in order. */
static void test_lines_land_in_the_file()
{
    printf("the lines\n");
    const string root = tempBase();
    const string name = sweepLogName(moment(2, 0));

    SweepFileLogTarget target;
    check(target.open(root, name, kSweepLogKeep), "the file opens");
    for (int i = 1; i <= 5; ++i) {
        check(target.append(dhcp::core::LogLevel::Info, "line " + to_string(i)),
              "append " + to_string(i));
    }
    target.close();

    const string text = readFile(root + "/log/AutoUpdate/" + name);
    check(text == "line 1\nline 2\nline 3\nline 4\nline 5\n",
          "the file holds the lines in order");
}

/** A crash inside a sweep may lose the batch in flight, never more. */
static void test_flushing_is_batched()
{
    printf("flushing\n");
    const string root = tempBase();
    const string name = sweepLogName(moment(3, 0));
    const string path = root + "/log/AutoUpdate/" + name;

    SweepFileLogTarget target;
    check(target.open(root, name, kSweepLogKeep), "the file opens");
    // One line short of the batch: nothing is guaranteed on the volume yet.
    for (uint32_t i = 1; i < SweepFileLogTarget::kFlushEveryLines; ++i) {
        target.append(dhcp::core::LogLevel::Info, "line " + to_string(i));
    }
    const size_t beforeBatch = readFile(path).size();
    // The line that completes the batch is flushed with it.
    target.append(dhcp::core::LogLevel::Info, "line batch");
    const size_t afterBatch = readFile(path).size();
    check(afterBatch > beforeBatch,
          "the batch is on the volume once it is full (" + to_string(beforeBatch) +
              " -> " + to_string(afterBatch) + " bytes)");

    // And the tail is not lost when the sweep ends.
    target.append(dhcp::core::LogLevel::Info, "the last line of the sweep");
    target.close();
    const string text = readFile(path);
    check(text.find("the last line of the sweep\n") != string::npos,
          "closing flushes the tail of the sweep");
    check(text.find("line batch\n") != string::npos,
          "and everything before it is still there");
    check(target.lines() == SweepFileLogTarget::kFlushEveryLines + 1,
          "the file counts the lines it wrote: " + to_string(target.lines()));
}

/** Nothing is written before a sweep, and nothing after it ends. */
static void test_closed_file_refuses_lines()
{
    printf("closed\n");
    const string root = tempBase();
    const string name = sweepLogName(moment(4, 0));

    SweepFileLogTarget target;
    check(!target.append(dhcp::core::LogLevel::Info, "too early"),
          "a line before open() is refused");
    check(target.open(root, name, kSweepLogKeep), "the file opens");
    check(target.isOpen(), "and says so");
    target.close();
    check(!target.isOpen(), "after close it says the opposite");
    check(!target.append(dhcp::core::LogLevel::Info, "too late"),
          "and refuses the line that would have gone nowhere");
    check(target.lines() == 0, "no line was counted");
}

/** The oldest sweeps go, the operator's own files stay. */
static void test_retention_leaves_foreign_files_alone()
{
    printf("retention\n");
    const string root = tempBase();
    const string folder = root + "/log/AutoUpdate";

    // four sweeps, then the operator drops two files of his own into the folder
    for (int hour = 1; hour <= 4; ++hour) {
        SweepFileLogTarget target;
        check(target.open(root, sweepLogName(moment(hour, 0)), kSweepLogKeep),
              "sweep " + to_string(hour) + " opens");
        target.append(dhcp::core::LogLevel::Info, "sweep " + to_string(hour));
        target.close();
    }
    FILE* mine = fopen((folder + "/notes.txt").c_str(), "wb");
    if (mine != nullptr) { fputs("mine", mine); fclose(mine); }
    FILE* neighbouring = fopen((folder + "/Errors.log").c_str(), "wb");
    if (neighbouring != nullptr) { fputs("neighbour", neighbouring); fclose(neighbouring); }

    // a fifth sweep with a limit of three removes the two oldest sweeps only
    SweepFileLogTarget target;
    check(target.open(root, sweepLogName(moment(5, 0)), 3), "the fifth sweep opens");
    target.close();

    const vector<string> left = listDir(folder);
    check(!contains(left, sweepLogName(moment(1, 0))), "the oldest sweep is gone");
    check(!contains(left, sweepLogName(moment(2, 0))), "so is the second");
    check(contains(left, sweepLogName(moment(3, 0))), "the third stays");
    check(contains(left, sweepLogName(moment(4, 0))), "the fourth stays");
    check(contains(left, sweepLogName(moment(5, 0))), "and the new one is there");
    check(contains(left, "notes.txt"), "a file of the operator's is untouched");
    check(contains(left, "Errors.log"), "so is a neighbouring log");
    check(left.size() == 5, "five entries are left, not more: " + to_string(left.size()));
}

/** Two sweeps inside one minute are two sweeps: the second appends. */
static void test_the_same_name_appends()
{
    printf("the same minute\n");
    const string root = tempBase();
    const string name = sweepLogName(moment(6, 15));

    SweepFileLogTarget first;
    check(first.open(root, name, kSweepLogKeep), "the first sweep opens");
    first.append(dhcp::core::LogLevel::Info, "first");
    first.close();

    SweepFileLogTarget second;
    check(second.open(root, name, kSweepLogKeep), "the second sweep opens the same name");
    second.append(dhcp::core::LogLevel::Info, "second");
    second.close();

    const string text = readFile(root + "/log/AutoUpdate/" + name);
    check(text == "first\nsecond\n",
          "both sweeps are in the file, neither replaced the other: " + text);
}

/** A volume that cannot be written is answered with false, not with a crash. */
static void test_a_volume_that_is_not_there()
{
    printf("a volume that is not there\n");
    SweepFileLogTarget target;
    check(!target.open("", sweepLogName(moment(7, 0)), kSweepLogKeep),
          "an empty mount point is refused");
    check(!target.open(tempBase(), "", kSweepLogKeep),
          "an empty file name is refused");
    check(!target.isOpen(), "nothing was opened");
}

int main()
{
    // A volume of our own: it exists (like a mount point does) and holds
    // nothing, so every folder below it has to be created by open().
    removeTree(tempBase());
    makeDir(tempBase());

    test_open_creates_the_folder_chain();
    test_lines_land_in_the_file();
    test_flushing_is_batched();
    test_closed_file_refuses_lines();
    test_retention_leaves_foreign_files_alone();
    test_the_same_name_appends();
    test_a_volume_that_is_not_there();

    removeTree(tempBase());   // never leave anything behind

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
