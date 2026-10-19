/**
 * @file test_filebrowser.cpp
 * @brief Unit tests for the two-pane SSH text browser (stage 225).
 *
 * `FileBrowser` is the whole screen minus the terminal: it takes keys, keeps two
 * panels, asks the volume through `IFileOperations`, and renders plain text lines.
 * That makes every rule of the screen checkable here — the cursor, the panes, the
 * volume switch, the prompts of F4..F8, the viewer and the sandbox — while the SSH
 * session only moves bytes.
 *
 * There is nothing to compare against in the past: the header did not exist before
 * this change, so the test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_filebrowser.cpp host_main.cpp -o test_filebrowser
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../src/net/FileBrowser.h"
#include "../src/net/IFileOperations.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)
#define TEST_ASSERT_HAS(haystack, needle) do { \
        const string _h = (haystack); const string _n = (needle); \
        if (_h.find(_n) == string::npos) { printf("FAIL: %s:%d: \"%s\" does not hold \"%s\"\n", \
                                                  __FILE__, __LINE__, _h.c_str(), _n.c_str()); return 1; } \
    } while(0)

using namespace dhcp::net;

namespace {

/** A volume in memory: paths are directories or hold text. */
class FakeVolume : public IFileOperations {
public:
    set<string> dirs { "/fat", "/sdcard" };
    map<string, string> files;

    FileOpStatus attributes(const string& path, bool& exists, bool& isDir,
                            uint64_t& size) override
    {
        exists = dirs.count(path) > 0 || files.count(path) > 0;
        isDir = dirs.count(path) > 0;
        size = isDir ? 0 : (files.count(path) ? files[path].size() : 0);
        return FileOpStatus::Ok;
    }

    FileOpStatus list(const string& path, vector<FileEntry>& out) override
    {
        if (!dirs.count(path)) return FileOpStatus::NotFound;
        const string prefix = path + "/";
        for (const string& d : dirs) {
            if (d.size() > prefix.size() && d.compare(0, prefix.size(), prefix) == 0 &&
                d.find('/', prefix.size()) == string::npos) {
                out.push_back({ d.substr(prefix.size()), true, 0 });
            }
        }
        for (const auto& f : files) {
            if (f.first.size() > prefix.size() &&
                f.first.compare(0, prefix.size(), prefix) == 0 &&
                f.first.find('/', prefix.size()) == string::npos) {
                out.push_back({ f.first.substr(prefix.size()), false, f.second.size() });
            }
        }
        return FileOpStatus::Ok;
    }

    FileOpStatus makeDir(const string& path) override
    {
        if (dirs.count(path) || files.count(path)) return FileOpStatus::Exists;
        dirs.insert(path);
        return FileOpStatus::Ok;
    }

    FileOpStatus touch(const string& path, bool& created) override
    {
        created = false;
        if (dirs.count(path) || files.count(path)) return FileOpStatus::Ok;
        files[path] = "";
        created = true;
        return FileOpStatus::Ok;
    }

    FileOpStatus removeFile(const string& path) override
    {
        if (dirs.count(path)) return FileOpStatus::IsDirectory;
        return files.erase(path) ? FileOpStatus::Ok : FileOpStatus::NotFound;
    }

    FileOpStatus removeTree(const string& path) override
    {
        if (!dirs.count(path) && !files.count(path)) return FileOpStatus::NotFound;
        const string prefix = path + "/";
        for (auto it = files.begin(); it != files.end();) {
            it = (it->first.compare(0, prefix.size(), prefix) == 0) ? files.erase(it) : next(it);
        }
        for (auto it = dirs.begin(); it != dirs.end();) {
            it = (*it == path || it->compare(0, prefix.size(), prefix) == 0) ? dirs.erase(it) : next(it);
        }
        return FileOpStatus::Ok;
    }

    FileOpStatus rename(const string& from, const string& to) override
    {
        const auto it = files.find(from);
        if (it == files.end()) return FileOpStatus::NotFound;
        files[to] = it->second;
        files.erase(it);
        return FileOpStatus::Ok;
    }

    FileOpStatus copy(const string& from, const string& to) override
    {
        const auto it = files.find(from);
        if (it == files.end()) return FileOpStatus::NotFound;
        files[to] = it->second;
        return FileOpStatus::Ok;
    }

    FileOpStatus readText(const string& path, uint64_t limit, string& out,
                          bool& binary) override
    {
        binary = false;
        const auto it = files.find(path);
        if (it == files.end()) return FileOpStatus::NotFound;
        if (it->second.size() > limit) return FileOpStatus::NoSpace;
        out = it->second;
        return FileOpStatus::Ok;
    }

    FileSpace space(const string& volumeRoot) override
    {
        FileSpace s;
        s.mounted = dirs.count(volumeRoot) > 0;
        s.totalBytes = 8 * 1024 * 1024;
        s.freeBytes = 4 * 1024 * 1024;
        return s;
    }
};

/// @brief 60 columns, 12 rows: small enough that a whole screen fits in a test.
struct Fixture {
    FakeVolume fs;
    /// Built on first use, so a test can fill the volume before the screen opens
    /// its panels — the real browser reads a directory once and then on demand.
    std::unique_ptr<FileBrowser> browser;

    FileBrowser& b()
    {
        if (!browser) browser = std::unique_ptr<FileBrowser>(new FileBrowser(fs, 60, 12));
        return *browser;
    }

    string screen()
    {
        string out;
        for (const string& line : b().render()) {
            out += line;
            out += "\n";
        }
        return out;
    }

    void press(TuiKeyKind kind) { b().feed({ kind, 0 }); }
    void type(const string& text)
    {
        for (char c : text) b().feed({ TuiKeyKind::Char, c });
    }
};

/** The screen opens in /fat, both panels on it, the left one active. */
int test_the_first_screen()
{
    Fixture f;
    f.fs.files["/fat/cache.dat"] = "abc";

    const string screen = f.screen();
    TEST_ASSERT_HAS(screen, "* /fat");
    TEST_ASSERT_HAS(screen, "  /fat");
    TEST_ASSERT_HAS(screen, "cache.dat");
    // The hints name the keys the operator agreed on.
    TEST_ASSERT_HAS(screen, "F3");
    TEST_ASSERT_HAS(screen, "F8");
    return 0;
}

/** The cursor moves, and Tab moves between the panels instead of the volumes. */
int test_the_navigation()
{
    Fixture f;
    f.fs.files["/fat/a.txt"] = "a";
    f.fs.files["/fat/b.txt"] = "b";
    f.fs.dirs.insert("/fat/logs");

    TEST_ASSERT_HAS(f.screen(), "> a.txt");
    f.press(TuiKeyKind::Down);
    TEST_ASSERT_HAS(f.screen(), "> b.txt");
    f.press(TuiKeyKind::Down);
    f.press(TuiKeyKind::Down);           // past the last entry: stays there
    TEST_ASSERT_HAS(f.screen(), "> logs/");
    f.press(TuiKeyKind::Home);
    TEST_ASSERT_HAS(f.screen(), "> a.txt");

    f.press(TuiKeyKind::Tab);
    const string screen = f.screen();
    TEST_ASSERT_HAS(screen, "  /fat");
    // The right panel is the active one now: its first entry carries the marker.
    const size_t right = screen.find("  /fat");
    TEST_ASSERT_TRUE(screen.find('>', right) != string::npos);
    return 0;
}

/** Enter walks into a directory, Backspace walks back out, and the root holds. */
int test_the_directories()
{
    Fixture f;
    f.fs.dirs.insert("/fat/logs");
    f.fs.files["/fat/logs/SSH.log"] = "x";

    f.press(TuiKeyKind::Down);
    TEST_ASSERT_HAS(f.screen(), "> logs/");
    f.press(TuiKeyKind::Enter);
    TEST_ASSERT_HAS(f.screen(), "* /fat/logs");
    TEST_ASSERT_HAS(f.screen(), "SSH.log");

    f.press(TuiKeyKind::Backspace);
    TEST_ASSERT_HAS(f.screen(), "* /fat");

    f.press(TuiKeyKind::Backspace);      // at the volume root: refused, with a reason
    TEST_ASSERT_HAS(f.screen(), "cannot leave its volume");
    return 0;
}

/** Alt+F1 and Alt+F2 move the volumes of their own panels, not the active one. */
int test_the_volume_switch()
{
    Fixture f;
    f.press(TuiKeyKind::AltF1);
    string screen = f.screen();
    TEST_ASSERT_HAS(screen, "* /sdcard");
    TEST_ASSERT_HAS(screen, "  /fat");

    f.press(TuiKeyKind::AltF1);
    TEST_ASSERT_HAS(f.screen(), "* /fat");

    f.press(TuiKeyKind::AltF2);
    screen = f.screen();
    // The right panel is now on the card; the left one stays where it was and is
    // still the active one.
    TEST_ASSERT_HAS(screen, "* /fat");
    TEST_ASSERT_HAS(screen, "  /sdcard");
    return 0;
}

/** PgDn and PgUp move by a screenful, End and Home jump to the edges. */
int test_the_paging()
{
    Fixture f;
    for (int i = 0; i < 20; i++) {
        f.fs.files["/fat/f" + to_string(i) + ".txt"] = "x";
    }
    // The list is longer than the pane: the last entries are reachable.
    f.press(TuiKeyKind::End);
    TEST_ASSERT_HAS(f.screen(), "> f9.txt");
    f.press(TuiKeyKind::PageUp);
    TEST_ASSERT_TRUE(f.screen().find("> f9.txt") == string::npos);
    f.press(TuiKeyKind::PageDown);
    TEST_ASSERT_HAS(f.screen(), "> f9.txt");
    return 0;
}

/** F7 asks for a name and creates the directory where the panel stands. */
int test_the_new_directory()
{
    Fixture f;
    f.press(TuiKeyKind::F7);
    TEST_ASSERT_HAS(f.screen(), "New directory:");
    f.type("archive");
    f.press(TuiKeyKind::Enter);
    TEST_ASSERT_TRUE(f.fs.dirs.count("/fat/archive") > 0);
    TEST_ASSERT_HAS(f.screen(), "archive/");

    // Escape leaves the prompt without doing anything.
    f.press(TuiKeyKind::F7);
    f.type("ignored");
    f.press(TuiKeyKind::Escape);
    TEST_ASSERT_TRUE(f.fs.dirs.count("/fat/ignored") == 0);
    return 0;
}

/** F4 creates an empty file, F6 renames the selected one, F8 deletes it. */
int test_the_file_operations()
{
    Fixture f;
    f.fs.files["/fat/a.txt"] = "content";

    f.press(TuiKeyKind::F4);
    TEST_ASSERT_HAS(f.screen(), "New file:");
    f.type("b.txt");
    f.press(TuiKeyKind::Enter);
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/b.txt") > 0);

    f.press(TuiKeyKind::Home);
    f.press(TuiKeyKind::F6);
    TEST_ASSERT_HAS(f.screen(), "Rename to:");
    // The prompt starts with the current name in it: clear it, then type the new
    // one, as a person would.
    for (int i = 0; i < 5; i++) f.press(TuiKeyKind::Backspace);
    f.type("c.txt");
    f.press(TuiKeyKind::Enter);
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/a.txt") == 0);
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/c.txt") > 0);

    // The cursor sits on the first entry, which is now b.txt: F8 removes that one.
    f.press(TuiKeyKind::Home);
    f.press(TuiKeyKind::F8);
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/b.txt") == 0);
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/c.txt") > 0);
    return 0;
}

/** F5 copies the selected file into the other panel's directory. */
int test_the_copy()
{
    Fixture f;
    f.fs.files["/fat/a.txt"] = "payload";
    f.fs.dirs.insert("/fat/target");

    // The right panel creates a directory and walks into it: the copy is meant to
    // land in the directory the other panel stands in.
    f.press(TuiKeyKind::Tab);
    f.press(TuiKeyKind::F7);
    f.type("target");
    f.press(TuiKeyKind::Enter);
    TEST_ASSERT_TRUE(f.fs.dirs.count("/fat/target") > 0);
    f.press(TuiKeyKind::Home);
    f.press(TuiKeyKind::Down);           // "target/"
    f.press(TuiKeyKind::Enter);
    TEST_ASSERT_HAS(f.screen(), "* /fat/target");

    f.press(TuiKeyKind::Tab);            // back to the left panel
    f.press(TuiKeyKind::Home);
    f.press(TuiKeyKind::F5);
    // The prompt is pre-filled with the other panel's directory.
    TEST_ASSERT_HAS(f.screen(), "Copy to: /fat/target/");
    f.press(TuiKeyKind::Enter);
    TEST_ASSERT_STREQ(f.fs.files["/fat/target/a.txt"], "payload");
    return 0;
}

/** F3 shows a file, the arrows and the pages scroll it, F3 or q comes back. */
int test_the_viewer()
{
    Fixture f;
    string text;
    for (int i = 0; i < 40; i++) text += "line " + to_string(i) + "\n";
    f.fs.files["/fat/big.txt"] = text;
    // Both files exist before the panels are read: the browser lists a directory
    // when it opens it, not on every keystroke (there is no watch on the volume).
    f.fs.files["/fat/huge.bin"] = string(70 * 1024, 'x');

    f.press(TuiKeyKind::F3);
    TEST_ASSERT_HAS(f.screen(), "line 0");
    TEST_ASSERT_TRUE(f.screen().find("line 39") == string::npos);

    // PgDn moves by a screenful; End goes to the last one.
    f.press(TuiKeyKind::PageDown);
    TEST_ASSERT_HAS(f.screen(), "line 10");
    f.press(TuiKeyKind::End);
    TEST_ASSERT_HAS(f.screen(), "line 39");
    f.press(TuiKeyKind::Home);
    TEST_ASSERT_HAS(f.screen(), "line 0");

    f.press(TuiKeyKind::F3);             // back to the panels
    TEST_ASSERT_HAS(f.screen(), "* /fat");

    // A file that cannot be printed says so instead of showing rubble.
    f.press(TuiKeyKind::End);
    f.press(TuiKeyKind::F3);
    TEST_ASSERT_HAS(f.screen(), "error");
    return 0;
}

/// @brief A row with the terminal's escape sequences taken out.
string stripAnsi(const string& text)
{
    string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() && (text[i] < '@' || text[i] > '~')) i++;
            continue;
        }
        out += text[i];
    }
    return out;
}

/** The two panels are the same width, and the screen has no unused column. */
int test_the_panes_are_equal()
{
    // An odd and an even width, because the column left over must go to the divider
    // rather than make one panel a column wider than the other.
    for (int cols : { 60, 61, 80, 81 }) {
        FakeVolume fs;
        const string longName(200, 'x');
        fs.files["/fat/" + longName] = "1";
        FileBrowser b(fs, cols, 12);

        const vector<string> lines = b.render();
        TEST_ASSERT_TRUE(lines.size() == 12u);
        const string line = lines[1];        // header, then the first list row
        TEST_ASSERT_TRUE(static_cast<int>(line.size()) == cols);

        const size_t bar = line.find('|');
        TEST_ASSERT_TRUE(bar == static_cast<size_t>((cols - 1) / 2));
        // The name fills both panels down to their own edge: neither is padded, so
        // neither is narrower than the other.
        TEST_ASSERT_TRUE(line[bar - 1] == 'x');
        TEST_ASSERT_TRUE(line[static_cast<size_t>(cols) - 1] == 'x');
    }
    return 0;
}

/** The panels are blue as in mc, and the colour changes nothing else. */
int test_the_panels_are_blue()
{
    Fixture f;
    f.fs.files["/fat/a.txt"] = "a";
    f.fs.dirs.insert("/fat/logs");

    const vector<string> ansi = f.b().renderAnsi();
    const vector<string> plain = f.b().render();
    TEST_ASSERT_TRUE(ansi.size() == plain.size());
    for (size_t i = 0; i < ansi.size(); i++) {
        // Colour is invisible: taking the escapes out gives the plain screen back,
        // character for character.
        TEST_ASSERT_STREQ(stripAnsi(ansi[i]), plain[i]);
    }
    // The active panel's header, the cursor row and a directory each have their own
    // colour; the bar lines at the bottom carry none.
    TEST_ASSERT_HAS(ansi[0], "\x1b[44;1;37m");
    TEST_ASSERT_HAS(ansi[1], "\x1b[46;30m");
    TEST_ASSERT_HAS(ansi[2], "\x1b[44;1;36m");
    TEST_ASSERT_HAS(ansi[ansi.size() - 1], "F8");
    TEST_ASSERT_FALSE(ansi[ansi.size() - 1].find("\x1b[44") != string::npos);
    return 0;
}

/** A resized window is redrawn at the new size, panels included. */
int test_the_resize()
{
    Fixture f;
    f.fs.files["/fat/a.txt"] = "a";

    f.b().resize(100, 30);
    TEST_ASSERT_TRUE(f.b().cols() == 100);
    TEST_ASSERT_TRUE(f.b().rows() == 30);
    vector<string> lines = f.b().render();
    TEST_ASSERT_TRUE(lines.size() == 30u);
    TEST_ASSERT_TRUE(lines[0].size() == 100u);
    TEST_ASSERT_TRUE(lines[0].find('|') == static_cast<size_t>(49));

    f.b().resize(40, 8);
    lines = f.b().render();
    TEST_ASSERT_TRUE(lines.size() == 8u);
    TEST_ASSERT_TRUE(lines[0].size() == 40u);
    TEST_ASSERT_TRUE(lines[0].find('|') == static_cast<size_t>(19));

    // The panels are the same panels: the entries are still there after the change.
    TEST_ASSERT_HAS(f.screen(), "a.txt");
    return 0;
}

/** A name is data: an escape inside it never reaches the terminal. */
int test_the_names_are_printed_safely()
{
    Fixture f;
    f.fs.files[string("/fat/bad\x1b[31m.txt")] = "x";

    const string screen = f.screen();
    TEST_ASSERT_FALSE(screen.find('\x1b') != string::npos);
    TEST_ASSERT_HAS(screen, "bad?[31m.txt");
    return 0;
}

/** F10 leaves; `q` is a key like any other now, as in mc. */
int test_the_exit()
{
    Fixture f;
    f.b().resize(80, 12);
    // The hints have to name the key that actually leaves, and to fit the screen.
    TEST_ASSERT_HAS(f.screen(), "F10 Quit");

    TEST_ASSERT_FALSE(f.b().finished());
    f.b().feed({ TuiKeyKind::Char, 'q' });
    TEST_ASSERT_FALSE(f.b().finished());
    f.press(TuiKeyKind::F10);
    TEST_ASSERT_TRUE(f.b().finished());
    return 0;
}

/** F10 means "out" from the viewer and from a prompt too, not just from the panels. */
int test_the_exit_from_every_mode()
{
    {
        Fixture f;
        f.fs.files["/fat/a.txt"] = "hello";
        f.press(TuiKeyKind::F3);           // the viewer is open
        f.press(TuiKeyKind::F10);
        TEST_ASSERT_TRUE(f.b().finished());
    }
    {
        Fixture f;
        f.press(TuiKeyKind::F4);           // a prompt is open
        f.press(TuiKeyKind::F10);
        TEST_ASSERT_TRUE(f.b().finished());
    }
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running FileBrowser tests...\n");
    int failures = 0;

    failures += test_the_first_screen();
    failures += test_the_navigation();
    failures += test_the_directories();
    failures += test_the_volume_switch();
    failures += test_the_paging();
    failures += test_the_new_directory();
    failures += test_the_file_operations();
    failures += test_the_copy();
    failures += test_the_viewer();
    failures += test_the_panes_are_equal();
    failures += test_the_panels_are_blue();
    failures += test_the_resize();
    failures += test_the_names_are_printed_safely();
    failures += test_the_exit();
    failures += test_the_exit_from_every_mode();

    if (failures == 0) {
        printf("All FileBrowser tests PASSED!\n");
    } else {
        printf("Some FileBrowser tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
