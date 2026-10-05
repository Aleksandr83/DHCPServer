/**
 * @file test_autoupdatelog.cpp
 * @brief Host test for the auto-update detail line (stage 179, rule 23).
 *
 * The line the operator reads in `logs/AutoUpdate.log` is built from a name, a
 * query type, whether the upstream confirmed the record and the fresh TTL. The
 * header has no ESP-IDF dependency on purpose, so the promise about what the file
 * says is checked on the development machine rather than by reading a card.
 *
 * Build (MinGW, from the repository root):
 *   g++ -std=c++17 -Wall -Wextra -I. test/test_autoupdatelog.cpp \
 *       -o test_autoupdatelog
 */

#include "../src/dns/AutoUpdateLogFormat.h"

#include <cstdio>
#include <string>

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (string(a) != string(b)) { \
    printf("FAIL: %s:%d:\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, \
           string(a).c_str(), string(b).c_str()); return 1; } } while(0)

using namespace std;

using namespace dhcp::dns;

int main()
{
    // A record the upstream confirmed: the fresh TTL is the point of the sweep,
    // so it is part of the line.
    TEST_ASSERT_STR_EQ(autoUpdateRecordText("example.com", 1, true, 300),
                       "example.com A refreshed (ttl 300 s)");
    TEST_ASSERT_STR_EQ(autoUpdateRecordText("ipv6.example.com", 28, true, 60),
                       "ipv6.example.com AAAA refreshed (ttl 60 s)");

    // A zero TTL is a value, not "unknown": an upstream may answer "do not
    // cache", and the line must not hide that behind a missing field.
    TEST_ASSERT_STR_EQ(autoUpdateRecordText("example.com", 1, true, 0),
                       "example.com A refreshed (ttl 0 s)");

    // A record the upstream did not confirm is kept and retried first next cycle
    // (a timeout is not proof that the name is gone), and no TTL is printed —
    // the refresh handler only fills one when an address came back.
    TEST_ASSERT_STR_EQ(autoUpdateRecordText("example.com", 1, false, 0),
                       "example.com A not confirmed, record kept");
    TEST_ASSERT_STR_EQ(autoUpdateRecordText("gone.example.com", 28, false, 0),
                       "gone.example.com AAAA not confirmed, record kept");

    // The type names are the cache reader's own numbers, so the log cannot drift
    // from what the cache stores; anything else is named by its number rather
    // than guessed at (the cache never holds one, and calling it AAAA would be a
    // lie in a diagnostic file).
    TEST_ASSERT_STR_EQ(autoUpdateTypeName(1), "A");
    TEST_ASSERT_STR_EQ(autoUpdateTypeName(28), "AAAA");
    TEST_ASSERT_STR_EQ(autoUpdateTypeName(15), "15");

    // A name as long as the cache allows still produces one line: the queue item
    // is what clamps a longer one, in the log itself.
    const string longest(127, 'a');
    const string line = autoUpdateRecordText(longest, 1, true, 86400);
    TEST_ASSERT_TRUE(line.size() > longest.size());
    TEST_ASSERT_STR_EQ(line.substr(0, longest.size()), longest);
    TEST_ASSERT_STR_EQ(line.substr(longest.size()), " A refreshed (ttl 86400 s)");

    printf("All AutoUpdateLog tests PASSED!\n");
    return 0;
}
