#include "PathUtil.h"

#include <vector>

using namespace std;

namespace {

// Rule 39: printable ASCII — del is a control character too.
constexpr unsigned char kAsciiPrintableMin = 0x20;
constexpr unsigned char kAsciiDel = 0x7F;

} // namespace

namespace dhcp {
namespace storage {

namespace {

/**
 * @brief Split @p path on '/' and validate every segment.
 *
 * Leading/trailing and duplicated slashes produce empty segments, which are
 * skipped (they carry no meaning for FAT). `..` is a hard error — the caller
 * must never be able to climb out of its volume.
 *
 * @param[out] segments Filled with the validated segments (may stay empty for
 *                      the volume root).
 */
bool splitSegments(const string& path, vector<string>& segments)
{
    segments.clear();

    size_t i = 0;
    while (i <= path.size()) {
        size_t slash = path.find('/', i);
        string seg = path.substr(
            i, slash == string::npos ? string::npos : slash - i);

        if (seg.empty() || seg == ".") {
            // Root, repeated separator or explicit "here" — nothing to add.
        } else if (seg == "..") {
            return false;
        } else if (!PathUtil::isValidName(seg)) {
            return false;
        } else {
            segments.push_back(seg);
        }

        if (slash == string::npos) break;
        i = slash + 1;

        // Depth is capped so a hostile client cannot build a path of 10 000
        // segments (the HTTP URI is bounded anyway).
        if (segments.size() > PathUtil::kMaxDepth) return false;
    }

    return segments.size() <= PathUtil::kMaxDepth;
}

/** @brief Rebuild a normalized path from validated segments. */
string buildPath(const vector<string>& segments)
{
    string out;
    for (const string& seg : segments) {
        out += '/';
        out += seg;
    }
    return out.empty() ? string("/") : out;
}

} // namespace

bool PathUtil::isValidChar(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);

    if (u < kAsciiPrintableMin || u == kAsciiDel) return false;   // control characters
    if (c == '/' || c == '\\') return false;   // separators are handled outside
    switch (c) {
        case '"': case '*': case '<': case '>':
        case '?': case '|': case ':':
            return false;                      // illegal in FAT file names
        default:
            return true;
    }
}

bool PathUtil::isPartName(const string& name)
{
    const string suffix = kUploadPartSuffix;
    if (name.size() <= suffix.size()) return false;
    return name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool PathUtil::isValidName(const string& name)
{
    if (name.empty() || name.size() > kMaxSegmentLen) return false;
    if (name == "." || name == "..") return false;

    // `.part` belongs to the upload machinery (see isPartName): accepting the
    // name would create a file the operator can never see or manage again.
    if (isPartName(name)) return false;

    // FATFS trims trailing dots/spaces on create, so a name that ends with one
    // would never be found again — reject it up front.
    const char last = name.back();
    if (last == '.' || last == ' ') return false;

    for (char c : name) {
        if (!isValidChar(c)) return false;
    }
    return true;
}

bool PathUtil::normalize(const string& raw, string& out)
{
    vector<string> segments;
    if (!splitSegments(raw, segments)) return false;

    const string normalized = buildPath(segments);
    if (normalized.size() > kMaxPathLen) return false;

    out = normalized;
    return true;
}

bool PathUtil::normalizeChild(const string& dir, const string& name,
                              string& out)
{
    if (!isValidName(name)) return false;

    vector<string> segments;
    if (!splitSegments(dir, segments)) return false;
    segments.push_back(name);

    if (segments.size() > kMaxDepth) return false;

    const string normalized = buildPath(segments);
    if (normalized.size() > kMaxPathLen) return false;

    out = normalized;
    return true;
}

string PathUtil::join(const string& mountPoint,
                           const string& relPath)
{
    if (relPath.empty() || relPath == "/") return mountPoint;

    string out = mountPoint;
    if (!out.empty() && out.back() == '/') out.pop_back();
    out += relPath.front() == '/' ? relPath : "/" + relPath;
    return out;
}

string PathUtil::parent(const string& relPath)
{
    if (relPath.empty() || relPath == "/") return "/";

    const size_t slash = relPath.find_last_of('/');
    if (slash == string::npos || slash == 0) return "/";

    return relPath.substr(0, slash);
}

string PathUtil::basename(const string& relPath)
{
    if (relPath.empty() || relPath == "/") return "";

    const size_t slash = relPath.find_last_of('/');
    return slash == string::npos ? relPath : relPath.substr(slash + 1);
}

} // namespace storage
} // namespace dhcp
