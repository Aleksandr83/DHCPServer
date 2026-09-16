/**
 * @file test_pathutil.cpp
 * @brief Unit tests for PathUtil (path policy of the FAT file explorer).
 *
 * Build with: pio test -e esp32dev
 *
 * `PathUtil` has no ESP-IDF dependency, so the same tests also run on a host:
 *   g++ -std=c++17 -Dapp_main=esp_test_app_main -I. \
 *       test/test_pathutil.cpp src/storage/PathUtil.cpp host_main.cpp -o test_pathutil
 */

#include <cstdio>
#include <string>

// Include the code under test (adjust path as needed for test build)
#include "../src/storage/PathUtil.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, std::string(a).c_str(), std::string(b).c_str()); return 1; } } while(0)

using dhcp::storage::PathUtil;

extern "C" {

/** Valid inputs are normalized to "/seg/seg" — or to "/" for the root. */
static int test_normalize_ok()
{
    std::string out;

    // Root variants
    TEST_ASSERT_TRUE(PathUtil::normalize("", out));
    TEST_ASSERT_STR_EQ(out, "/");
    TEST_ASSERT_TRUE(PathUtil::normalize("/", out));
    TEST_ASSERT_STR_EQ(out, "/");
    TEST_ASSERT_TRUE(PathUtil::normalize("///", out));
    TEST_ASSERT_STR_EQ(out, "/");
    TEST_ASSERT_TRUE(PathUtil::normalize("/.", out));
    TEST_ASSERT_STR_EQ(out, "/");
    TEST_ASSERT_TRUE(PathUtil::normalize("./", out));
    TEST_ASSERT_STR_EQ(out, "/");

    // Missing leading slash is added
    TEST_ASSERT_TRUE(PathUtil::normalize("logs", out));
    TEST_ASSERT_STR_EQ(out, "/logs");

    // Slash / dot noise is removed
    TEST_ASSERT_TRUE(PathUtil::normalize("/a/b/", out));
    TEST_ASSERT_STR_EQ(out, "/a/b");
    TEST_ASSERT_TRUE(PathUtil::normalize("a//b", out));
    TEST_ASSERT_STR_EQ(out, "/a/b");
    TEST_ASSERT_TRUE(PathUtil::normalize("/a/./b/", out));
    TEST_ASSERT_STR_EQ(out, "/a/b");
    TEST_ASSERT_TRUE(PathUtil::normalize("//a///b//c//", out));
    TEST_ASSERT_STR_EQ(out, "/a/b/c");

    // Realistic names: spaces, unicode, dashes, dots inside the name
    TEST_ASSERT_TRUE(PathUtil::normalize("/logs/2026-09-15 dns.txt", out));
    TEST_ASSERT_STR_EQ(out, "/logs/2026-09-15 dns.txt");
    TEST_ASSERT_TRUE(PathUtil::normalize("/фото/образ 1.jpg", out));
    TEST_ASSERT_STR_EQ(out, "/фото/образ 1.jpg");
    TEST_ASSERT_TRUE(PathUtil::normalize("/cache.dat", out));
    TEST_ASSERT_STR_EQ(out, "/cache.dat");
    TEST_ASSERT_TRUE(PathUtil::normalize("/a.b.c", out));
    TEST_ASSERT_STR_EQ(out, "/a.b.c");

    // Output must be untouched on failure
    out = "sentinel";
    TEST_ASSERT_FALSE(PathUtil::normalize("/../etc", out));
    TEST_ASSERT_STR_EQ(out, "sentinel");

    return 0;
}

/** Anything that could escape the volume or confuse FATFS is rejected. */
static int test_normalize_rejects()
{
    std::string out;

    // Escaping the volume
    TEST_ASSERT_FALSE(PathUtil::normalize("..", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/..", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/../", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("a/../b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a/b/../../..", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("a/..", out));

    // Illegal characters (FAT-illegal set, control chars, backslash)
    TEST_ASSERT_FALSE(PathUtil::normalize("/a*b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a?b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a|b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a:b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a<b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a>b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a\"b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/c:\\temp", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/a\\b", out));
    TEST_ASSERT_FALSE(PathUtil::normalize(std::string("/a") + '\x01', out));
    TEST_ASSERT_FALSE(PathUtil::normalize(std::string("/a") + '\x7F', out));

    // Trailing dot / space would be trimmed by FATFS on create
    TEST_ASSERT_FALSE(PathUtil::normalize("/name.", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/name ", out));
    TEST_ASSERT_FALSE(PathUtil::normalize("/dir/name. ", out));

    // Length and depth caps
    TEST_ASSERT_FALSE(PathUtil::normalize("/" + std::string(PathUtil::kMaxSegmentLen + 1, 'x'), out));
    std::string deep;
    for (size_t i = 0; i <= PathUtil::kMaxDepth + 1; ++i) deep += "/d";
    TEST_ASSERT_FALSE(PathUtil::normalize(deep, out));
    std::string longPath = "/";
    for (size_t i = 0; i < PathUtil::kMaxPathLen; ++i) longPath += 'y';
    TEST_ASSERT_FALSE(PathUtil::normalize(longPath, out));

    return 0;
}

/** Boundary values that must still be accepted. */
static int test_normalize_limits()
{
    std::string out;

    // Exactly the maximum segment length
    TEST_ASSERT_TRUE(PathUtil::normalize("/" + std::string(PathUtil::kMaxSegmentLen, 'x'), out));
    TEST_ASSERT_EQ(out.size(), PathUtil::kMaxSegmentLen + 1);

    // Exactly the maximum depth
    std::string deep;
    for (size_t i = 0; i < PathUtil::kMaxDepth; ++i) deep += "/d";
    TEST_ASSERT_TRUE(PathUtil::normalize(deep, out));
    TEST_ASSERT_STR_EQ(out, deep);

    return 0;
}

/** Names (single segments) used by mkdir/rename/upload. */
static int test_valid_name()
{
    TEST_ASSERT_TRUE(PathUtil::isValidName("logs"));
    TEST_ASSERT_TRUE(PathUtil::isValidName("2026-09-15.txt"));
    TEST_ASSERT_TRUE(PathUtil::isValidName("cache.dat"));
    TEST_ASSERT_TRUE(PathUtil::isValidName("образ 1.jpg"));

    TEST_ASSERT_FALSE(PathUtil::isValidName(""));
    TEST_ASSERT_FALSE(PathUtil::isValidName("."));
    TEST_ASSERT_FALSE(PathUtil::isValidName(".."));
    TEST_ASSERT_FALSE(PathUtil::isValidName("a/b"));
    TEST_ASSERT_FALSE(PathUtil::isValidName("/a"));
    TEST_ASSERT_FALSE(PathUtil::isValidName("a\\b"));
    TEST_ASSERT_FALSE(PathUtil::isValidName("a:b"));
    TEST_ASSERT_FALSE(PathUtil::isValidName("name."));
    TEST_ASSERT_FALSE(PathUtil::isValidName("name "));
    TEST_ASSERT_FALSE(PathUtil::isValidName(std::string(PathUtil::kMaxSegmentLen + 1, 'x')));
    return 0;
}

/** Directory + name → child path (upload / mkdir / rename destination). */
static int test_normalize_child()
{
    std::string out;

    TEST_ASSERT_TRUE(PathUtil::normalizeChild("/", "a.txt", out));
    TEST_ASSERT_STR_EQ(out, "/a.txt");
    TEST_ASSERT_TRUE(PathUtil::normalizeChild("", "a.txt", out));
    TEST_ASSERT_STR_EQ(out, "/a.txt");
    TEST_ASSERT_TRUE(PathUtil::normalizeChild("/logs/", "a.txt", out));
    TEST_ASSERT_STR_EQ(out, "/logs/a.txt");
    TEST_ASSERT_TRUE(PathUtil::normalizeChild("/logs", "2026 dns.txt", out));
    TEST_ASSERT_STR_EQ(out, "/logs/2026 dns.txt");

    TEST_ASSERT_FALSE(PathUtil::normalizeChild("/", "../a.txt", out));
    TEST_ASSERT_FALSE(PathUtil::normalizeChild("/", "a/b", out));
    TEST_ASSERT_FALSE(PathUtil::normalizeChild("/../x", "a.txt", out));
    TEST_ASSERT_FALSE(PathUtil::normalizeChild("/", "", out));
    TEST_ASSERT_FALSE(PathUtil::normalizeChild("/", ".", out));
    // Depth cap applies to the resulting path, not just to the input
    std::string deep;
    for (size_t i = 0; i < PathUtil::kMaxDepth; ++i) deep += "/d";
    TEST_ASSERT_FALSE(PathUtil::normalizeChild(deep, "x", out));

    return 0;
}

/** join / parent / basename — the helpers the REST layer builds paths with. */
static int test_join_parent_basename()
{
    TEST_ASSERT_STR_EQ(PathUtil::join("/fat", "/"), "/fat");
    TEST_ASSERT_STR_EQ(PathUtil::join("/fat", ""), "/fat");
    TEST_ASSERT_STR_EQ(PathUtil::join("/fat", "/a.txt"), "/fat/a.txt");
    TEST_ASSERT_STR_EQ(PathUtil::join("/fat/", "/a/b"), "/fat/a/b");
    TEST_ASSERT_STR_EQ(PathUtil::join("/sdcard", "/logs/x.txt"), "/sdcard/logs/x.txt");

    TEST_ASSERT_STR_EQ(PathUtil::parent("/"), "/");
    TEST_ASSERT_STR_EQ(PathUtil::parent("/a"), "/");
    TEST_ASSERT_STR_EQ(PathUtil::parent("/a/b"), "/a");
    TEST_ASSERT_STR_EQ(PathUtil::parent("/a/b/c.txt"), "/a/b");

    TEST_ASSERT_STR_EQ(PathUtil::basename("/"), "");
    TEST_ASSERT_STR_EQ(PathUtil::basename("/a"), "a");
    TEST_ASSERT_STR_EQ(PathUtil::basename("/a/b/c.txt"), "c.txt");

    // Round-trip: parent + basename rebuilds the original path
    std::string norm;
    TEST_ASSERT_TRUE(PathUtil::normalize("/logs/2026/15 dns.txt", norm));
    TEST_ASSERT_STR_EQ(PathUtil::join(PathUtil::parent(norm),
                                      PathUtil::basename(norm)), norm);

    return 0;
}

/** `<name>.part` is reserved for uploads: neither creatable nor listed. */
static int test_upload_part_names()
{
    TEST_ASSERT_TRUE(PathUtil::isPartName("movie.mkv.part"));
    TEST_ASSERT_TRUE(PathUtil::isPartName("a.part"));
    TEST_ASSERT_FALSE(PathUtil::isPartName(".part"));       // the suffix alone
    TEST_ASSERT_FALSE(PathUtil::isPartName("part"));
    TEST_ASSERT_FALSE(PathUtil::isPartName("x.part.txt"));
    TEST_ASSERT_FALSE(PathUtil::isPartName("x.PART"));      // FAT is case-insensitive, the suffix is not

    TEST_ASSERT_FALSE(PathUtil::isValidName("movie.mkv.part"));
    TEST_ASSERT_FALSE(PathUtil::isValidName("a.part"));
    TEST_ASSERT_TRUE(PathUtil::isValidName("part"));
    TEST_ASSERT_TRUE(PathUtil::isValidName("part.txt"));

    std::string route;
    TEST_ASSERT_FALSE(PathUtil::normalize("/logs/movie.mkv.part", route));
    TEST_ASSERT_FALSE(PathUtil::normalizeChild("/logs", "x.part", route));
    TEST_ASSERT_TRUE(PathUtil::normalize("/logs/movie.mkv", route));

    return 0;
}

void app_main()
{
    printf("Running PathUtil tests...\n");
    int failures = 0;

    failures += test_normalize_ok();
    failures += test_normalize_rejects();
    failures += test_normalize_limits();
    failures += test_valid_name();
    failures += test_upload_part_names();
    failures += test_normalize_child();
    failures += test_join_parent_basename();

    if (failures == 0) {
        printf("All PathUtil tests PASSED!\n");
    } else {
        printf("Some PathUtil tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
