#ifndef DHCP_STORAGE_PATHUTIL_H
#define DHCP_STORAGE_PATHUTIL_H

#include <string>

namespace dhcp {
namespace storage {

/**
 * @brief Path handling for the FAT file explorer (internal FAT / microSD).
 *
 * All paths in the Files API are **volume-relative** and are always given
 * with a leading slash, e.g. `/`, `/logs`, `/logs/2026-09-15.txt`. The
 * absolute path used with the VFS is built by @ref join with the volume's
 * mount point (`/fat`, `/sdcard`).
 *
 * @ref normalize is the single point where a client-supplied path is
 * validated, so every REST handler and the web UI share exactly one policy:
 *
 *  - `.` and empty segments are dropped (`/a//b/`, `/a/./b` → `/a/b`),
 *  - `..` is rejected outright (a client must never escape its volume),
 *  - control characters, `\` and the FAT-illegal set `" * < > ? | :` are
 *    rejected — the browser sends POSIX-style paths, so a backslash is much
 *    more likely to be a mistake than a separator,
 *  - a segment must not end with a dot or a space (FATFS silently trims those,
 *    which would make "the file I just created" unfindable),
 *  - the result is length- and depth-capped (@ref kMaxPathLen,
 *    @ref kMaxSegmentLen, @ref kMaxDepth) because the HTTP URI is bounded too.
 *
 * Deliberately free of ESP-IDF dependencies, so the class is unit-tested on
 * the host (see `test/test_pathutil.cpp`), like `core::Subnet` and
 * `time::TimeMath`.
 */
class PathUtil {
public:
    /** Maximum length of a normalized path, including the leading slash. */
    static constexpr size_t kMaxPathLen = 255;
    /** Maximum length of a single path segment (name). */
    static constexpr size_t kMaxSegmentLen = 128;
    /** Maximum number of segments below the volume root. */
    static constexpr size_t kMaxDepth = 16;

    /**
     * @brief Normalize and validate a client-supplied volume-relative path.
     *
     * Accepts `""`, `/`, `//`, `a/b`, `/a/b/`, `/a//b`, `/a/./b` and returns
     * `/`, `/a/b` … Rejects anything that tries to escape the volume or
     * contains illegal characters (see the class comment).
     *
     * @param[in]  raw Client-supplied path (may be empty, may lack the slash).
     * @param[out] out Normalized path — untouched when the result is false.
     * @return true when @p raw is a valid path inside a volume.
     */
    static bool normalize(const std::string& raw, std::string& out);

    /**
     * @brief Normalize a path and append a new child name in one step.
     *
     * Used by upload/rename/mkdir where the UI sends a directory plus a name.
     * @return false when @p dir is invalid or @p name is not a valid segment.
     */
    static bool normalizeChild(const std::string& dir, const std::string& name,
                               std::string& out);

    /**
     * @brief Validate a single name (a file or directory entry, no slashes).
     * @return true for a non-empty, legal FAT long-file-name segment.
     */
    static bool isValidName(const std::string& name);

    /** @brief True when @p c may appear inside a path segment. */
    static bool isValidChar(char c);

    /** @brief Absolute VFS path: @ref join("/fat", "/logs/x.txt") → `/fat/logs/x.txt`. */
    static std::string join(const std::string& mountPoint,
                            const std::string& relPath);

    /** @brief Parent of a normalized path (`/a/b` → `/a`, `/a` → `/`). */
    static std::string parent(const std::string& relPath);

    /** @brief Last segment of a normalized path (`/a/b.txt` → `b.txt`). */
    static std::string basename(const std::string& relPath);
};

} // namespace storage
} // namespace dhcp

#endif // DHCP_STORAGE_PATHUTIL_H
