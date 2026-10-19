#include "FileShellPath.h"

#include <cstring>
#include <vector>

using namespace std;

namespace dhcp {
namespace net {

constexpr const char* FileShellPath::kVolumes[];

namespace {

/// @brief Is @p name one of the volume roots?
bool isVolume(const string& name)
{
    for (const char* volume : FileShellPath::kVolumes) {
        if (name == volume) return true;
    }
    return false;
}

/// @brief Does @p name start with `/fat` or `/sdcard` as a whole component?
bool startsWithVolume(const string& name)
{
    for (const char* volume : FileShellPath::kVolumes) {
        const size_t len = strlen(volume);
        if (name.compare(0, len, volume) == 0 &&
            (name.size() == len || name[len] == '/')) {
            return true;
        }
    }
    return false;
}

bool hasControlByte(const string& text)
{
    for (unsigned char c : text) {
        if (c < 0x20) return true;
    }
    return false;
}

string trimmedSlashes(string text)
{
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    return text;
}

} // namespace

FileShellPath::FileShellPath()
    : current_(kVolumes[0])
{
}

string FileShellPath::volumeOf(const string& absolute)
{
    for (const char* volume : kVolumes) {
        const size_t len = strlen(volume);
        if (absolute.compare(0, len, volume) == 0 &&
            (absolute.size() == len || absolute[len] == '/')) {
            return volume;
        }
    }
    return "";
}

string FileShellPath::resolve(const string& typed, string* why) const
{
    const auto fail = [why](const char* reason) {
        if (why) *why = reason;
        return string();
    };

    if (typed.empty()) return current_;
    if (hasControlByte(typed)) return fail("a path cannot hold control symbols");

    // Where the walk starts: the current directory for a relative name, the volume
    // root for an absolute one. An absolute name that is not a volume is refused
    // here, before anything is opened.
    string base;
    string rest;
    if (typed[0] == '/') {
        if (!startsWithVolume(typed)) {
            return fail("a path starts with /fat or /sdcard");
        }
        base = volumeOf(typed);
        rest = typed.substr(base.size());
    } else {
        base = volumeOf(current_);
        if (base.empty()) base = kVolumes[0];
        rest = typed;
    }

    // The components are folded one by one: `.` is dropped, `..` pops, and popping
    // past the volume root is where the sandbox says no — the operator's rule is
    // that leaving the volume is refused, not quietly clamped to its root.
    vector<string> parts;
    if (typed[0] != '/') {
        // A relative name starts from where the shell stands, not from the root.
        size_t at = base.size();
        while (at < current_.size()) {
            while (at < current_.size() && current_[at] == '/') ++at;
            const size_t slash = current_.find('/', at);
            const size_t end = (slash == string::npos) ? current_.size() : slash;
            if (end > at) parts.push_back(current_.substr(at, end - at));
            at = (slash == string::npos) ? current_.size() : slash;
        }
    }
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t slash = rest.find('/', start);
        const size_t end = (slash == string::npos) ? rest.size() : slash;
        const string part = rest.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (parts.empty()) {
                    return fail("a path cannot leave its volume");
                }
                parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }
        if (slash == string::npos) break;
        start = slash + 1;
    }

    string out = base;
    for (const string& part : parts) {
        if (out.back() != '/') out += '/';
        out += part;
    }
    return trimmedSlashes(out);
}

bool FileShellPath::changeTo(const string& typed, string* why)
{
    if (typed.empty()) {
        // A bare `cd` goes to the root of the volume the shell is standing on.
        const string volume = volumeOf(current_);
        current_ = volume.empty() ? kVolumes[0] : volume;
        return true;
    }
    const string target = resolve(typed, why);
    if (target.empty()) return false;
    current_ = target;
    return true;
}

} // namespace net
} // namespace dhcp
