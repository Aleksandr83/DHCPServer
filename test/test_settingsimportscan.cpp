/**
 * @file test_settingsimportscan.cpp
 * @brief Unit tests for the rule that separates a *key* of an imported settings
 *        document from an ordinary string value (stage 183b).
 *
 * `POST /api/settings/import` tells the operator which parts of the file were not
 * imported because this firmware does not know them. That list decides what the
 * operator is going to fix by hand, so a wrong entry is expensive in both
 * directions: a missing one hides a setting that was silently dropped, and a
 * false one sends the operator after a parameter that is perfectly fine.
 *
 * The scan cannot be guessed from the JSON grammar alone — the import reads the
 * body as text, so a key has to be told apart from a value that ends in a quote
 * and a colon. A key follows '{' or ','; a value follows ':'. These tests pin that
 * difference, which is the whole of `SettingsImportScan.h`.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_settingsimportscan.cpp -o test_settingsimportscan
 */
#include "src/web/SettingsImportScan.h"

#include <cstdio>
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

static string join(const vector<string>& names)
{
    string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    return out;
}

static vector<string> unknown(const string& body, const vector<string>& known)
{
    return dhcp::web::unknownSettingsKeys(body, known);
}

/** A key the firmware does not know is what the report is for. */
static void test_an_unknown_key_is_named()
{
    printf("an unknown key\n");
    const vector<string> keys = unknown(
        "{\"format\":\"dhcpserver-settings\",\"future_section\":1}", {"format"});
    check(keys.size() == 1 && keys[0] == "future_section",
          "the section this firmware has never heard of is named: " + join(keys));
    const vector<string> both = unknown(
        "{\"format\":\"dhcpserver-settings\",\"future_section\":{\"x\":1}}",
        {"format"});
    check(both.size() == 2 && both[0] == "future_section" && both[1] == "x",
          "and so is what it holds: " + join(both));
}

/** Everything the table holds stays out of the report. */
static void test_known_keys_are_not_reported()
{
    printf("known keys\n");
    const vector<string> known = {"format", "dhcp", "dns", "enabled", "name",
                                  "ip4", "local_hosts"};
    check(unknown("{\"format\":\"dhcpserver-settings\",\"dhcp\":{\"enabled\":true},"
                  "\"dns\":{\"enabled\":false}}", known).empty(),
          "the format marker and the sections are not reported");
    check(unknown("{\"local_hosts\":[{\"name\":\"nas\",\"ip4\":\"10.0.0.5\"}]}",
                  known).empty(),
          "nor are the fields of a list entry");
}

/** A key unknown to this firmware, at any depth, is reported. */
static void test_nested_unknown_key_is_named()
{
    printf("a nested unknown key\n");
    const vector<string> keys = unknown(
        "{\"dhcp\":{\"enabled\":true,\"reservation_pool\":3}}", {"dhcp", "enabled"});
    check(keys.size() == 1 && keys[0] == "reservation_pool",
          "a field inside a known section is still a field: " + join(keys));
}

/**
 * The defect the scan exists for: a *value* is not a key. The old report looked at
 * every quoted token followed by ':', so `"name":"a: b"` made it name `a` — a
 * parameter that is not in the file at all.
 */
static void test_a_value_is_not_a_key()
{
    printf("a value is not a key\n");
    const vector<string> known = {"dhcp", "name"};
    check(unknown("{\"dhcp\":{\"name\":\"a: b\"}}", known).empty(),
          "a colon inside a value does not make a key out of the value");
    check(unknown("{\"dhcp\":{\"name\":\"Bedroom: TV\"}}", known).empty(),
          "and neither does a colon in the middle of a name");
    check(unknown("{\"dhcp\":{\"name\":\"  spaced  \"}}", known).empty(),
          "leading and trailing spaces of a value are still part of the value");
    // The case that used to be reported as an unknown field: a value holding an
    // escaped quote and a colon looks like a key pair to a scan that ignores
    // escapes.
    check(unknown("{\"dhcp\":{\"name\":\"TV \\\"bedroom\\\": 2\"}}", known).empty(),
          "an escaped quote inside a value does not make a field out of it");
}

/** A value holding quotes cannot fake a key pair. */
static void test_a_value_cannot_fake_a_key()
{
    printf("a value that tries to look like a key\n");
    const vector<string> known = {"dhcp", "name"};
    const vector<string> keys = unknown(
        "{\"dhcp\":{\"name\":\"x\\\",\\\"evil\\\":1\"}}", known);
    check(keys.empty(),
          "an escaped quote inside a value does not end it: " + join(keys));
    const string raw = "{\"dhcp\":{\"name\":\"{\\\"evil\\\":1}\"}}";
    check(unknown(raw, known).empty(),
          "an object written inside a value is a value: " + join(unknown(raw, known)));
}

/** An object written as a value is not a key, but a key of a real object is. */
static void test_arrays_of_objects()
{
    printf("objects inside arrays\n");
    const vector<string> known = {"local_hosts", "name", "ip4"};
    check(unknown("{\"local_hosts\":[{\"name\":\"nas\",\"ip4\":\"10.0.0.5\"}]}",
                  known).empty(),
          "the fields of an entry are known");
    const vector<string> keys = unknown(
        "{\"local_hosts\":[{\"name\":\"nas\",\"ip4\":\"10.0.0.5\",\"ttl\":60}]}",
        known);
    check(keys.size() == 1 && keys[0] == "ttl",
          "a field the table does not hold is reported: " + join(keys));
}

/** Whitespace between the key, its ':' and the braces is JSON's business. */
static void test_whitespace_and_newlines()
{
    printf("whitespace\n");
    check(unknown("{\n  \"future\" : {\n    \"x\" : 1\n  }\n}",
                  {"future", "x"}).empty(),
          "a key surrounded by newlines is still a key");
    const vector<string> keys = unknown("{ \"future\"\t:\n1 }", {});
    check(keys.size() == 1 && keys[0] == "future",
          "space before the colon does not hide a key: " + join(keys));
}

/** One name, one entry: the report is a list the operator reads. */
static void test_duplicates_are_named_once()
{
    printf("duplicates\n");
    const vector<string> keys = unknown("{\"a\":1,\"a\":2,\"b\":3}", {});
    check(keys.size() == 2 && keys[0] == "a" && keys[1] == "b",
          "a key met twice is reported once, in order: " + join(keys));
}

/** A body the import would never apply must not invent anything either. */
static void test_broken_bodies()
{
    printf("bodies that are not usable\n");
    check(unknown("", {}).empty(), "an empty body has no keys");
    check(unknown("{\"name\":\"unterminated", {}).size() == 1,
          "a cut value ends the scan after the keys it did read");
    check(unknown("{\"a\":\"\\\"}", {}).size() == 1,
          "a value ending in an escaped quote is not a value that is closed");
    check(unknown("{\"a\":", {}).size() == 1,
          "a key with nothing after it was still seen");
    check(unknown("not json at all", {}).empty(),
          "text that is not JSON carries no keys");
}

/** The list itself is the answer: order is the file's, and it is not sorted. */
static void test_order_of_the_file()
{
    printf("order\n");
    const vector<string> keys = unknown("{\"z\":1,\"a\":2}", {});
    check(keys.size() == 2 && keys[0] == "z" && keys[1] == "a",
          "the report follows the file, not the alphabet: " + join(keys));
}

/** What counts as space inside the document. */
static void test_what_counts_as_space()
{
    printf("space between tokens\n");
    check(dhcp::web::isJsonSpace(' '), "a space separates");
    check(dhcp::web::isJsonSpace('\t'), "so does a tab");
    check(dhcp::web::isJsonSpace('\n'), "and a newline");
    check(dhcp::web::isJsonSpace('\r'), "and a carriage return");
    check(!dhcp::web::isJsonSpace(':'), "a colon is not space");
    check(!dhcp::web::isJsonSpace('\0'), "nor is the end of the string");
    check(!dhcp::web::isJsonSpace(static_cast<char>(0xA0)),
          "nor is a byte above 0x7F (no locale is consulted)");
}

int main()
{
    test_an_unknown_key_is_named();
    test_known_keys_are_not_reported();
    test_nested_unknown_key_is_named();
    test_a_value_is_not_a_key();
    test_a_value_cannot_fake_a_key();
    test_arrays_of_objects();
    test_whitespace_and_newlines();
    test_duplicates_are_named_once();
    test_broken_bodies();
    test_order_of_the_file();
    test_what_counts_as_space();

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
