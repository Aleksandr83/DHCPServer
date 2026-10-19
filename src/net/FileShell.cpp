#include "FileShell.h"

#include <algorithm>
#include <cstring>

using namespace std;

namespace dhcp {
namespace net {

namespace {

/// @brief A line for an SSH channel: CR LF, because the client is a raw terminal.
constexpr const char* kEol = "\r\n";
/// @brief Clear the screen and put the cursor in its corner (ANSI/ECMA-48).
constexpr const char* kClearScreen = "\x1b[H\x1b[2J";

} // namespace

FileShell::FileShell(::dhcp::menu::IConsoleOutput& out, IFileOperations& fs)
    : out_(out)
    , fs_(fs)
{
}

bool FileShell::handles(const string& word)
{
    static const char* const kVerbs[] = {
        "ls", "cd", "pwd", "mkdir", "touch", "rm", "mv", "cp", "cat", "df", "clear"
    };
    for (const char* verb : kVerbs) {
        if (word == verb) return true;
    }
    return false;
}

void FileShell::say(const string& text)
{
    out_.write(text + kEol);
}

void FileShell::sayFailure(const char* verb, const string& path, FileOpStatus status)
{
    const char* reason = "the volume refused the operation";
    switch (status) {
        case FileOpStatus::NotFound:      reason = "no such file or directory"; break;
        case FileOpStatus::Exists:        reason = "already exists"; break;
        case FileOpStatus::NotDirectory:  reason = "not a directory"; break;
        case FileOpStatus::IsDirectory:   reason = "is a directory"; break;
        case FileOpStatus::NotEmpty:      reason = "the directory is not empty"; break;
        case FileOpStatus::Denied:        reason = "permission denied"; break;
        case FileOpStatus::NoSpace:       reason = "no space left"; break;
        case FileOpStatus::IoError:       reason = "the volume reported an I/O error"; break;
        case FileOpStatus::Unsupported:   reason = "not supported by this volume"; break;
        case FileOpStatus::Ok:            return;
    }
    say(string("error: ") + reason + ": " + verb + " " + path);
}

FileShell::Args FileShell::parseArgs(const string& rest){
    Args args;
    size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && rest[i] == ' ') ++i;
        if (i >= rest.size()) break;
        const size_t start = i;
        while (i < rest.size() && rest[i] != ' ') ++i;
        const string word = rest.substr(start, i - start);
        // Only the two flags this stage defines are flags; everything else is a
        // name, including a name that happens to start with a dash.
        if (word == "-r") { args.recursive = true; continue; }
        if (word == "-f") { args.force = true; continue; }
        args.words.push_back(word);
    }
    return args;
}

bool FileShell::pathExists(const string& path, bool* isDir, uint64_t* size)
{
    bool exists = false;
    bool directory = false;
    uint64_t bytes = 0;
    if (fs_.attributes(path, exists, directory, bytes) != FileOpStatus::Ok) return false;
    if (isDir) *isDir = exists && directory;
    if (size) *size = bytes;
    return exists;
}

void FileShell::execute(const string& line)
{
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ') ++i;
    const string verb = line.substr(start, i - start);
    const string rest = line.substr(i);

    const Args args = parseArgs(rest);
    if (verb == "ls") { cmdLs(args); return; }
    if (verb == "cd") { cmdCd(args); return; }
    if (verb == "pwd") { cmdPwd(); return; }
    if (verb == "mkdir") { cmdMkdir(args); return; }
    if (verb == "touch") { cmdTouch(args); return; }
    if (verb == "rm") { cmdRm(args); return; }
    if (verb == "mv") { cmdMv(args); return; }
    if (verb == "cp") { cmdCp(args); return; }
    if (verb == "cat") { cmdCat(args); return; }
    if (verb == "df") { cmdDf(); return; }
    if (verb == "clear") { cmdClear(); return; }
}

void FileShell::cmdPwd()
{
    say(path_.current());
}

void FileShell::cmdCd(const Args& args)
{
    string why;
    const string typed = args.words.empty() ? string() : args.words[0];
    if (!path_.changeTo(typed, &why)) {
        say("error: " + why + ": " + (typed.empty() ? string(".") : typed));
    }
}

void FileShell::cmdLs(const Args& args)
{
    string why;
    const string target = path_.resolve(args.words.empty() ? string() : args.words[0], &why);
    if (target.empty()) {
        say("error: " + why);
        return;
    }

    vector<FileEntry> entries;
    const FileOpStatus status = fs_.list(target, entries);
    if (status != FileOpStatus::Ok) {
        sayFailure("ls", target, status);
        return;
    }
    if (entries.empty()) {
        say("(empty)");
        return;
    }
    sort(entries.begin(), entries.end(),
         [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    for (const FileEntry& entry : entries) {
        if (entry.isDir) {
            say("d  " + entry.name + "/");
        } else {
            say("f  " + to_string(entry.size) + "  " + entry.name);
        }
    }
}

void FileShell::cmdMkdir(const Args& args)
{
    if (args.words.empty()) {
        say("error: mkdir needs a name");
        return;
    }
    string why;
    const string target = path_.resolve(args.words[0], &why);
    if (target.empty()) {
        say("error: " + why);
        return;
    }
    const FileOpStatus status = fs_.makeDir(target);
    if (status != FileOpStatus::Ok) sayFailure("mkdir", target, status);
}

void FileShell::cmdTouch(const Args& args)
{
    if (args.words.empty()) {
        say("error: touch needs a name");
        return;
    }
    string why;
    const string target = path_.resolve(args.words[0], &why);
    if (target.empty()) {
        say("error: " + why);
        return;
    }
    bool created = false;
    const FileOpStatus status = fs_.touch(target, created);
    if (status != FileOpStatus::Ok) {
        sayFailure("touch", target, status);
        return;
    }
    // Saying nothing about an existing file would look like a write that happened.
    if (!created) say("touch: unchanged: " + target);
}

void FileShell::cmdRm(const Args& args)
{
    if (args.words.empty()) {
        say("error: rm needs a name");
        return;
    }
    string why;
    const string target = path_.resolve(args.words[0], &why);
    if (target.empty()) {
        say("error: " + why);
        return;
    }
    if (target == FileShellPath::volumeOf(target)) {
        say("error: a volume root cannot be removed: " + target);
        return;
    }

    bool exists = false;
    bool isDir = false;
    uint64_t size = 0;
    const FileOpStatus about = fs_.attributes(target, exists, isDir, size);
    if (about != FileOpStatus::Ok) {
        sayFailure("rm", target, about);
        return;
    }
    if (!exists) {
        say("error: no such file or directory: " + target);
        return;
    }
    if (isDir && !args.recursive) {
        say("error: is a directory, use rm -r: " + target);
        return;
    }

    const FileOpStatus status = isDir ? fs_.removeTree(target) : fs_.removeFile(target);
    if (status != FileOpStatus::Ok) sayFailure("rm", target, status);
}

void FileShell::cmdMv(const Args& args)
{
    if (args.words.size() < 2) {
        say("error: mv needs a source and a destination");
        return;
    }
    string whyFrom;
    string whyTo;
    const string from = path_.resolve(args.words[0], &whyFrom);
    const string to = path_.resolve(args.words[1], &whyTo);
    if (from.empty() || to.empty()) {
        say("error: " + (from.empty() ? whyFrom : whyTo));
        return;
    }
    const FileOpStatus status = pathExists(to) && !args.force
        ? FileOpStatus::Exists
        : fs_.rename(from, to);
    if (status != FileOpStatus::Ok) {
        if (status == FileOpStatus::Exists) {
            say("error: already exists, use -f: " + to);
        } else {
            sayFailure("mv", from, status);
        }
    }
}

void FileShell::cmdCp(const Args& args)
{
    if (args.words.size() < 2) {
        say("error: cp needs a source and a destination");
        return;
    }
    string whyFrom;
    string whyTo;
    const string from = path_.resolve(args.words[0], &whyFrom);
    const string to = path_.resolve(args.words[1], &whyTo);
    if (from.empty() || to.empty()) {
        say("error: " + (from.empty() ? whyFrom : whyTo));
        return;
    }

    bool exists = false;
    bool isDir = false;
    uint64_t size = 0;
    const FileOpStatus about = fs_.attributes(from, exists, isDir, size);
    if (about != FileOpStatus::Ok) {
        sayFailure("cp", from, about);
        return;
    }
    if (!exists) {
        say("error: no such file or directory: " + from);
        return;
    }
    if (isDir) {
        // Copying a tree is not what this stage does, and a half-copied directory
        // reported as success would be worse than a refusal.
        say("error: directories are not copied: " + from);
        return;
    }
    if (pathExists(to) && !args.force) {
        say("error: already exists, use -f: " + to);
        return;
    }
    const FileOpStatus status = fs_.copy(from, to);
    if (status != FileOpStatus::Ok) sayFailure("cp", from, status);
}

void FileShell::cmdCat(const Args& args)
{
    if (args.words.empty()) {
        say("error: cat needs a name");
        return;
    }
    string why;
    const string target = path_.resolve(args.words[0], &why);
    if (target.empty()) {
        say("error: " + why);
        return;
    }

    string text;
    bool binary = false;
    const FileOpStatus status = fs_.readText(target, kCatLimitBytes, text, binary);
    if (status == FileOpStatus::NoSpace) {
        say("error: larger than " + to_string(kCatLimitBytes) + " bytes: " + target);
        return;
    }
    if (status != FileOpStatus::Ok) {
        sayFailure("cat", target, status);
        return;
    }
    if (binary) {
        say("error: not a text file: " + target);
        return;
    }

    // The content is printed as it is, with its own line breaks turned into the
    // channel's CR LF so a file reads the same on both ends.
    std::string out;
    out.reserve(text.size() + 2);
    for (char c : text) {
        if (c == '\n') {
            out += kEol;
        } else if (c != '\r') {
            out += c;
        }
    }
    if (!out.empty() && out.size() >= 2 && out.compare(out.size() - 2, 2, kEol) != 0) {
        out += kEol;
    }
    out_.write(out);
}

void FileShell::cmdDf()
{
    for (const char* volume : FileShellPath::kVolumes) {
        const FileSpace space = fs_.space(volume);
        if (!space.mounted) {
            say(string(volume) + "  not mounted");
            continue;
        }
        const uint64_t totalKb = space.totalBytes / 1024u;
        const uint64_t freeKb = space.freeBytes / 1024u;
        say(string(volume) + "  " + to_string(totalKb) + " KB total, " +
            to_string(freeKb) + " KB free");
    }
}

void FileShell::cmdClear()
{
    out_.write(kClearScreen);
}

void FileShell::printHelp()
{
    out_.write(
        "File commands (only /fat and /sdcard):" + string(kEol) +
        "  ls [path]        list a directory" + kEol +
        "  cd [path]        change the directory (a volume is the ceiling)" + kEol +
        "  pwd              print the current directory" + kEol +
        "  mkdir <path>     create one directory" + kEol +
        "  touch <path>     create an empty file, never truncate one" + kEol +
        "  rm [-r] <path>   remove a file; a directory needs -r" + kEol +
        "  cp [-f] <a> <b>  copy a file; -f replaces an existing target" + kEol +
        "  mv [-f] <a> <b>  move a file; -f replaces an existing target" + kEol +
        "  cat <path>       print a text file up to 64 KB" + kEol +
        "  df               free space on both volumes" + kEol +
        "  clear            clear the terminal" + kEol +
        "  mc               two-pane file browser (F3 view, F4 new, F5 copy, "
        "F6 rename, F7 mkdir, F8 delete, Tab panel, Alt+F1/F2 volume, q quit)" + kEol);
}

} // namespace net
} // namespace dhcp
