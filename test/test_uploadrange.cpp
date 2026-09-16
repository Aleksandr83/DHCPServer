/**
 * @file test_uploadrange.cpp
 * @brief Unit tests for UploadRange (chunk arithmetic of a resumable upload).
 *
 * Build with: pio test -e esp32dev
 *
 * `UploadRange` has no ESP-IDF dependency, so the same tests also run on a host:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Dapp_main=esp_test_app_main -I. \
 *       test/test_uploadrange.cpp src/files/UploadRange.cpp host_main.cpp \
 *       -o test_uploadrange
 */

#include <cstdio>
#include <string>

#include "../src/files/UploadRange.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, std::string(a).c_str(), std::string(b).c_str()); return 1; } } while(0)

using dhcp::files::UploadRange;

extern "C" {

/** A request without `total` is one whole file and starts from scratch. */
static int test_single_request()
{
    UploadRange r;
    TEST_ASSERT_STR_EQ(UploadRange::check(0, 0, 1000, false, 0, r), "");
    TEST_ASSERT_FALSE(r.resumable);
    TEST_ASSERT_EQ(r.offset, 0u);
    TEST_ASSERT_EQ(r.total, 1000u);
    TEST_ASSERT_TRUE(r.completes());
    TEST_ASSERT_EQ(r.neededBytes(), 1000u);

    // An `offset` without `total` is meaningless for such a request: whatever is
    // on the device is replaced, which is what the client asks for by not
    // sending `total`.
    TEST_ASSERT_STR_EQ(UploadRange::check(700, 500, 10, false, 0, r), "");
    TEST_ASSERT_EQ(r.offset, 0u);
    TEST_ASSERT_EQ(r.total, 10u);

    // An empty body is a valid, empty file for such a request: that is how the
    // text editor truncates one (the upload endpoint refuses it earlier, with
    // 411, because a browser never sends an empty file that way).
    TEST_ASSERT_STR_EQ(UploadRange::check(0, 0, 0, false, 0, r), "");
    TEST_ASSERT_EQ(r.total, 0u);
    TEST_ASSERT_TRUE(r.completes());
    TEST_ASSERT_EQ(r.neededBytes(), 0u);
    return 0;
}

/** The three chunks of a resumable upload: first, middle, last. */
static int test_resumable_chunks()
{
    UploadRange r;

    // First chunk of 250 bytes: 100 now, 150 to go.
    TEST_ASSERT_STR_EQ(UploadRange::check(0, 0, 100, true, 250, r), "");
    TEST_ASSERT_TRUE(r.resumable);
    TEST_ASSERT_FALSE(r.completes());
    TEST_ASSERT_EQ(r.endOffset(), 100u);
    TEST_ASSERT_EQ(r.neededBytes(), 250u);

    // Continued where the device stopped.
    TEST_ASSERT_STR_EQ(UploadRange::check(100, 100, 100, true, 250, r), "");
    TEST_ASSERT_FALSE(r.completes());
    TEST_ASSERT_EQ(r.endOffset(), 200u);
    TEST_ASSERT_EQ(r.neededBytes(), 150u);

    // Last chunk completes the file, and the space check only counts what is
    // still missing.
    TEST_ASSERT_STR_EQ(UploadRange::check(200, 200, 50, true, 250, r), "");
    TEST_ASSERT_TRUE(r.completes());
    TEST_ASSERT_EQ(r.endOffset(), 250u);
    TEST_ASSERT_EQ(r.neededBytes(), 50u);

    // A chunk that arrives in one piece after a pause of zero bytes behaves like
    // the first one.
    UploadRange whole;
    TEST_ASSERT_STR_EQ(UploadRange::check(0, 0, 250, true, 250, whole), "");
    TEST_ASSERT_TRUE(whole.completes());
    return 0;
}

/** The chunk has to line up exactly with what the device already has. */
static int test_offset_mismatch()
{
    UploadRange r;
    std::string err;

    // Device has less than the client claims.
    err = UploadRange::check(0, 100, 10, true, 250, r);
    TEST_ASSERT_TRUE(err.find("100") != std::string::npos);
    TEST_ASSERT_TRUE(err.find("0 bytes") != std::string::npos);

    // Device has more (e.g. a repeated chunk after a lost reply).
    err = UploadRange::check(150, 100, 10, true, 250, r);
    TEST_ASSERT_TRUE(err.find("150 bytes") != std::string::npos);
    return 0;
}

/** Bodies that cannot be part of the file are refused. */
static int test_out_of_range()
{
    UploadRange r;
    std::string err;

    // Body runs past the end.
    err = UploadRange::check(200, 200, 100, true, 250, r);
    TEST_ASSERT_TRUE(err.find("runs past the end") != std::string::npos);

    // Offset at the end with bytes still to send.
    err = UploadRange::check(250, 250, 10, true, 250, r);
    TEST_ASSERT_TRUE(err.find("at or past the end") != std::string::npos);

    // Offset beyond the end.
    err = UploadRange::check(300, 300, 10, true, 250, r);
    TEST_ASSERT_TRUE(err.find("at or past the end") != std::string::npos);

    // A file of zero bytes is not an upload: use /api/files/text for that.
    err = UploadRange::check(0, 0, 10, true, 0, r);
    TEST_ASSERT_STR_EQ(err, "total must be greater than zero");

    // Empty body in resumable mode, too.
    err = UploadRange::check(0, 0, 0, true, 250, r);
    TEST_ASSERT_STR_EQ(err, "empty body");
    return 0;
}

} // extern "C"

extern "C" int app_main(void)
{
    struct { const char* name; int (*fn)(void); } tests[] = {
        { "single request", test_single_request },
        { "resumable chunks", test_resumable_chunks },
        { "offset mismatch", test_offset_mismatch },
        { "out of range", test_out_of_range },
    };

    int failed = 0;
    for (auto& t : tests) {
        if (t.fn() != 0) {
            printf("  -> test '%s' FAILED\n", t.name);
            failed++;
        }
    }

    if (failed == 0) {
        printf("All UploadRange tests PASSED!\n");
        return 0;
    }
    printf("%d UploadRange test(s) FAILED\n", failed);
    return 1;
}
