/**
 * @file test_settingskeys.cpp
 * @brief Unit test for the three readers of the settings document agreeing
 *        (rule 23, stage 214).
 *
 * `GET /api/settings/export` writes a key, `kKnownSettingsKeys` recognises it, and
 * `POST /api/settings/import` applies it. Three places, one document, and nothing
 * in the compiler keeps them in step: a setting written by the export and forgotten
 * by the import is not a compile error, it is a **silent loss** - the device answers
 * `"dns": true` and keeps the value it already had.
 *
 * That is not hypothetical. It was the state of the statistics autosave on
 * 16.10.2026: `stats_autosave`, `stats_autosave_period` and
 * `stats_autosave_interval` were written by the export and listed as known, and the
 * import never read them, so restoring a backup left the device with whatever
 * autosave it happened to have. Nothing said so, twice over: the whitelist entry
 * made the keys look handled, and `warnings` only speaks about keys the firmware
 * does **not** claim to know. This test is the guard that was missing.
 *
 * It reads `src/web/RestApi.cpp` itself - the same way `test_routetable` reads
 * `WebServer.cpp` - because the defect it guards against is a disagreement between
 * two functions in that file. Three checks, one per way they can drift:
 *
 *   1. every key the export writes is in `kKnownSettingsKeys` - a key the device
 *      writes but does not recognise makes it report its own file as holding
 *      unknown fields;
 *   2. every key the export writes is read by the import - otherwise the setting
 *      is dropped in silence;
 *   3. every key in `kKnownSettingsKeys` appears in the export - otherwise the
 *      table has rotted into names nothing writes.
 *
 * The envelope (`format`, `schema`, `firmware_version`) and the section names are
 * exempt from check 2, and only from it: they are the document's own shape, not
 * settings - the import validates the marker, compares `firmware_version` by
 * release, leaves `schema` informational, and looks a section up before reading the
 * fields inside it. All of them are checked against the whitelist like the rest.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_settingskeys.cpp -o test_settingskeys
 */
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

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

static string readFile(const char* path)
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

/** The body of one `esp_err_t RestApi::…(httpd_req* req)` function. */
static string functionBody(const string& text, const string& signature)
{
    const size_t start = text.find(signature);
    if (start == string::npos) return string();
    const size_t next = text.find("\nesp_err_t RestApi::", start + signature.size());
    return text.substr(start, next == string::npos ? string::npos : next - start);
}

/**
 * @brief Every key the export writes, from the two spellings it uses.
 *
 * Values written through `addJsonString()`/`addJsonBool()`/`addJsonInt()` carry the
 * key as a plain quoted string; the three hand-built arrays (`static_bindings`,
 * `allowed_computers`, `local_hosts`) write theirs escaped, inside a longer literal.
 * Both spellings are read here, because a key missed by this test is a key that
 * escapes the whole check.
 */
static vector<string> exportKeys(const string& body)
{
    vector<string> out;
    auto push = [&out](const string& key) {
        for (const string& seen : out) {
            if (seen == key) return;
        }
        out.push_back(key);
    };

    // Values written through addJsonString()/addJsonBool()/addJsonInt(): the key is
    // the call's first argument, a plain quoted string, and the call always begins
    // `addJson…(json, "key",`. That shape is required here rather than "the first
    // quoted string after a comma": the word `addJson…` also appears in the prose of
    // comments, and a comma in a sentence followed by a quote in some later literal
    // would invent a key out of everything between them.
    for (size_t p = body.find("addJson"); p != string::npos; p = body.find("addJson", p + 1)) {
        const size_t open = body.find('(', p);
        if (open == string::npos || body.compare(open + 1, 5, "json,") != 0) continue;
        size_t i = open + 6;
        while (i < body.size() && isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= body.size() || body[i] != '"') continue;
        const size_t start = ++i;
        while (i < body.size() && body[i] != '"') ++i;
        if (i >= body.size() || body.compare(i + 1, 1, ",") != 0) continue;
        push(body.substr(start, i - start));
    }

    // The three hand-built arrays write their keys *escaped*, inside a longer
    // literal (`json += "{\"mac\":\"…"`), so a key is the sequence `\"name\":` -
    // and the colon is what tells a key from a value, which is the same rule the
    // import's own scanner follows (`SettingsImportScan.h`, stage 183b).
    for (size_t p = body.find("\\\""); p != string::npos; p = body.find("\\\"", p + 2)) {
        size_t i = p + 2;
        string key;
        while (i < body.size() &&
               (isalnum(static_cast<unsigned char>(body[i])) || body[i] == '_')) {
            key += body[i++];
        }
        if (!key.empty() && body.compare(i, 3, "\\\":") == 0) push(key);
    }
    return out;
}

/** The keys of `kKnownSettingsKeys[]`, in the order the table lists them. */
static vector<string> whitelistKeys(const string& text)
{
    vector<string> out;
    const size_t table = text.find("kKnownSettingsKeys[]");
    if (table == string::npos) return out;
    const size_t open = text.find('{', table);
    const size_t close = text.find("};", open);
    if (open == string::npos || close == string::npos) return out;

    const string body = text.substr(open, close - open);
    for (size_t p = body.find('"'); p != string::npos; p = body.find('"', p + 1)) {
        const size_t end = body.find('"', p + 1);
        if (end == string::npos) break;
        out.push_back(body.substr(p + 1, end - p - 1));
        p = end;
    }
    return out;
}

static bool contains(const vector<string>& list, const string& key)
{
    for (const string& item : list) {
        if (item == key) return true;
    }
    return false;
}

/** Does this text mention the key as a JSON key (plain or inside a literal)? */
static bool mentionsKey(const string& text, const string& key)
{
    if (text.find("\"" + key + "\"") != string::npos) return true;
    return text.find("\\\"" + key + "\\\"") != string::npos;
}

/**
 * @brief Is the key an argument of a reader in the import?
 *
 * Mere mention is not enough: the import also names fields it *writes* into its
 * answer (`skipped_fields`, the per-section flags), and a mention of those would
 * make this check answer the wrong question. So the key has to sit inside a
 * `jsonGet…(…)` call, which is what "the import reads it" means.
 */
static bool readByImport(const string& body, const string& key)
{
    const string needle = "\"" + key + "\"";
    for (size_t p = body.find(needle); p != string::npos; p = body.find(needle, p + 1)) {
        const size_t window = p < 120 ? p : 120;
        const string before = body.substr(p - window, window);
        if (before.find("jsonGet") != string::npos) return true;
    }
    return false;
}

static string joined(const vector<string>& keys)
{
    string out;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i > 0) out += ", ";
        out += keys[i];
    }
    return out;
}

int main()
{
    printf("Running SettingsKeys tests...\n");

    const string text = readFile("src/web/RestApi.cpp");
    if (text.empty()) {
        printf("  FAIL src/web/RestApi.cpp could not be read (run me from the "
               "repository root)\n");
        printf("1 of 1 SettingsKeys checks FAILED!\n");
        return 1;
    }

    const string exportBody =
        functionBody(text, "esp_err_t RestApi::handleGetSettingsExport(");
    const string importBody =
        functionBody(text, "esp_err_t RestApi::handlePostSettingsImport(");
    const vector<string> exported = exportKeys(exportBody);
    const vector<string> known = whitelistKeys(text);

    check(!exportBody.empty() && !importBody.empty() && !exported.empty() && !known.empty(),
          "the export, the import and the key table were all found in RestApi.cpp");

    // 1. A key the device writes must be one it recognises.
    vector<string> unknown;
    for (const string& key : exported) {
        if (!contains(known, key)) unknown.push_back(key);
    }
    check(unknown.empty(),
          "every key the export writes is in kKnownSettingsKeys"
          + (unknown.empty() ? string() : " (missing: " + joined(unknown) + ")"));

    // 2. A key the device writes must be one it applies on import. The document's
    //    own shape is not a setting: the envelope is validated rather than applied
    //    (`format` is the marker, `firmware_version` is compared by release, `schema`
    //    is informational), and a section name is a place the import *looks for*
    //    before it reads the fields inside it.
    const char* const kStructural[] = {
        "format", "schema", "firmware_version",
        "dhcp", "static_bindings", "allowed_computers", "dns", "time",
        "local_hosts", "security", "files", "device", "ssh", "device_log",
    };
    vector<string> ignored;
    for (const string& key : exported) {
        bool isStructural = false;
        for (const char* structural : kStructural) {
            if (key == structural) isStructural = true;
        }
        if (isStructural) continue;
        if (!readByImport(importBody, key)) ignored.push_back(key);
    }
    check(ignored.empty(),
          "every setting the export writes is read by the import"
          + (ignored.empty() ? string() : " (ignored: " + joined(ignored) + ")"));

    // 3. And the other way round: a name nothing writes is a name that has rotted.
    vector<string> stale;
    for (const string& key : known) {
        if (!mentionsKey(exportBody, key)) stale.push_back(key);
    }
    check(stale.empty(),
          "every key in kKnownSettingsKeys is written by the export"
          + (stale.empty() ? string() : " (stale: " + joined(stale) + ")"));

    if (g_failed == 0) {
        printf("All %d SettingsKeys checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d SettingsKeys checks FAILED!\n", g_failed, g_checks);
    return 1;
}
