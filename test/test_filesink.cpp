/**
 * @file test_filesink.cpp
 * @brief Unit tests for FileSink (temporary file of an upload, resumable).
 *
 * Build with: pio test -e esp32dev
 *
 * Runs on a host too — the sink only needs POSIX plus logging, and
 * `test/stubs/esp_log.h` stands in for the ESP-IDF header:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Dapp_main=esp_test_app_main \
 *       -I test/stubs -I. test/test_filesink.cpp src/files/FileSink.cpp \
 *       host_main.cpp -o test_filesink
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "../src/files/FileSink.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)

using namespace std;

using dhcp::files::FileSink;

namespace {

const char* kDest = "test_filesink_dest.bin";
const char* kPart = "test_filesink_dest.bin.part";

/** Read a file into @p out; false when it does not exist. */
bool readFile(const char* path, string& out)
{
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    out.clear();
    char buf[256];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

long fileSize(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return -1;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    return size;
}

void clean()
{
    ::unlink(kDest);
    ::unlink(kPart);
}

} // namespace

extern "C" {

/** A kept chunk stays on the device as `<name>.part` and is not published. */
static int test_keep_holds_the_part()
{
    clean();

    {
        FileSink sink(kDest);
        TEST_ASSERT_TRUE(sink.isOpen());
        const char first[9] = "01234567";
        TEST_ASSERT_TRUE(sink.write(reinterpret_cast<const uint8_t*>(first), 8));
        TEST_ASSERT_EQ(sink.written(), 8u);
        sink.keep();
    }

    // keep() must survive the destructor: the part is there, the file is not.
    TEST_ASSERT_EQ(fileSize(kPart), 8);
    TEST_ASSERT_EQ(fileSize(kDest), -1);

    clean();
    return 0;
}

/** The next sink appends exactly where the kept one stopped. */
static int test_append_finishes_the_file()
{
    clean();

    {
        FileSink sink(kDest);
        const char first[5] = "ABCD";
        TEST_ASSERT_TRUE(sink.write(reinterpret_cast<const uint8_t*>(first), 4));
        sink.keep();
    }

    {
        // The upload layer has checked this number against the part on the
        // device (FileManager::openWrite / UploadRange).
        const long partSize = fileSize(kPart);
        TEST_ASSERT_EQ(partSize, 4);

        FileSink sink(kDest, static_cast<uint64_t>(partSize));
        TEST_ASSERT_TRUE(sink.isOpen());
        TEST_ASSERT_EQ(sink.written(), 4u);   // continues counting the file
        const char rest[5] = "EFGH";
        TEST_ASSERT_TRUE(sink.write(reinterpret_cast<const uint8_t*>(rest), 4));
        TEST_ASSERT_EQ(sink.written(), 8u);
        TEST_ASSERT_TRUE(sink.commit());
    }

    string content;
    TEST_ASSERT_TRUE(readFile(kDest, content));
    TEST_ASSERT_EQ(content.size(), 8u);
    TEST_ASSERT_TRUE(content == "ABCDEFGH");
    TEST_ASSERT_EQ(fileSize(kPart), -1);      // the part became the file

    clean();
    return 0;
}

/** Destroying an unfinished sink (no commit, no keep) discards the part. */
static int test_unfinished_sink_is_discarded()
{
    clean();

    {
        FileSink sink(kDest);
        const char data[5] = "lost";
        TEST_ASSERT_TRUE(sink.write(reinterpret_cast<const uint8_t*>(data), 4));
    }

    TEST_ASSERT_EQ(fileSize(kPart), -1);
    TEST_ASSERT_EQ(fileSize(kDest), -1);

    clean();
    return 0;
}

/** abort() removes the part and leaves an existing destination alone. */
static int test_abort_keeps_the_destination()
{
    clean();

    {
        FILE* f = fopen(kDest, "wb");
        TEST_ASSERT_TRUE(f != nullptr);
        fputs("old", f);
        fclose(f);
    }

    {
        FileSink sink(kDest);
        const char data[5] = "new!";
        TEST_ASSERT_TRUE(sink.write(reinterpret_cast<const uint8_t*>(data), 4));
        sink.abort();
    }

    string content;
    TEST_ASSERT_TRUE(readFile(kDest, content));
    TEST_ASSERT_TRUE(content == "old");
    TEST_ASSERT_EQ(fileSize(kPart), -1);

    clean();
    return 0;
}

} // extern "C"

extern "C" int app_main(void)
{
    struct { const char* name; int (*fn)(void); } tests[] = {
        { "keep holds the part", test_keep_holds_the_part },
        { "append finishes the file", test_append_finishes_the_file },
        { "unfinished sink is discarded", test_unfinished_sink_is_discarded },
        { "abort keeps the destination", test_abort_keeps_the_destination },
    };

    int failed = 0;
    for (auto& t : tests) {
        if (t.fn() != 0) {
            printf("  -> test '%s' FAILED\n", t.name);
            failed++;
        }
    }

    if (failed == 0) {
        printf("All FileSink tests PASSED!\n");
        return 0;
    }
    printf("%d FileSink test(s) FAILED\n", failed);
    return 1;
}
