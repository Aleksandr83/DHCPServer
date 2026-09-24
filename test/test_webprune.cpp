/**
 * @file test_webprune.cpp
 * @brief Unit tests for the rule that keeps the device's web tree equal to an
 *        uploaded folder (stage 169).
 *
 * The interface is updated from a folder, one file per request, and that path only
 * ever writes — so a page dropped from `data/` stayed on the device for good. The
 * prune is the other half: the device is told which files the folder holds and
 * removes the rest. This is the part that decides what gets **deleted**, and a
 * wrong decision here strips files off a device that may be a thousand kilometres
 * away, which is why the rule is free of ESP-IDF and checked on the host.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_webprune.cpp src/web/WebPrune.cpp -o test_webprune
 */
#include "src/web/WebPrune.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace std;

using dhcp::web::WebPrune;

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

static vector<string> deviceTree()
{
    return { "index.html", "login.html", "header.html", "footer.html",
             "css/style.css", "js/app.js", "i18n/ru.json", "i18n/en.json",
             "pages/certs.html", "pages/dhcp_static.html" };
}

/** A folder that holds everything the device holds leaves the device alone. */
static void test_equal_trees_remove_nothing()
{
    printf("equal trees\n");
    const vector<string> tree = deviceTree();
    check(WebPrune::extra(tree, tree).empty(), "the same list on both sides means nothing to remove");
}

/** A page that the folder no longer has is the whole reason the prune exists. */
static void test_a_dropped_file_is_named()
{
    printf("a dropped file\n");
    vector<string> uploaded = deviceTree();
    uploaded.erase(uploaded.begin() + 8);   // pages/certs.html is gone from the folder
    const vector<string> extra = WebPrune::extra(deviceTree(), uploaded);
    check(extra.size() == 1 && extra[0] == "pages/certs.html",
          "the file the folder dropped is the one to remove: " + join(extra));
}

/** What was uploaded from a wrong folder is exactly what has to go. */
static void test_foreign_files_are_named()
{
    printf("files that are not the interface\n");
    vector<string> onDevice = deviceTree();
    onDevice.push_back("esptool.exe");
    onDevice.push_back("openocd.exe");
    const vector<string> extra = WebPrune::extra(onDevice, deviceTree());
    check(extra.size() == 2 && extra[0] == "esptool.exe" && extra[1] == "openocd.exe",
          "everything the folder does not hold goes: " + join(extra));
}

/** The volume is case-sensitive, so the folder's spelling is the one that wins. */
static void test_case_is_respected()
{
    printf("case\n");
    const vector<string> extra =
        WebPrune::extra({"Index.html"}, {"index.html"});
    check(extra.size() == 1 && extra[0] == "Index.html",
          "Index.html is not index.html on this volume: " + join(extra));
}

/** The mount point and a leading slash are the same name. */
static void test_names_are_normalised()
{
    printf("names\n");
    check(WebPrune::normalise("/pages/x.html") == "pages/x.html", "a leading slash is dropped");
    check(WebPrune::normalise("/spiffs/pages/x.html") == "pages/x.html",
          "the mount prefix is dropped");
    check(WebPrune::normalise("spiffs/index.html") == "index.html", "so is `spiffs/`");
    check(WebPrune::normalise("pages/x.html/") == "pages/x.html", "a trailing slash is dropped");
    check(WebPrune::extra({"/spiffs/pages/x.html"}, {"pages/x.html"}).empty(),
          "and the two spellings describe one file");
}

/** The answer keeps the volume's own spelling and order, so the caller can act on it. */
static void test_order_and_spelling_of_the_volume()
{
    printf("volume spelling and order\n");
    const vector<string> extra = WebPrune::extra(
        {"z.html", "/pages/a.html", "b.html"}, {"b.html"});
    check(extra.size() == 2 && extra[0] == "z.html" && extra[1] == "/pages/a.html",
          "the names come back as the volume reports them: " + join(extra));
}

/** A folder that lists one file twice is still a folder with that file. */
static void test_duplicates_are_harmless()
{
    printf("duplicates\n");
    check(WebPrune::extra({"a.html"}, {"a.html", "a.html"}).empty(),
          "a name listed twice in the folder is still just a name");
}

/** An empty folder list means "everything", which is why the route refuses one. */
static void test_an_empty_list_means_everything()
{
    printf("an empty list\n");
    const vector<string> extra = WebPrune::extra({"a.html", "b.html"}, {});
    check(extra.size() == 2, "an empty folder would remove the whole tree — the caller's business");
}

/** The name rule is the upload's own: a list that could not have been uploaded is refused. */
static void test_acceptable_names()
{
    printf("what a web-file name may be\n");
    check(WebPrune::isAcceptableName("index.html"), "the home page is fine");
    check(WebPrune::isAcceptableName("pages/dhcp_static.html"), "so is a nested page");
    check(WebPrune::isAcceptableName("css/style.css"), "and a stylesheet");
    check(WebPrune::isAcceptableName("i18n/ru.json"), "and a dictionary");
    check(!WebPrune::isAcceptableName(""), "an empty name is not");
    check(!WebPrune::isAcceptableName("/index.html"), "an absolute path is not (the upload writes relative)");
    check(!WebPrune::isAcceptableName("pages//x.html"), "an empty segment is not");
    check(!WebPrune::isAcceptableName("pages/./x.html"), "a `.` segment is not");
    check(!WebPrune::isAcceptableName("pages/../x.html"), "and neither is `..`");
    check(!WebPrune::isAcceptableName("a b.html"), "a space is not a web-file character");
    check(!WebPrune::isAcceptableName("a\"b.html"), "a quote is not either");
    check(!WebPrune::isAcceptableName("web_ui_spiffs.bin "), "a trailing space is refused");
    check(WebPrune::isAcceptableName(string(WebPrune::kMaxNameLen, 'a')),
          "a name of exactly the limit passes");
    check(!WebPrune::isAcceptableName(string(WebPrune::kMaxNameLen + 1, 'a')),
          "one byte more is refused");
}

int main()
{
    test_equal_trees_remove_nothing();
    test_a_dropped_file_is_named();
    test_foreign_files_are_named();
    test_case_is_respected();
    test_names_are_normalised();
    test_order_and_spelling_of_the_volume();
    test_duplicates_are_harmless();
    test_an_empty_list_means_everything();
    test_acceptable_names();

    printf("\n%d checks\n", g_checks);
    if (g_failed == 0) {
        printf("PASSED!\n");
        return 0;
    }
    printf("FAILED (%d)\n", g_failed);
    return 1;
}
