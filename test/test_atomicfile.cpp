/**
 * @file test_atomicfile.cpp
 * @brief Unit tests for publishing a file so that a crash cannot empty it.
 *
 * `cache.dat` was written straight into the file, and `fopen(…, "wb")` truncates
 * it at once: a panic in the middle of that write left a half-written file, the
 * next boot refused it as damaged, and the operator's cache was simply gone
 * (19.10.2026, when turning the SSH server off panicked the device). The cure is
 * the policy the file explorer already uses for uploads — write `<name>.part`,
 * then publish — plus the one thing FatFS forces: `f_rename()` refuses an existing
 * destination, so the old file goes first and the window between the two calls is
 * covered by a recovery step that publishes a complete `.part` when the
 * destination is missing.
 *
 * These tests run against the real host filesystem, in a directory of their own
 * under %TEMP% (the same approach as `test_transferengine`).
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_atomicfile.cpp host_main.cpp -o test_atomicfile
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

#include "../src/core/AtomicFile.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using namespace dhcp::core;

namespace {

string g_dir;

string path(const string& name)
{
    return g_dir + "/" + name;
}

bool exists(const string& p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

void writeText(const string& p, const string& text)
{
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

string readText(const string& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "<missing>";
    char buf[256] = { 0 };
    const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return string(buf, got);
}

/**
 * @brief Remove both names a test works with.
 *
 * The tests run against a real directory that outlives one run (`%TEMP%`), and a
 * file left by an earlier run is exactly what makes a "nothing exists" assert lie.
 */
void clean(const string& p)
{
    ::remove(p.c_str());
    ::remove(atomicfile::partPath(p).c_str());
}

/** The names the policy works with are the destination plus two suffixes. */
int test_the_names()
{
    TEST_ASSERT_STREQ(atomicfile::partPath("/fat/cache.dat"), "/fat/cache.dat.part");
    return 0;
}

/** A publish replaces the old content and leaves nothing behind. */
int test_the_publish_replaces()
{
    const string file = path("a.dat");
    clean(file);
    writeText(atomicfile::partPath(file), "new");
    writeText(file, "old");

    TEST_ASSERT_TRUE(atomicfile::publish(file));
    TEST_ASSERT_STREQ(readText(file), "new");
    TEST_ASSERT_TRUE(!exists(atomicfile::partPath(file)));
    return 0;
}

/** A publish with no `.part` beside it is a failure, not a silent success. */
int test_the_publish_needs_a_part()
{
    const string file = path("b.dat");
    clean(file);
    writeText(file, "keep");
    TEST_ASSERT_TRUE(!atomicfile::publish(file));
    TEST_ASSERT_STREQ(readText(file), "keep");
    return 0;
}

/**
 * The window FatFS forces: the old file is removed before the rename, so a crash
 * between the two leaves a complete `.part` and no destination. Recovery publishes
 * the part — the complete new file — instead of losing both.
 */
int test_the_recovery_of_a_lost_destination()
{
    const string file = path("c.dat");
    clean(file);
    writeText(atomicfile::partPath(file), "complete");
    TEST_ASSERT_TRUE(!exists(file));

    atomicfile::recover(file);
    TEST_ASSERT_STREQ(readText(file), "complete");
    TEST_ASSERT_TRUE(!exists(atomicfile::partPath(file)));
    return 0;
}

/** Recovery must never overwrite a healthy file with an older `.part`. */
int test_the_recovery_keeps_a_healthy_file()
{
    const string file = path("d.dat");
    clean(file);
    writeText(file, "good");
    writeText(atomicfile::partPath(file), "half-written");

    atomicfile::recover(file);
    TEST_ASSERT_STREQ(readText(file), "good");
    return 0;
}

/** Recovery of a name with nothing beside it changes nothing at all. */
int test_the_recovery_of_nothing()
{
    const string file = path("e.dat");
    clean(file);
    atomicfile::recover(file);
    TEST_ASSERT_TRUE(!exists(file));
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running AtomicFile tests...\n");

    // A directory of our own: the tests write real files, and one of them asks
    // what happens when a file is missing.
    char tempPath[512] = { 0 };
#ifdef _WIN32
    const char* base = getenv("TEMP");
    snprintf(tempPath, sizeof(tempPath), "%s/dhcpserver_atomicfile_test",
             base ? base : ".");
    _mkdir(tempPath);
#else
    snprintf(tempPath, sizeof(tempPath), "/tmp/dhcpserver_atomicfile_test");
    mkdir(tempPath, 0755);
#endif
    g_dir = tempPath;

    int failures = 0;
    failures += test_the_names();
    failures += test_the_publish_replaces();
    failures += test_the_publish_needs_a_part();
    failures += test_the_recovery_of_a_lost_destination();
    failures += test_the_recovery_keeps_a_healthy_file();
    failures += test_the_recovery_of_nothing();

    if (failures == 0) {
        printf("All AtomicFile tests PASSED!\n");
    } else {
        printf("Some AtomicFile tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
