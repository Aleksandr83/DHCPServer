/**
 * @file test_filejson.cpp
 * @brief Unit tests for FileJson / JsonWriter (file-explorer REST payloads).
 *
 * Regression cover for the bug that broke the Files page: a hand-written
 * `GET /api/files/volumes` body started with a stray comma (`{,"enabled":…`),
 * so the response was `200 OK` but `JSON.parse()` in the browser threw
 * "Expected property name or '}' in JSON at position 1".
 *
 * Both classes are free of ESP-IDF dependencies, so the same tests run on a
 * host:
 *   g++ -std=c++17 -Wall -Wextra -Dapp_main=esp_test_app_main -I. \
 *       test/test_filejson.cpp src/web/FileJson.cpp src/web/JsonWriter.cpp \
 *       host_main.cpp -o test_filejson
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/web/FileJson.h"
#include "../src/web/JsonWriter.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { \
    printf("FAIL: %s:%d:\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, \
           std::string(a).c_str(), std::string(b).c_str()); return 1; } } while(0)

using dhcp::web::FileJson;
using dhcp::web::JsonWriter;
using dhcp::files::FileEntry;
using dhcp::storage::VolumeInfo;
namespace {

/**
 * @brief Structural sanity check of a JSON document.
 *
 * Not a parser: it verifies what the writers are responsible for — balanced
 * braces/brackets, closed string literals, and no comma in a position that
 * makes the document (or a browser's `JSON.parse`) fail: right after `{`/`[`,
 * right before `}`/`]`, or doubled.
 *
 * @return "" when the document looks well formed, otherwise a description.
 */
std::string jsonProblem(const std::string& doc)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    char prevMeaningful = '\0';

    if (doc.size() < 2 || doc.front() != '{' || doc.back() != '}') {
        return "not a JSON object";
    }

    for (size_t i = 0; i < doc.size(); ++i) {
        const char c = doc[i];

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            if (static_cast<unsigned char>(c) < 0x20) return "raw control character";
            continue;
        }

        switch (c) {
            case '"':
                inString = true;
                break;
            case '{':
            case '[':
                depth++;
                break;
            case '}':
            case ']':
                if (prevMeaningful == ',') return "trailing comma before a closing bracket";
                depth--;
                if (depth < 0) return "unbalanced closing bracket";
                break;
            case ',':
                if (prevMeaningful == '{' || prevMeaningful == '[') {
                    return "comma right after an opening bracket";
                }
                if (prevMeaningful == ',') return "doubled comma";
                break;
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                continue;   // whitespace does not count as content
            default:
                break;
        }
        prevMeaningful = c;
    }

    if (inString) return "unterminated string";
    if (depth != 0) return "unbalanced brackets";
    return "";
}

/** @brief Fail the test when @p doc is structurally broken. */
#define ASSERT_WELL_FORMED(doc) do { \
    const std::string problem_ = jsonProblem(doc); \
    if (!problem_.empty()) { \
        printf("FAIL: %s:%d: %s in:\n%s\n", __FILE__, __LINE__, problem_.c_str(), (doc).c_str()); \
        return 1; \
    } } while(0)

// ─────────────────────────────────────────────────────
// JsonWriter
// ─────────────────────────────────────────────────────

/** Field order, all value types, and no comma in front of the first member. */
int test_writer_basic()
{
    JsonWriter w;
    w.str("id", "fat");
    w.boolean("mounted", true);
    w.num("size", 42);
    TEST_ASSERT_STR_EQ(w.toString(),
                       "{\"id\":\"fat\",\"mounted\":true,\"size\":42}");
    ASSERT_WELL_FORMED(w.toString());
    return 0;
}

/** An empty writer is `{}`, not `{,}` or `{`. */
int test_writer_empty()
{
    JsonWriter w;
    TEST_ASSERT_TRUE(w.empty());
    TEST_ASSERT_STR_EQ(w.toString(), "{}");
    ASSERT_WELL_FORMED(w.toString());
    return 0;
}

/** Escaping: quotes, backslash, newline, tab; Cyrillic passes through. */
int test_writer_escaping()
{
    JsonWriter w;
    w.str("a", "quote\" back\\ nl\n tab\t cr\r file.txt");
    w.str("b", "\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb");   // "Файл" in UTF-8
    TEST_ASSERT_STR_EQ(w.toString(),
                       "{\"a\":\"quote\\\" back\\\\ nl\\n tab\\t cr\\r file.txt\","
                       "\"b\":\"\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb\"}");
    ASSERT_WELL_FORMED(w.toString());
    return 0;
}

/** Other control characters are dropped instead of breaking the document. */
int test_writer_drops_control_chars()
{
    JsonWriter w;
    w.str("a", std::string("x") + '\x01' + "y");
    TEST_ASSERT_STR_EQ(w.toString(), "{\"a\":\"xy\"}");
    ASSERT_WELL_FORMED(w.toString());
    return 0;
}

/** A literal value is inserted verbatim (arrays are built by the caller). */
int test_writer_literal()
{
    JsonWriter w;
    w.boolean("enabled", false);
    w.literal("items", "[1,2]");
    TEST_ASSERT_STR_EQ(w.toString(), "{\"enabled\":false,\"items\":[1,2]}");
    ASSERT_WELL_FORMED(w.toString());
    return 0;
}

// ─────────────────────────────────────────────────────
// FileJson
// ─────────────────────────────────────────────────────

/** `GET /api/files/volumes` without any volume — the smallest valid body. */
int test_volumes_empty()
{
    const std::string body = FileJson::volumes(false, {});
    TEST_ASSERT_STR_EQ(body, "{\"enabled\":false,\"volumes\":[]}");
    ASSERT_WELL_FORMED(body);
    return 0;
}

/**
 * The regression itself: the body must start with `{"` and never with `{,`.
 * The old hand-written version produced exactly `{,"enabled":…` and the page
 * showed "files.load_fail: SyntaxError: Expected property name or '}' …".
 */
int test_volumes_has_no_leading_comma()
{
    std::vector<VolumeInfo> vols(2);
    vols[0].id = "fat";
    const std::string body = FileJson::volumes(true, vols);
    TEST_ASSERT_TRUE(body.rfind("{\"", 0) == 0);
    TEST_ASSERT_TRUE(body.find("{,") == std::string::npos);
    ASSERT_WELL_FORMED(body);
    return 0;
}

/** Two volumes, the second one unmounted with an error text that needs escaping. */
int test_volumes_two()
{
    std::vector<VolumeInfo> vols(2);

    vols[0].id = "fat";
    vols[0].mountPoint = "/fat";
    vols[0].mounted = true;
    vols[0].present = true;
    vols[0].totalBytes = 21889024;
    vols[0].freeBytes = 20000000;

    vols[1].id = "sd";
    vols[1].mountPoint = "/sdcard";
    vols[1].mounted = false;
    vols[1].present = false;
    vols[1].error = "mount failed (4-bit): bad \"card\" \\ slot";

    const std::string body = FileJson::volumes(true, vols);
    TEST_ASSERT_STR_EQ(body,
        "{\"enabled\":true,\"volumes\":["
        "{\"id\":\"fat\",\"mount_point\":\"/fat\",\"mounted\":true,\"present\":true,"
        "\"total_bytes\":21889024,\"free_bytes\":20000000,\"error\":\"\"},"
        "{\"id\":\"sd\",\"mount_point\":\"/sdcard\",\"mounted\":false,\"present\":false,"
        "\"total_bytes\":0,\"free_bytes\":0,"
        "\"error\":\"mount failed (4-bit): bad \\\"card\\\" \\\\ slot\"}]}");
    ASSERT_WELL_FORMED(body);
    return 0;
}

/** A listing of an empty folder: `entries` is `[]`, not missing and not `[,]`. */
int test_list_empty_entries()
{
    FileJson::ListPayload p;
    p.volume = "fat";
    p.path = "/";
    p.mounted = true;
    p.totalBytes = 21889024;
    p.freeBytes = 21000000;

    const std::string body = FileJson::list(p);
    TEST_ASSERT_STR_EQ(body,
        "{\"volume\":\"fat\",\"path\":\"/\",\"mounted\":true,"
        "\"total_bytes\":21889024,\"free_bytes\":21000000,"
        "\"truncated\":false,\"entries\":[]}");
    ASSERT_WELL_FORMED(body);
    return 0;
}

/** A listing with a folder and two files, names carrying quotes/tab/Cyrillic. */
int test_list_with_entries()
{
    FileJson::ListPayload p;
    p.volume = "fat";
    p.path = "/logs";
    p.mounted = true;
    p.totalBytes = 1000;
    p.freeBytes = 900;
    p.truncated = true;
    p.entries = {
        FileEntry{"sub", true, 0, 1700000000},
        FileEntry{"a\"b.txt", false, 12, 1700000001},
        FileEntry{"\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb \xd0\xbb\xd0\xbe\xd0\xb3.txt", false, 3, 1700000002},
    };

    const std::string body = FileJson::list(p);
    TEST_ASSERT_STR_EQ(body,
        "{\"volume\":\"fat\",\"path\":\"/logs\",\"mounted\":true,"
        "\"total_bytes\":1000,\"free_bytes\":900,\"truncated\":true,\"entries\":["
        "{\"name\":\"sub\",\"is_dir\":true,\"size\":0,\"mtime\":1700000000},"
        "{\"name\":\"a\\\"b.txt\",\"is_dir\":false,\"size\":12,\"mtime\":1700000001},"
        "{\"name\":\"\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb \xd0\xbb\xd0\xbe\xd0\xb3.txt\","
        "\"is_dir\":false,\"size\":3,\"mtime\":1700000002}]}");
    ASSERT_WELL_FORMED(body);
    return 0;
}

/** Text payload: multi-line content with quotes survives the round trip. */
int test_text_payload()
{
    FileJson::TextPayload p;
    p.volume = "fat";
    p.path = "/notes.txt";
    p.text = "line 1\nsay \"hi\"\tend";
    p.size = p.text.size();
    p.mtime = 1700000003;

    const std::string body = FileJson::text(p);
    TEST_ASSERT_STR_EQ(body,
        "{\"volume\":\"fat\",\"path\":\"/notes.txt\",\"size\":19,\"mtime\":1700000003,"
        "\"truncated\":false,\"text\":\"line 1\\nsay \\\"hi\\\"\\tend\"}");
    ASSERT_WELL_FORMED(body);
    return 0;
}

/** Settings payload with all six members. */
int test_settings_payload()
{
    FileJson::SettingsPayload p;
    p.enabled = true;
    p.allowOwnSubnet = true;
    p.filterActive = false;
    p.subnetAddress = "192.168.1.201";
    p.subnetMask = "255.255.255.0";
    p.blockedCount = 3;

    const std::string body = FileJson::settings(p);
    TEST_ASSERT_STR_EQ(body,
        "{\"enabled\":true,\"allow_own_subnet\":true,"
        "\"subnet_address\":\"192.168.1.201\",\"subnet_mask\":\"255.255.255.0\","
        "\"filter_active\":false,\"blocked_count\":3}");
    ASSERT_WELL_FORMED(body);
    return 0;
}

// ─────────────────────────────────────────────────────
// The volume array shared with `GET /api/status`
// ─────────────────────────────────────────────────────

/**
 * `volumeArray()` is what the home page renders next to the RAM bars, so the
 * endpoint has to be able to paste it into its own object: an empty array, one
 * volume and two volumes, each time glued into a `{"volumes":…}` document.
 */
int test_volume_array()
{
    TEST_ASSERT_STR_EQ(FileJson::volumeArray({}), "[]");
    ASSERT_WELL_FORMED("{\"volumes\":" + FileJson::volumeArray({}) + "}");

    std::vector<VolumeInfo> one(1);
    one[0].id = "fat";
    one[0].mountPoint = "/fat";
    one[0].mounted = true;
    one[0].present = true;
    one[0].totalBytes = 21889024;
    one[0].freeBytes = 21889024;
    const std::string oneJson = FileJson::volumeArray(one);
    TEST_ASSERT_STR_EQ(oneJson,
        "[{\"id\":\"fat\",\"mount_point\":\"/fat\",\"mounted\":true,\"present\":true,"
        "\"total_bytes\":21889024,\"free_bytes\":21889024,\"error\":\"\"}]");

    // The status document wraps the array: the field needs one comma in front
    // and the array itself must not start or end with one.
    const std::string insideStatus =
        "{\"files_enabled\":true,\"volumes\":" + oneJson +
        ",\"cpu_load0\":0,\"ram_total\":786432}";
    ASSERT_WELL_FORMED(insideStatus);
    TEST_ASSERT_TRUE(insideStatus.find("{,") == std::string::npos);

    // A second, unmounted volume (no card inserted): two objects, one comma.
    std::vector<VolumeInfo> two = one;
    two.emplace_back();
    two[1].id = "sd";
    two[1].mountPoint = "/sdcard";
    two[1].error = "mount failed (1-bit): ESP_ERR_TIMEOUT";
    const std::string twoJson = FileJson::volumeArray(two);
    TEST_ASSERT_TRUE(twoJson.find("\"id\":\"sd\"") != std::string::npos);
    TEST_ASSERT_TRUE(twoJson.find("},\"{") == std::string::npos);
    TEST_ASSERT_TRUE(twoJson.find("},{") != std::string::npos);
    ASSERT_WELL_FORMED("{\"volumes\":" + twoJson + "}");

    // ... and the explorer's own endpoint still reports the same array.
    const std::string explorer = FileJson::volumes(true, two);
    TEST_ASSERT_TRUE(explorer.find(oneJson.substr(1, oneJson.size() - 2)) != std::string::npos);
    ASSERT_WELL_FORMED(explorer);
    return 0;
}

// ─────────────────────────────────────────────────────
// Volume-check report (`GET /api/files/check`)
// ─────────────────────────────────────────────────────

/**
 * A check report is polled while the walk runs, so the payload has both states:
 * running (counters, current path, no failures yet) and finished (with the
 * failures). The escaped failure text is what the dialog renders, so quotes and
 * non-ASCII paths have to survive the round trip.
 */
int test_check_report()
{
    ::dhcp::files::CheckReport running;
    running.busy = true;
    running.volume = "sd";
    running.current = "/logs/a.txt";
    running.dirs = 3;
    running.files = 12;
    running.bytes = 4096;
    running.budgetBytes = 67108864;

    const std::string busyBody = FileJson::check(running);
    TEST_ASSERT_STR_EQ(busyBody,
        "{\"busy\":true,\"finished\":false,\"truncated\":false,\"cancelled\":false,"
        "\"volume\":\"sd\",\"current\":\"/logs/a.txt\",\"dirs\":3,\"files\":12,"
        "\"bad_entries\":0,\"bytes_read\":4096,\"budget_bytes\":67108864,\"errors\":[]}");
    ASSERT_WELL_FORMED(busyBody);

    // Finished and clean: the errors array stays an empty array, not a missing
    // member (the dialog checks `bad_entries`, the list checks `errors`).
    ::dhcp::files::CheckReport clean;
    clean.finished = true;
    clean.volume = "fat";
    clean.dirs = 2;
    clean.files = 5;
    clean.bytes = 1024;
    clean.budgetBytes = 67108864;
    const std::string cleanBody = FileJson::check(clean);
    TEST_ASSERT_TRUE(cleanBody.find("\"errors\":[]") != std::string::npos);
    ASSERT_WELL_FORMED(cleanBody);

    // Finished with failures: one escaped path, one escaped detail.
    ::dhcp::files::CheckReport bad = clean;
    bad.badEntries = 2;
    bad.truncated = true;
    bad.errors.push_back({"/logs/broken \"file\".bin",
                          "size mismatch (4096 in the entry, 0 readable)"});
    bad.errors.push_back({"/фото/снимок.jpg", "Input/output error"});

    const std::string badBody = FileJson::check(bad);
    TEST_ASSERT_STR_EQ(badBody,
        "{\"busy\":false,\"finished\":true,\"truncated\":true,\"cancelled\":false,"
        "\"volume\":\"fat\",\"current\":\"\",\"dirs\":2,\"files\":5,"
        "\"bad_entries\":2,\"bytes_read\":1024,\"budget_bytes\":67108864,"
        "\"errors\":[{\"path\":\"/logs/broken \\\"file\\\".bin\","
        "\"detail\":\"size mismatch (4096 in the entry, 0 readable)\"},"
        "{\"path\":\"/фото/снимок.jpg\",\"detail\":\"Input/output error\"}]}");
    ASSERT_WELL_FORMED(badBody);
    return 0;
}

// ─────────────────────────────────────────────────────
// The structural checker itself
// ─────────────────────────────────────────────────────

/** Guard the guard: the checker must reject the body that broke the page. */
int test_checker_rejects_broken_documents()
{
    TEST_ASSERT_FALSE(jsonProblem("{,\"enabled\":false}").empty());
    TEST_ASSERT_FALSE(jsonProblem("{\"a\":1,}").empty());
    TEST_ASSERT_FALSE(jsonProblem("{\"a\":1,,\"b\":2}").empty());
    TEST_ASSERT_FALSE(jsonProblem("{\"a\":[,1]}").empty());
    TEST_ASSERT_FALSE(jsonProblem("{\"a\":\"unterminated}").empty());
    TEST_ASSERT_FALSE(jsonProblem("{\"a\":1").empty());
    TEST_ASSERT_FALSE(jsonProblem("[,]").empty());

    TEST_ASSERT_TRUE(jsonProblem("{}").empty());
    TEST_ASSERT_TRUE(jsonProblem("{\"a\":\"x,y}\"}").empty());   // commas inside text
    TEST_ASSERT_TRUE(jsonProblem("{\"a\":[1,2]}").empty());
    return 0;
}

/** Every builder output survives the checker with an arbitrary payload. */
int test_all_builders_well_formed()
{
    std::vector<VolumeInfo> vols(1);
    vols[0].id = "sd";
    vols[0].error = "x";
    ASSERT_WELL_FORMED(FileJson::volumes(true, vols));

    FileJson::ListPayload list;
    list.volume = "sd";
    list.path = "/";
    ASSERT_WELL_FORMED(FileJson::list(list));

    FileJson::TextPayload text;
    text.volume = "sd";
    text.path = "/a.txt";
    text.text = "";
    ASSERT_WELL_FORMED(FileJson::text(text));

    FileJson::SettingsPayload settings;
    ASSERT_WELL_FORMED(FileJson::settings(settings));

    ::dhcp::files::CheckReport check;
    check.volume = "sd";
    check.errors.push_back({"/a", "b"});
    ASSERT_WELL_FORMED(FileJson::check(check));
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running FileJson/JsonWriter tests...\n");
    int failures = 0;

    failures += test_writer_basic();
    failures += test_writer_empty();
    failures += test_writer_escaping();
    failures += test_writer_drops_control_chars();
    failures += test_writer_literal();
    failures += test_volumes_empty();
    failures += test_volumes_has_no_leading_comma();
    failures += test_volumes_two();
    failures += test_list_empty_entries();
    failures += test_list_with_entries();
    failures += test_text_payload();
    failures += test_settings_payload();
    failures += test_volume_array();
    failures += test_check_report();
    failures += test_checker_rejects_broken_documents();
    failures += test_all_builders_well_formed();

    if (failures == 0) {
        printf("All FileJson/JsonWriter tests PASSED!\n");
    } else {
        printf("Some FileJson/JsonWriter tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
