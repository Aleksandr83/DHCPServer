/**
 * @file test_routetable.cpp
 * @brief Unit tests for the web server's route-table rules.
 *
 * The table itself (82 routes, with the handlers) lives in `WebServer.cpp` and
 * needs ESP-IDF; the rules about it do not: an entry without a URI or a handler
 * is unusable, a repeated pair of (URI, method) shadows an earlier route, and
 * the HTTP server's handler limit has to be derived from the table — the
 * stage-136 defect was a limit of 81 next to a table of 82, which silently cost
 * the route registered last (`/pages/version.html`, the page that answered 404).
 *
 * Two more rules are checked against the project tree itself, because nothing
 * else can see their violations: every file in `data/` has a route in that table,
 * and every one of those paths is short enough for SPIFFS to open.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -I. \
 *       test/test_routetable.cpp src/web/RouteTable.cpp host_main.cpp \
 *       -o test_routetable
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

#include "../src/web/RouteTable.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); return 1; } } while(0)

using dhcp::web::RouteMethod;
using dhcp::web::RouteTable;
using dhcp::web::WebRoute;

namespace {

/** Stands in for a real handler: the tests never call one, they only store it. */
int dummyHandler(httpd_req*)
{
    return 0;   // ESP_OK
}

int otherHandler(httpd_req*)
{
    return 0;
}

WebRoute route(const char* uri, RouteMethod method = RouteMethod::Get,
               dhcp::web::RouteHandler handler = dummyHandler)
{
    return WebRoute{ uri, method, handler };
}

/**
 * The parts of the project the two tree checks read, both relative to the test's
 * working directory (the project root): the route table and the served files.
 */
const char* const kRoutesFile = "src/web/WebServer.cpp";
const char* const kDataDir = "data";

/** Index of the first entry that repeats an earlier one, or kNone. */
size_t firstRepeat(const WebRoute* routes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (RouteTable::repeatsEarlier(routes, i)) return i;
    }
    return RouteTable::kNone;
}

/**
 * Collect every file under `prefix` (a path relative to the working directory)
 * as an absolute-style URI, e.g. `data/pages/files.html` → `/pages/files.html`.
 *
 * POSIX `dirent` rather than `<filesystem>`: the other host tests in this project
 * already read directories this way, and MinGW has it.
 */
void collectFiles(const string& dir, const string& prefix,
                  vector<string>& out)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        const string name = e->d_name;
        if (name == "." || name == "..") continue;
        const string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collectFiles(path, prefix + "/" + name, out);
        } else if (S_ISREG(st.st_mode)) {
            out.push_back(prefix + "/" + name);
        }
    }
    closedir(d);
}

/** The whole file `path` as one string, or an empty one when it cannot be read. */
string readWholeFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return string();
    string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

/**
 * The longest path SPIFFS can open on this project — `CONFIG_SPIFFS_OBJ_NAME_LEN`
 * read from the sdkconfig files next to the test, minus the terminating NUL
 * (`SPIFFS_OBJ_NAME_LEN` counts it, so 32 leaves 31), or 0 when no sdkconfig is
 * there. Read rather than written down so that a changed configuration moves the
 * limit the test checks instead of silently disagreeing with it.
 */
size_t spiffsMaxPathLen()
{
    DIR* d = opendir(".");
    if (!d) return 0;
    size_t limit = 0;
    while (struct dirent* e = readdir(d)) {
        const string name = e->d_name;
        if (name.size() < 9 || name.compare(0, 9, "sdkconfig") != 0) continue;
        const string cfg = readWholeFile(name.c_str());
        const string key = "CONFIG_SPIFFS_OBJ_NAME_LEN=";
        const size_t at = cfg.find(key);
        if (at == string::npos) continue;   // e.g. sdkconfig.defaults
        const size_t value = strtoul(cfg.c_str() + at + key.size(), nullptr, 10);
        if (value == 0) continue;
        if (limit == 0 || value < limit) limit = value;
    }
    closedir(d);
    return limit == 0 ? 0 : limit - 1;
}

} // namespace

extern "C" {

/** A table of distinct routes is clean. */
static int test_clean_table()
{
    const WebRoute routes[] = {
        route("/api/status"),
        route("/api/version"),
        route("/pages/version.html"),
    };
    constexpr size_t count = sizeof(routes) / sizeof(routes[0]);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(RouteTable::isValid(routes[i]));
        TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, i));
    }
    TEST_ASSERT_EQ(firstRepeat(routes, count), RouteTable::kNone);
    return 0;
}

/** The same URI with the same method twice: the later one is the repeat. */
static int test_repeat_is_found()
{
    const WebRoute routes[] = {
        route("/api/status"),
        route("/api/version"),
        // The same pair, even with a different handler: the pair is the identity.
        route("/api/status", RouteMethod::Get, otherHandler),
        route("/pages/version.html"),
    };
    constexpr size_t count = sizeof(routes) / sizeof(routes[0]);
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, 0));
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, 1));
    TEST_ASSERT_TRUE(RouteTable::repeatsEarlier(routes, 2));
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, 3));
    TEST_ASSERT_EQ(firstRepeat(routes, count), 2u);
    return 0;
}

/** GET and POST on one URI are two routes, not a repeat. */
static int test_same_uri_two_methods()
{
    const WebRoute routes[] = {
        route("/api/time/settings", RouteMethod::Get),
        route("/api/time/settings", RouteMethod::Post),
        route("/api/time/settings", RouteMethod::Post),   // this one is a repeat
    };
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, 0));
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, 1));
    TEST_ASSERT_TRUE(RouteTable::repeatsEarlier(routes, 2));
    return 0;
}

/** Comparison is exact: a longer URI is a different URI. */
static int test_prefix_is_not_a_repeat()
{
    const WebRoute routes[] = {
        route("/api/status"),
        route("/api/status2"),           // next to its neighbour in the real table
        route("/pages/version.html"),
        route("/pages/version.html2"),
    };
    constexpr size_t count = sizeof(routes) / sizeof(routes[0]);
    TEST_ASSERT_EQ(firstRepeat(routes, count), RouteTable::kNone);
    return 0;
}

/** An entry with no URI or no handler is unusable, and two holes are not a repeat. */
static int test_invalid_entries()
{
    const WebRoute routes[] = {
        route("/api/status"),
        route(""),                        // empty URI
        route(nullptr),                   // no URI at all
        route("/api/version", RouteMethod::Get, nullptr),   // no handler
        route(nullptr, RouteMethod::Post),                  // a second hole: still not a repeat
    };
    TEST_ASSERT_TRUE(RouteTable::isValid(routes[0]));
    TEST_ASSERT_FALSE(RouteTable::isValid(routes[1]));
    TEST_ASSERT_FALSE(RouteTable::isValid(routes[2]));
    TEST_ASSERT_FALSE(RouteTable::isValid(routes[3]));
    TEST_ASSERT_FALSE(RouteTable::isValid(routes[4]));
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, 4));   // two holes are not a repeat
    return 0;
}

/** The table itself must be there: a null pointer is not a crash. */
static int test_null_table()
{
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(nullptr, 5));
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(nullptr, 0));
    const WebRoute routes[] = { route("/api/status") };
    TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(routes, 0));   // nothing before the first
    return 0;
}

/** The HTTP server's limit is the table's size — not one less, not one more. */
static int test_handler_slots_are_derived()
{
    TEST_ASSERT_EQ(RouteTable::slotsFor(0), 0u);
    TEST_ASSERT_EQ(RouteTable::slotsFor(1), 1u);
    TEST_ASSERT_EQ(RouteTable::slotsFor(82), 82u);

    // The shape of the real table: many routes, one slot each. The URI strings
    // are built FIRST and completely: `c_str()` pointers into a vector that
    // still grows would be dangling, and the test would compare freed memory
    // (which is exactly what it did before this loop was split in two).
    vector<string> uris;
    for (int i = 0; i < 82; i++) {
        uris.push_back("/api/route" + to_string(i));
    }
    vector<WebRoute> many;
    for (const string& uri : uris) {
        many.push_back(route(uri.c_str()));
    }
    for (size_t i = 0; i < many.size(); i++) {
        TEST_ASSERT_FALSE(RouteTable::repeatsEarlier(many.data(), i));
    }
    TEST_ASSERT_EQ(RouteTable::slotsFor(many.size()), many.size());
    TEST_ASSERT_EQ(many.size(), 82u);
    return 0;
}

/**
 * The defect of stage 136, as a rule: with a table of N routes and a limit of
 * N − 1, the route registered LAST is the one that finds no slot. That is why
 * `slotsFor` is the table's own size, and why the server reports the result.
 */
static int test_limit_smaller_than_table_loses_the_last_route()
{
    const size_t routes = 82;
    const size_t limitThatWasThere = 81;          // the constant this stage removed
    TEST_ASSERT_TRUE(limitThatWasThere < routes);
    TEST_ASSERT_EQ(RouteTable::slotsFor(routes), routes);
    TEST_ASSERT_TRUE(RouteTable::slotsFor(routes) > limitThatWasThere);
    return 0;
}

/**
 * Every file of the web interface must have a route.
 *
 * A page that exists in `data/` but has no route answers 404 while every other
 * page works — exactly the defect that hid in stage 132 (the allow-list page was
 * created, linked in the menu and backed by REST, but its route was never
 * registered) and survived stage 137, which moved the old list into the table row
 * for row. Nothing else notices it: the mock server serves files itself, so the
 * browser check cannot see it, and the firmware registers explicit routes only
 * (no wildcard matching).
 *
 * The check reads the project tree — the routes from `src/web/WebServer.cpp`, the
 * files from `data/` — so it needs both next to the test's working directory. A
 * bare copy of this module has neither: then it says so and passes, because a
 * check that cannot look must not pretend that it did.
 */
static int test_every_web_file_has_a_route()
{
    const string src = readWholeFile(kRoutesFile);
    if (src.empty()) {
        printf("SKIP: %s not found — run this test from the project root\n", kRoutesFile);
        return 0;
    }

    // A table row is `{ "/path", ...` at the start of a line (after indentation);
    // the rest of the file — comments, log strings — is not mistaken for a route.
    vector<string> routes;
    size_t pos = 0;
    while (pos < src.size()) {
        const size_t eol = src.find('\n', pos);
        const string line = src.substr(pos, (eol == string::npos)
                                              ? string::npos : eol - pos);
        pos = (eol == string::npos) ? src.size() : eol + 1;

        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) b++;
        if (line.compare(b, 3, "{ \"") != 0) continue;
        const size_t uriStart = b + 3;
        const size_t uriEnd = line.find('"', uriStart);
        if (uriEnd != string::npos) {
            routes.push_back(line.substr(uriStart, uriEnd - uriStart));
        }
    }

    if (routes.empty()) {
        printf("SKIP: no routes parsed from %s — is that the server source?\n", kRoutesFile);
        return 0;
    }

    vector<string> files;
    collectFiles(kDataDir, "", files);
    TEST_ASSERT_TRUE(!files.empty());        // data/ must exist when the routes do

    size_t missing = 0;
    for (const string& path : files) {
        bool found = false;
        for (const string& route : routes) {
            if (route == path) { found = true; break; }
        }
        if (!found) {
            printf("FAIL: %s has no route — it will answer 404\n", path.c_str());
            missing++;
        }
    }
    TEST_ASSERT_EQ(missing, 0u);
    return 0;
}

/**
 * Every path of the web interface must fit the SPIFFS object-name limit.
 *
 * `CONFIG_SPIFFS_OBJ_NAME_LEN = 32` counts the terminating NUL, so at most 31
 * characters *including* the leading slash can be opened at runtime. A longer
 * path is written into the image by `spiffsgen.py` without a word of complaint,
 * so the build is happy and the file is really there — and then the same file
 * breaks in two ways at once: its page answers 404 (`staticFileHandler` cannot
 * open it) and its upload over `POST /api/web/file` fails with "Cannot open
 * file", which the web interface shows as the opaque "1 files failed". It
 * happened twice: `/pages/dhcp_static_bindings.html` (32 chars, 2026-08-09,
 * renamed to `/pages/dhcp_static.html`) and `/pages/security_certificates.html`
 * (33 chars, stage 160 — the lesson of 2026-08-09 forgotten, renamed to
 * `/pages/certs.html`).
 *
 * Nothing else in the project can see this: the mock server serves the files
 * from the host, the route table matches by name rather than by length, and the
 * device only reports the two failures above, which name the path but not the
 * reason.
 */
static int test_web_paths_fit_the_spiffs_name_limit()
{
    const size_t maxLen = spiffsMaxPathLen();
    if (maxLen == 0) {
        printf("SKIP: no sdkconfig with CONFIG_SPIFFS_OBJ_NAME_LEN — run this test from the project root\n");
        return 0;
    }

    vector<string> files;
    collectFiles(kDataDir, "", files);
    TEST_ASSERT_TRUE(!files.empty());        // data/ must exist when the routes do

    size_t tooLong = 0;
    for (const string& path : files) {
        if (path.size() > maxLen) {
            printf("FAIL: %s (%u chars) is longer than SPIFFS opens (%u) — it will 404 and refuse an upload\n",
                   path.c_str(), (unsigned)path.size(), (unsigned)maxLen);
            tooLong++;
        }
    }
    TEST_ASSERT_EQ(tooLong, 0u);
    return 0;
}

void app_main()
{
    printf("Running RouteTable tests...\n");
    int failures = 0;

    failures += test_clean_table();
    failures += test_repeat_is_found();
    failures += test_same_uri_two_methods();
    failures += test_prefix_is_not_a_repeat();
    failures += test_invalid_entries();
    failures += test_null_table();
    failures += test_handler_slots_are_derived();
    failures += test_limit_smaller_than_table_loses_the_last_route();
    failures += test_every_web_file_has_a_route();
    failures += test_web_paths_fit_the_spiffs_name_limit();

    if (failures == 0) {
        printf("All RouteTable tests PASSED!\n");
    } else {
        printf("Some RouteTable tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#else   // !DHCP_TEST_HOST

#include <cstdio>

extern "C" void app_main()
{
    printf("test_routetable is a host-only test (build with -DDHCP_TEST_HOST)"
           " - see the file header.\n");
}

#endif  // DHCP_TEST_HOST
