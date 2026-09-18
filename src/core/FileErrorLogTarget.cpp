#include "FileErrorLogTarget.h"

#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>

namespace dhcp {
namespace core {

namespace {

/** Is @p path an existing directory? */
bool isDirectory(const std::string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

FileErrorLogTarget::FileErrorLogTarget(std::string path)
    : path_(std::move(path))
{
}

std::string FileErrorLogTarget::directoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return {};
    return path.substr(0, slash);
}

bool FileErrorLogTarget::ensureDirectory()
{
    if (directoryReady_) return true;

    const std::string dir = directoryOf(path_);
    if (dir.empty()) { directoryReady_ = true; return true; }

    // One level only, and only inside something that is already there. The mount
    // point belongs to the storage layer: if `/fat` does not exist (the volume was
    // not mounted, the card is out), creating a *directory* called `/fat` would
    // hide the real problem behind a log file that pretends the volume is fine.
    if (isDirectory(dir)) { directoryReady_ = true; return true; }

    const std::string parent = directoryOf(dir);
    if (parent.empty() || !isDirectory(parent)) return false;

    if (mkdir(dir.c_str(), 0777) != 0) {
        // Someone may have created it in between; only a second look can tell a
        // race from a failure.
        if (!isDirectory(dir)) return false;
    }

    directoryReady_ = true;
    return true;
}

bool FileErrorLogTarget::rotateIfNeeded(size_t incomingBytes)
{
    struct stat st {};
    if (stat(path_.c_str(), &st) != 0) return true;          // nothing there yet
    // A directory (or anything else) under the log's name: refuse instead of
    // failing on fopen with a reason nobody can read later.
    if (S_ISDIR(st.st_mode)) return false;
    const size_t size = static_cast<size_t>(st.st_size);
    if (size + incomingBytes + 1 <= kMaxBytes) return true;  // +1 for the newline

    const std::string kept = path_ + kRotatedSuffix;
    // Windows (and FatFS's rename) refuse to replace an existing destination, so
    // the previous generation is removed first: one generation, not a pile.
    std::remove(kept.c_str());
    if (std::rename(path_.c_str(), kept.c_str()) != 0) {
        // A full or read-only volume: drop this generation rather than refusing
        // to log at all — the newest lines are the ones worth having.
        std::remove(path_.c_str());
    }
    return true;
}

bool FileErrorLogTarget::append(LogLevel /*level*/, const std::string& line)
{
    // The level is already inside the line ("[E]"/"[W]"): the file is read by a
    // human in the file explorer, not parsed.
    if (!ensureDirectory()) return false;

    const std::string text = line + "\n";
    if (!rotateIfNeeded(text.size())) return false;

    std::FILE* file = std::fopen(path_.c_str(), "ab");
    if (file == nullptr) return false;

    const size_t written = std::fwrite(text.data(), 1, text.size(), file);
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;

    return written == text.size() && flushed && closed;
}

} // namespace core
} // namespace dhcp
