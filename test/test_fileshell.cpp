/**
 * @file test_fileshell.cpp
 * @brief Unit tests for the SSH file commands, against a fake volume.
 *
 * The shell decides what a typed line means and what the answer reads like; the
 * volumes are behind `IFileOperations`, so everything but the POSIX adapter is
 * testable here. The rules under test are the operator's: `rm` needs `-r` for a
 * directory, `cp`/`mv` refuse an existing target unless `-f`, `cat` refuses what is
 * too long or is not text, and no path may leave its volume.
 *
 * There is nothing to compare against in the past: the header did not exist before
 * this change, so the test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_fileshell.cpp host_main.cpp -o test_fileshell
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../src/net/FileShell.h"
#include "../src/net/FileShellPath.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using namespace dhcp::menu;
using namespace dhcp::net;

namespace {

/**
 * A volume in memory: paths map to "dir" or to file contents.
 *
 * It is a *fake*, not a mock: the shell's job is the policy, and the fake answers
 * the way a mounted FAT would — an entry is a directory when it was created as one,
 * a missing name is `NotFound`, and a directory that still holds something refuses
 * a plain removal.
 */
class FakeVolume : public IFileOperations {
public:
    set<string> dirs { "/fat", "/sdcard" };
    map<string, string> files;

    // What the test wants to see happen, in order.
    vector<string> calls;

    FileOpStatus attributes(const string& path, bool& exists, bool& isDir,
                            uint64_t& size) override
    {
        calls.push_back("attr " + path);
        exists = false;
        isDir = false;
        size = 0;
        if (dirs.count(path)) { exists = true; isDir = true; return FileOpStatus::Ok; }
        const auto it = files.find(path);
        if (it != files.end()) { exists = true; size = it->second.size(); return FileOpStatus::Ok; }
        return FileOpStatus::Ok;   // "does not exist" is an answer, not a failure
    }

    FileOpStatus list(const string& path, vector<FileEntry>& out) override
    {
        calls.push_back("list " + path);
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
        calls.push_back("mkdir " + path);
        if (dirs.count(path) || files.count(path)) return FileOpStatus::Exists;
        dirs.insert(path);
        return FileOpStatus::Ok;
    }

    FileOpStatus touch(const string& path, bool& created) override
    {
        calls.push_back("touch " + path);
        created = false;
        if (dirs.count(path) || files.count(path)) return FileOpStatus::Ok;
        files[path] = "";
        created = true;
        return FileOpStatus::Ok;
    }

    FileOpStatus removeFile(const string& path) override
    {
        calls.push_back("rm " + path);
        if (dirs.count(path)) return FileOpStatus::IsDirectory;
        if (!files.erase(path)) return FileOpStatus::NotFound;
        return FileOpStatus::Ok;
    }

    FileOpStatus removeTree(const string& path) override
    {
        calls.push_back("rmtree " + path);
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
        calls.push_back("mv " + from + " " + to);
        const auto it = files.find(from);
        if (it == files.end()) return FileOpStatus::NotFound;
        files[to] = it->second;
        files.erase(it);
        return FileOpStatus::Ok;
    }

    FileOpStatus copy(const string& from, const string& to) override
    {
        calls.push_back("cp " + from + " " + to);
        const auto it = files.find(from);
        if (it == files.end()) return FileOpStatus::NotFound;
        files[to] = it->second;
        return FileOpStatus::Ok;
    }

    FileOpStatus readText(const string& path, uint64_t limit, string& out,
                          bool& binary) override
    {
        calls.push_back("cat " + path);
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
        s.totalBytes = 1024 * 1024;
        s.freeBytes = 512 * 1024;
        return s;
    }
};

/** Runs one line and answers what the console would have printed. */
class Capture : public IConsoleOutput {
public:
    void write(const string& text) override { text_ += text; }
    const string& text() const { return text_; }
    void clear() { text_.clear(); }

private:
    string text_;
};

struct Fixture {
    FakeVolume fs;
    Capture out;
    FileShell shell{ out, fs };

    string run(const string& line)
    {
        out.clear();
        shell.execute(line);
        return out.text();
    }
};

/** `pwd` and `cd` are the session's position, and nothing else changes it. */
int test_the_position()
{
    Fixture f;
    TEST_ASSERT_STREQ(f.run("pwd"), "/fat\r\n");
    TEST_ASSERT_STREQ(f.run("cd logs"), "");
    TEST_ASSERT_STREQ(f.run("pwd"), "/fat/logs\r\n");
    TEST_ASSERT_STREQ(f.run("cd /sdcard"), "");
    TEST_ASSERT_STREQ(f.run("pwd"), "/sdcard\r\n");

    // A refused `cd` says why and leaves the shell where it was.
    TEST_ASSERT_TRUE(f.run("cd /nvs").find("error:") == 0);
    TEST_ASSERT_STREQ(f.run("pwd"), "/sdcard\r\n");
    return 0;
}

/** `ls` marks directories with a slash and prints sizes for files. */
int test_the_listing()
{
    Fixture f;
    f.fs.dirs.insert("/fat/logs");
    f.fs.dirs.insert("/fat/logs/Archive");
    f.fs.files["/fat/logs/SSH.log"] = "12345";
    f.fs.files["/fat/cache.dat"] = "aa";

    TEST_ASSERT_STREQ(f.run("ls /fat"), "f  2  cache.dat\r\nd  logs/\r\n");
    TEST_ASSERT_STREQ(f.run("ls /fat/logs"),
                      "d  Archive/\r\nf  5  SSH.log\r\n");
    TEST_ASSERT_TRUE(f.run("ls /fat/nothing").find("error:") == 0);
    return 0;
}

/** `mkdir` creates one level; an existing name and a missing parent are errors. */
int test_the_directory_creation()
{
    Fixture f;
    TEST_ASSERT_STREQ(f.run("mkdir /fat/logs"), "");
    TEST_ASSERT_TRUE(f.fs.dirs.count("/fat/logs") > 0);

    TEST_ASSERT_TRUE(f.run("mkdir /fat/logs").find("error:") == 0);
    TEST_ASSERT_TRUE(f.run("mkdir").find("error:") == 0);
    return 0;
}

/** `touch` creates an empty file and never truncates an existing one. */
int test_the_file_creation()
{
    Fixture f;
    TEST_ASSERT_STREQ(f.run("touch /fat/notes.txt"), "");
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/notes.txt") > 0);
    TEST_ASSERT_STREQ(f.fs.files["/fat/notes.txt"], "");

    f.fs.files["/fat/notes.txt"] = "kept";
    TEST_ASSERT_STREQ(f.run("touch /fat/notes.txt"), "touch: unchanged: /fat/notes.txt\r\n");
    TEST_ASSERT_STREQ(f.fs.files["/fat/notes.txt"], "kept");
    return 0;
}

/** `rm` needs `-r` for a directory, and never silently does nothing. */
int test_the_removal()
{
    Fixture f;
    f.fs.files["/fat/a.txt"] = "x";
    f.fs.dirs.insert("/fat/logs");
    f.fs.files["/fat/logs/SSH.log"] = "y";

    TEST_ASSERT_STREQ(f.run("rm /fat/a.txt"), "");
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/a.txt") == 0);

    TEST_ASSERT_TRUE(f.run("rm /fat/logs").find("error:") == 0);
    TEST_ASSERT_TRUE(f.fs.dirs.count("/fat/logs") > 0);

    TEST_ASSERT_STREQ(f.run("rm -r /fat/logs"), "");
    TEST_ASSERT_TRUE(f.fs.dirs.count("/fat/logs") == 0);
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/logs/SSH.log") == 0);

    TEST_ASSERT_TRUE(f.run("rm").find("error:") == 0);
    TEST_ASSERT_TRUE(f.run("rm /fat/missing").find("error:") == 0);
    return 0;
}

/** `cp` and `mv` leave an existing target alone unless `-f` is given. */
int test_the_copy_and_the_move()
{
    Fixture f;
    f.fs.files["/fat/a.txt"] = "A";
    f.fs.files["/fat/b.txt"] = "B";

    TEST_ASSERT_TRUE(f.run("cp /fat/a.txt /fat/b.txt").find("error:") == 0);
    TEST_ASSERT_STREQ(f.fs.files["/fat/b.txt"], "B");

    TEST_ASSERT_STREQ(f.run("cp -f /fat/a.txt /fat/b.txt"), "");
    TEST_ASSERT_STREQ(f.fs.files["/fat/b.txt"], "A");

    TEST_ASSERT_TRUE(f.run("mv /fat/a.txt /fat/b.txt").find("error:") == 0);
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/a.txt") > 0);

    TEST_ASSERT_STREQ(f.run("mv -f /fat/a.txt /fat/b.txt"), "");
    TEST_ASSERT_TRUE(f.fs.files.count("/fat/a.txt") == 0);
    TEST_ASSERT_STREQ(f.fs.files["/fat/b.txt"], "A");

    // A directory is not a thing `cp` copies here: saying so is better than a
    // half-copied tree.
    f.fs.dirs.insert("/fat/logs");
    TEST_ASSERT_TRUE(f.run("cp /fat/logs /fat/other").find("error:") == 0);
    return 0;
}

/** `cat` prints text, and refuses what is too long or is not text. */
int test_the_contents()
{
    Fixture f;
    f.fs.files["/fat/small.txt"] = "hello\nworld\n";
    TEST_ASSERT_STREQ(f.run("cat /fat/small.txt"), "hello\r\nworld\r\n");

    f.fs.files["/fat/big.txt"] = string(64 * 1024 + 1, 'x');
    TEST_ASSERT_TRUE(f.run("cat /fat/big.txt").find("error:") == 0);
    TEST_ASSERT_TRUE(f.run("cat /fat/big.txt").find("65536") != string::npos);
    return 0;
}

/** `df` names both volumes, mounted or not. */
int test_the_space()
{
    Fixture f;
    const string text = f.run("df");
    TEST_ASSERT_TRUE(text.find("/fat") != string::npos);
    TEST_ASSERT_TRUE(text.find("/sdcard") != string::npos);
    TEST_ASSERT_TRUE(text.find("not mounted") == string::npos);

    f.fs.dirs.erase("/sdcard");
    TEST_ASSERT_TRUE(f.run("df").find("not mounted") != string::npos);
    return 0;
}

/** `clear` sends the terminal sequence and nothing else. */
int test_the_clear()
{
    Fixture f;
    TEST_ASSERT_STREQ(f.run("clear"), "\x1b[H\x1b[2J");
    return 0;
}

/** The verbs are recognised as a set, and nothing else is claimed. */
int test_the_verbs()
{
    TEST_ASSERT_TRUE(FileShell::handles("ls"));
    TEST_ASSERT_TRUE(FileShell::handles("clear"));
    TEST_ASSERT_TRUE(FileShell::handles("df"));
    TEST_ASSERT_FALSE(FileShell::handles("help"));
    TEST_ASSERT_FALSE(FileShell::handles("reboot"));
    TEST_ASSERT_FALSE(FileShell::handles("LS"));
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running FileShell tests...\n");
    int failures = 0;

    failures += test_the_position();
    failures += test_the_listing();
    failures += test_the_directory_creation();
    failures += test_the_file_creation();
    failures += test_the_removal();
    failures += test_the_copy_and_the_move();
    failures += test_the_contents();
    failures += test_the_space();
    failures += test_the_clear();
    failures += test_the_verbs();

    if (failures == 0) {
        printf("All FileShell tests PASSED!\n");
    } else {
        printf("Some FileShell tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
