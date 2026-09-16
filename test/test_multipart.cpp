/**
 * @file test_multipart.cpp
 * @brief Unit tests for MultipartExtractor (multipart/form-data payloads).
 *
 * Build with: pio test -e esp32dev
 *
 * `MultipartExtractor` has no ESP-IDF dependency, so the same tests also run on
 * a host:
 *   g++ -std=c++17 -Dapp_main=esp_test_app_main -I. \
 *       test/test_multipart.cpp src/web/MultipartExtractor.cpp host_main.cpp \
 *       -o test_multipart
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// Include the code under test (adjust path as needed for test build)
#include "../src/web/MultipartExtractor.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, std::string(a).c_str(), std::string(b).c_str()); return 1; } } while(0)

using dhcp::web::MultipartExtractor;

namespace {

const char* kB = "----WebKitFormBoundaryAbC123";

/** @brief A realistic browser body with a binary payload. */
std::string makeBody(const std::string& payload)
{
    std::string body;
    body += "--";
    body += kB;
    body += "\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"DHCPServer.bin\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body += payload;
    body += "\r\n--";
    body += kB;
    body += "--\r\n";
    return body;
}

/** @brief Feed @p body in chunks of @p chunkSize and collect the payload. */
std::string extract(const std::string& body, size_t chunkSize, bool& finished, bool& failed)
{
    std::string out;
    MultipartExtractor ex(kB, [&out](const uint8_t* d, size_t n) {
        out.append(reinterpret_cast<const char*>(d), n);
        return true;
    });

    for (size_t i = 0; i < body.size(); i += chunkSize) {
        const size_t n = std::min(chunkSize, body.size() - i);
        if (!ex.feed(reinterpret_cast<const uint8_t*>(body.data() + i), n)) break;
    }
    finished = ex.finish();
    failed = ex.failed();
    return out;
}

/** @brief Payload of 1 KB with the OTA image magic, NULs and the delimiter. */
std::string binaryPayload()
{
    std::string p;
    p += '\xE9';                       // image magic — must survive verbatim
    p += "\x01\x02\x00\xFF";           // NUL and high bytes
    for (int i = 0; i < 1000; ++i) p += static_cast<char>(i & 0xFF);
    p += "\r\n--almost";               // delimiter prefix, but not the delimiter
    return p;
}

} // namespace

extern "C" {

/** One-shot feed: the payload comes out exactly, envelope and epilogue dropped. */
static int test_single_feed()
{
    const std::string payload = "hello OTA image";
    bool finished = false, failed = false;
    const std::string got = extract(makeBody(payload), 4096, finished, failed);

    TEST_ASSERT_FALSE(failed);
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_STR_EQ(got, payload);
    return 0;
}

/** The extractor must not care how the body is chunked (worst case: 1 byte). */
static int test_every_chunk_size()
{
    const std::string payload = binaryPayload();
    const std::string body = makeBody(payload);

    for (size_t chunk : {size_t(1), size_t(2), size_t(3), size_t(7), size_t(16),
                         size_t(63), size_t(64), size_t(65), size_t(1024),
                         size_t(4096)}) {
        bool finished = false, failed = false;
        const std::string got = extract(body, chunk, finished, failed);
        if (failed || !finished || got != payload) {
            printf("FAIL: chunk size %u (failed=%d finished=%d got=%u bytes)\n",
                   (unsigned)chunk, (int)failed, (int)finished, (unsigned)got.size());
            return 1;
        }
    }
    return 0;
}

/** A payload that contains the delimiter prefix must not be truncated. */
static int test_payload_with_delimiter_prefix()
{
    const std::string payload = "A\r\n--" + std::string(kB).substr(0, 10) + "B";
    bool finished = false, failed = false;
    const std::string got = extract(makeBody(payload), 4, finished, failed);

    TEST_ASSERT_FALSE(failed);
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_STR_EQ(got, payload);
    return 0;
}

/** Empty payload (a zero-byte part) is valid and yields nothing. */
static int test_empty_payload()
{
    bool finished = false, failed = false;
    const std::string got = extract(makeBody(""), 4096, finished, failed);

    TEST_ASSERT_FALSE(failed);
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_STR_EQ(got, "");
    return 0;
}

/** A truncated body (no closing delimiter) must be reported as incomplete. */
static int test_missing_closing_delimiter()
{
    std::string body = makeBody("payload that never ends");
    body.resize(body.size() - 30);   // cut through payload + delimiter

    bool finished = false, failed = false;
    extract(body, 8, finished, failed);

    TEST_ASSERT_FALSE(finished);
    return 0;
}

/** Empty boundary → the extractor refuses instead of guessing. */
static int test_empty_boundary()
{
    MultipartExtractor ex("", [](const uint8_t*, size_t) { return true; });
    TEST_ASSERT_TRUE(ex.failed());
    TEST_ASSERT_FALSE(ex.finish());
    return 0;
}

/** Headers longer than the cap (no CRLFCRLF) must fail, not eat memory. */
static int test_header_flood()
{
    std::string body = "--";
    body += kB;
    body += "\r\nX-Pad: ";
    body.append(MultipartExtractor::kMaxHeaderBytes + 64, 'x');

    MultipartExtractor ex(kB, [](const uint8_t*, size_t) { return true; });
    ex.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size());

    TEST_ASSERT_TRUE(ex.failed());
    return 0;
}

/** A sink failure aborts the transfer (used for OTA write errors). */
static int test_sink_failure()
{
    const std::string body = makeBody("0123456789");
    MultipartExtractor ex(kB, [](const uint8_t*, size_t) { return false; });

    const bool ok = ex.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(ex.failed());
    return 0;
}

/** A real OTA-like image (magic + 1.2 MB of data) is extracted byte-exact. */
static int test_image_like_payload()
{
    std::string payload;
    payload += '\xE9';
    payload.append(1200 * 1024, '\x5A');   // ~1.2 MB, like DHCPServer.bin
    const std::string body = makeBody(payload);

    bool finished = false, failed = false;
    const std::string got = extract(body, 1024, finished, failed);

    TEST_ASSERT_FALSE(failed);
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_EQ(got.size(), payload.size());
    TEST_ASSERT_TRUE(got == payload);
    return 0;
}

void app_main()
{
    printf("Running MultipartExtractor tests...\n");
    int failures = 0;

    failures += test_single_feed();
    failures += test_every_chunk_size();
    failures += test_payload_with_delimiter_prefix();
    failures += test_empty_payload();
    failures += test_missing_closing_delimiter();
    failures += test_empty_boundary();
    failures += test_header_flood();
    failures += test_sink_failure();
    failures += test_image_like_payload();

    if (failures == 0) {
        printf("All MultipartExtractor tests PASSED!\n");
    } else {
        printf("Some MultipartExtractor tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
