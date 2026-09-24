#include "WebPrune.h"

#include <algorithm>
#include <cstring>

// Rule 40: a translation unit of its own, so the directive cannot leak.
using namespace std;

namespace dhcp {
namespace web {

namespace {
constexpr const char* kMountPrefix = "spiffs/";
} // namespace

string WebPrune::normalise(const string& name)
{
    string out = name;

    // A leading slash is the VFS spelling of the same object ("/pages/x.html"
    // and "pages/x.html" are one file).
    while (!out.empty() && out.front() == '/') out.erase(out.begin());

    // The mount point is not part of the name: a caller that passes what it read
    // from a full path must not compare "spiffs/pages/x.html" with "pages/x.html".
    if (out.rfind(kMountPrefix, 0) == 0) out.erase(0, strlen(kMountPrefix));

    // A trailing slash names a directory, which SPIFFS does not have: drop it so
    // "pages/" and "pages" are one name (and both are refused later as files).
    while (!out.empty() && out.back() == '/') out.pop_back();

    return out;
}

bool WebPrune::isAcceptableName(const string& name)
{
    if (name.empty() || name.size() > kMaxNameLen) return false;
    if (name.front() == '/') return false;

    // The same character rule the upload handler enforces, segment by segment:
    // letters, digits, '.', '_' and '-' only, no empty segment (so no "//"), and
    // neither "." nor ".." as a segment.
    auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    };

    size_t segStart = 0;
    while (segStart <= name.size()) {
        const size_t slash = name.find('/', segStart);
        const string seg = name.substr(segStart, slash == string::npos
                                                      ? string::npos
                                                      : slash - segStart);
        if (seg.empty() || seg == "." || seg == "..") return false;
        for (char c : seg) {
            if (!allowed(c)) return false;
        }
        if (slash == string::npos) break;
        segStart = slash + 1;
    }
    return true;
}

vector<string> WebPrune::extra(const vector<string>& onDevice,
                               const vector<string>& uploaded)
{
    vector<string> known;
    known.reserve(uploaded.size());
    for (const string& name : uploaded) {
        const string norm = normalise(name);
        if (!norm.empty()) known.push_back(norm);
    }
    sort(known.begin(), known.end());

    vector<string> out;
    for (const string& name : onDevice) {
        const string norm = normalise(name);
        if (norm.empty()) continue;   // nothing to compare, nothing to delete
        if (!binary_search(known.begin(), known.end(), norm)) out.push_back(name);
    }
    return out;
}

} // namespace web
} // namespace dhcp
