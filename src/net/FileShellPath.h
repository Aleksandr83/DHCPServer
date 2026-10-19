#ifndef DHCP_NET_FILESHELLPATH_H
#define DHCP_NET_FILESHELLPATH_H

#include <string>

namespace dhcp {
namespace net {

/**
 * @file FileShellPath.h
 * @brief The sandbox of the SSH file commands: which paths may be named at all.
 *
 * The device exposes two volumes — the internal FAT data partition at `/fat` and a
 * card at `/sdcard` — and a shell that could name anything else could read the NVS
 * partition, the certificates, or the running application. So the shell has no
 * notion of "a filesystem": it has these two roots and nothing else.
 *
 * The rules, in one place and without any I/O, so a host test can hold them:
 *
 *   * the session starts in `/fat`;
 *   * a relative name is read from the current directory; `.` and `..` are folded
 *     away before anything is opened;
 *   * `..` in the root of a volume stays in that root — the volume is a ceiling,
 *     not a step on the way out;
 *   * an absolute path must name a volume: `/fat/...` or `/sdcard/...`, and
 *     `/logs` is refused rather than guessed at;
 *   * a path that tries to leave through `..` (`/fat/../nvs`) is refused, not
 *     clamped: silently reaching a different place than the one typed is worse
 *     than saying no;
 *   * control bytes are refused — they cannot be printed back and cannot be typed
 *     into a file name honestly.
 */
class FileShellPath {
public:
    /// @brief The volume roots the shell may touch, in the order `df` lists them.
    static constexpr const char* kVolumes[] = { "/fat", "/sdcard" };

    FileShellPath();

    /// @brief The current directory, absolute (e.g. `/fat/logs`).
    const std::string& current() const { return current_; }

    /**
     * @brief Turn a typed name into an absolute path inside a volume.
     *
     * An empty name is the current directory. Returns "" and fills @p why when the
     * name cannot be honoured; the current directory is never changed by this call.
     */
    std::string resolve(const std::string& typed, std::string* why = nullptr) const;

    /**
     * @brief Move the session to @p typed (empty = the volume root of the current).
     *
     * On failure the current directory stays where it was: a refused `cd` must not
     * leave the shell half-way.
     */
    bool changeTo(const std::string& typed, std::string* why = nullptr);

    /// @brief `/fat` for `/fat/logs`, "" for a path outside the volumes.
    static std::string volumeOf(const std::string& absolute);

private:
    std::string current_;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_FILESHELLPATH_H
