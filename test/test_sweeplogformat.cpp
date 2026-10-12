/**
 * @file test_sweeplogformat.cpp
 * @brief Unit tests for the name, the folder chain and the retention rule of the
 *        per-sweep auto-update log (stage 186).
 *
 * The operator asked for one file per sweep on the microSD card, named after the
 * moment the sweep started (`log/AutoUpdate/2026.10.11_01.34.log`). All three
 * rules in `SweepLogFormat.h` decide something about files that a single run on
 * the device cannot check: the name is local time and sorts as it happened, the
 * folder chain is created one level at a time, and the retention rule deletes
 * **only** the files this feature wrote.
 *
 * The last one is the dangerous one — a mistake there deletes somebody else's
 * file off the operator's card — so it is checked against names that are nearly
 * right (a different suffix, a missing digit, a shorter year, a name from the
 * file manager) as well as against the real ones.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_sweeplogformat.cpp -o test_sweeplogformat
 */
#include "src/core/SweepLogFormat.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace std;

using dhcp::core::isSweepLogName;
using dhcp::core::kSweepLogFolder;
using dhcp::core::kSweepLogKeep;
using dhcp::core::sweepLogFilesToRemove;
using dhcp::core::sweepLogFolderChain;
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

static string join(const vector<string>& names)
{
    string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    return out;
}

static tm localTime(int year, int mon, int day, int hour, int min)
{
    tm tmv{};
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = 30;   // never part of the name: a sweep is not named by its second
    return tmv;
}

/** The name the operator asked for, with every field zero-padded. */
static void test_the_name()
{
    printf("the name\n");
    check(sweepLogName(localTime(2026, 10, 11, 1, 34)) == "2026.10.11_01.34.log",
          "2026-10-11 01:34 -> 2026.10.11_01.34.log: " +
              sweepLogName(localTime(2026, 10, 11, 1, 34)));
    check(sweepLogName(localTime(2026, 1, 2, 3, 4)) == "2026.01.02_03.04.log",
          "single digits are padded: " + sweepLogName(localTime(2026, 1, 2, 3, 4)));
    check(sweepLogName(localTime(2026, 12, 31, 23, 59)) == "2026.12.31_23.59.log",
          "the end of the year: " + sweepLogName(localTime(2026, 12, 31, 23, 59)));
    check(sweepLogName(localTime(2026, 10, 11, 1, 34)).size() == 20,
          "the name is 20 characters, whatever the date");
}

/** The sweep is named by its minute, so the seconds are not in the name. */
static void test_seconds_are_not_in_the_name()
{
    printf("seconds\n");
    tm early = localTime(2026, 10, 11, 1, 34);
    early.tm_sec = 1;
    tm late = localTime(2026, 10, 11, 1, 34);
    late.tm_sec = 58;
    check(sweepLogName(early) == sweepLogName(late),
          "two sweeps in the same minute share a name (the second appends)");
}

/** The names have to sort chronologically without being parsed. */
static void test_names_sort_as_they_happened()
{
    printf("order\n");
    const vector<string> names = {
        sweepLogName(localTime(2026, 10, 11, 2, 0)),
        sweepLogName(localTime(2025, 12, 31, 23, 59)),
        sweepLogName(localTime(2026, 10, 11, 1, 34)),
        sweepLogName(localTime(2026, 9, 30, 23, 59)),
    };
    vector<string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    check(sorted[0] == sweepLogName(localTime(2025, 12, 31, 23, 59)) &&
              sorted[1] == sweepLogName(localTime(2026, 9, 30, 23, 59)) &&
              sorted[2] == sweepLogName(localTime(2026, 10, 11, 1, 34)) &&
              sorted[3] == sweepLogName(localTime(2026, 10, 11, 2, 0)),
          "the name order is the time order: " + join(sorted));
}

/** What counts as a file of this feature — and, more importantly, what does not. */
static void test_what_is_ours()
{
    printf("our files\n");
    check(isSweepLogName("2026.10.11_01.34.log"), "the real name is ours");
    check(isSweepLogName("2026.01.02_03.04.log"), "and so is a padded one");

    check(!isSweepLogName("2026.10.11_01.34.txt"), "another suffix is not");
    check(!isSweepLogName("AutoUpdate_2026.10.11_01.34.log"),
          "the name carries no feature word — the folder does");
    check(!isSweepLogName("2026.10.11_01.3.log"), "a missing minute digit is not");
    check(!isSweepLogName("26.10.11_01.34.log"), "a short year is not");
    check(!isSweepLogName("2026-10-11_01.34.log"), "dashes instead of dots are not");
    check(!isSweepLogName("2026.10.11 01.34.log"), "a space instead of `_` is not");
    check(!isSweepLogName("2026.10.11_01.34.log.1"),
          "the rotated generation of another log is not");
    check(!isSweepLogName("Errors.log"), "a neighbouring log is not");
    check(!isSweepLogName("cache.dat"), "a cache file is not");
    check(!isSweepLogName(""), "an empty name is not");
    check(!isSweepLogName(".."), "a directory entry is not");
}

/** The folders the writer has to create, outermost first. */
static void test_the_folder_chain()
{
    printf("the folder chain\n");
    const vector<string> chain = sweepLogFolderChain("/sdcard");
    check(chain.size() == 2 && chain[0] == "/sdcard/log" &&
              chain[1] == "/sdcard/log/AutoUpdate",
          "two levels, outermost first: " + join(chain));
    check(sweepLogFolderChain("/sdcard/").size() == 2 &&
              sweepLogFolderChain("/sdcard/")[0] == "/sdcard/log",
          "a trailing slash on the mount point is ignored");
    check(sweepLogFolderChain("").empty(),
          "no mount point, no folders (and no log)");
    check(sweepLogFolderChain("/").empty(), "a bare root is not a mount point");
    check(string(kSweepLogFolder) == "log/AutoUpdate",
          "the folder is the operator's: log/AutoUpdate");
}

/** The retention rule keeps the newest files and never touches a foreign one. */
static void test_retention()
{
    printf("retention\n");
    const vector<string> foreign = {"Errors.log", "my-notes.txt", "cache.dat"};

    check(sweepLogFilesToRemove({}, kSweepLogKeep).empty(),
          "an empty folder has nothing to remove");
    check(sweepLogFilesToRemove(foreign, kSweepLogKeep).empty(),
          "a folder with no sweep logs has nothing to remove");

    // exactly the limit: nothing goes
    vector<string> full;
    for (int i = 1; i <= static_cast<int>(kSweepLogKeep); ++i) {
        full.push_back(sweepLogName(localTime(2026, 10, 1, i, 0)));
    }
    check(sweepLogFilesToRemove(full, kSweepLogKeep).empty(),
          "with exactly the limit nothing is removed");

    // one over: the oldest goes
    vector<string> over = full;
    over.push_back(sweepLogName(localTime(2026, 10, 1, 0, 30)));
    const vector<string> gone = sweepLogFilesToRemove(over, kSweepLogKeep);
    check(gone.size() == 1 && gone[0] == sweepLogName(localTime(2026, 10, 1, 0, 30)),
          "the oldest file goes first: " + join(gone));

    // a whole day of sweeps, mixed with the operator's files
    vector<string> many = foreign;
    for (int hour = 0; hour < 24; ++hour) {
        many.push_back(sweepLogName(localTime(2026, 10, 1, hour, 15)));
    }
    const vector<string> removed = sweepLogFilesToRemove(many, kSweepLogKeep);
    check(removed.size() == many.size() - foreign.size() - kSweepLogKeep,
          "24 sweeps and a limit of 10 remove 14: " + to_string(removed.size()));
    bool oldestFirst = true;
    for (size_t i = 1; i < removed.size(); ++i) {
        if (removed[i - 1] > removed[i]) { oldestFirst = false; break; }
    }
    check(oldestFirst, "and they are listed oldest first");
    bool touchedForeign = false;
    for (const auto& name : removed) {
        for (const auto& mine : foreign) {
            if (name == mine) touchedForeign = true;
        }
    }
    check(!touchedForeign, "not one file of somebody else's is removed");
    check(removed[0] == sweepLogName(localTime(2026, 10, 1, 0, 15)),
          "the first to go is the earliest of the day");
}

/** A limit of zero removes everything this feature wrote — and nothing else. */
static void test_a_limit_of_zero()
{
    printf("a limit of zero\n");
    const vector<string> names = {"Errors.log",
                                  sweepLogName(localTime(2026, 10, 1, 1, 0)),
                                  sweepLogName(localTime(2026, 10, 1, 2, 0))};
    const vector<string> gone = sweepLogFilesToRemove(names, 0);
    check(gone.size() == 2 && gone[0] == sweepLogName(localTime(2026, 10, 1, 1, 0)),
          "both of ours go, the neighbour stays: " + join(gone));
}

int main()
{
    test_the_name();
    test_seconds_are_not_in_the_name();
    test_names_sort_as_they_happened();
    test_what_is_ours();
    test_the_folder_chain();
    test_retention();
    test_a_limit_of_zero();

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
