#include "PathUtil.h"

#include <vector>

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
bool splitSegments(const std::string& path, std::vector<std::string>& segments)
{
    segments.clear();

    size_t i = 0;
    while (i <= path.size()) {
        size_t slash = path.find('/', i);
        std::string seg = path.substr(
            i, slash == std::string::npos ? std::string::npos : slash - i);

        if (seg.empty() || seg == ".") {
            // Root, repeated separator or explicit "here" — nothing to add.
        } else if (seg == "..") {
            return false;
        } else if (!PathUtil::isValidName(seg)) {
            return false;
        } else {
            segments.push_back(seg);
        }

        if (slash == std::string::npos) break;
        i = slash + 1;

        // Depth is capped so a hostile client cannot build a path of 10 000
        // segments (the HTTP URI is bounded anyway).
        if (segments.size() > PathUtil::kMaxDepth) return false;
    }

    return segments.size() <= PathUtil::kMaxDepth;
}

/** @brief Rebuild a normalized path from validated segments. */
std::string buildPath(const std::vector<std::string>& segments)
{
    std::string out;
    for (const std::string& seg : segments) {
        out += '/';
        out += seg;
    }
    return out.empty() ? std::string("/") : out;
}

} // namespace

bool PathUtil::isValidChar(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);

    if (u < 0x20 || u == 0x7F) return false;   // control characters
    if (c == '/' || c == '\\') return false;   // separators are handled outside
    switch (c) {
        case '"': case '*': case '<': case '>':
        case '?': case '|': case ':':
            return false;                      // illegal in FAT file names
        default:
            return true;
    }
}

bool PathUtil::isValidName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxSegmentLen) return false;
    if (name == "." || name == "..") return false;

    // FATFS trims trailing dots/spaces on create, so a name that ends with one
    // would never be found again — reject it up front.
    const char last = name.back();
    if (last == '.' || last == ' ') return false;

    for (char c : name) {
        if (!isValidChar(c)) return false;
    }
    return true;
}

bool PathUtil::normalize(const std::string& raw, std::string& out)
{
    std::vector<std::string> segments;
    if (!splitSegments(raw, segments)) return false;

    const std::string normalized = buildPath(segments);
    if (normalized.size() > kMaxPathLen) return false;

    out = normalized;
    return true;
}

bool PathUtil::normalizeChild(const std::string& dir, const std::string& name,
                              std::string& out)
{
    if (!isValidName(name)) return false;

    std::vector<std::string> segments;
    if (!splitSegments(dir, segments)) return false;
    segments.push_back(name);

    if (segments.size() > kMaxDepth) return false;

    const std::string normalized = buildPath(segments);
    if (normalized.size() > kMaxPathLen) return false;

    out = normalized;
    return true;
}

std::string PathUtil::join(const std::string& mountPoint,
                           const std::string& relPath)
{
    if (relPath.empty() || relPath == "/") return mountPoint;

    std::string out = mountPoint;
    if (!out.empty() && out.back() == '/') out.pop_back();
    out += relPath.front() == '/' ? relPath : "/" + relPath;
    return out;
}

std::string PathUtil::parent(const std::string& relPath)
{
    if (relPath.empty() || relPath == "/") return "/";

    const size_t slash = relPath.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";

    return relPath.substr(0, slash);
}

std::string PathUtil::basename(const std::string& relPath)
{
    if (relPath.empty() || relPath == "/") return "";

    const size_t slash = relPath.find_last_of('/');
    return slash == std::string::npos ? relPath : relPath.substr(slash + 1);
}

} // namespace storage
} // namespace dhcp
