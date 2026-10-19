#include "PosixFileOperations.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_vfs_fat.h"

using namespace std;

namespace dhcp {
namespace net {

namespace {

/// @brief The size of one read or write of a copy, and the stack buffer it uses.
constexpr size_t kCopyChunkBytes = 4096;

/// @brief `errno` → the status the shell speaks in. One place, so a message and a
///        failure cannot disagree.
FileOpStatus fromErrno(int err)
{
    switch (err) {
        case 0:        return FileOpStatus::Ok;
        case ENOENT:   return FileOpStatus::NotFound;
        case EEXIST:   return FileOpStatus::Exists;
        case ENOTDIR:  return FileOpStatus::NotDirectory;
        case EISDIR:   return FileOpStatus::IsDirectory;
        case ENOTEMPTY: return FileOpStatus::NotEmpty;
        case EACCES:
        case EPERM:
        case EROFS:    return FileOpStatus::Denied;
        case ENOSPC:
        case EDQUOT:   return FileOpStatus::NoSpace;
        default:       return FileOpStatus::IoError;
    }
}

string join(const string& dir, const string& name)
{
    if (dir.empty() || dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

bool isDot(const char* name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/// @brief Remove a directory with everything under it. `rmdir` only after the
///        contents are gone, which is what makes an interrupted pass resumable.
FileOpStatus removeTreeAt(const string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return fromErrno(errno);
    if (!S_ISDIR(st.st_mode)) {
        return (::unlink(path.c_str()) == 0) ? FileOpStatus::Ok : fromErrno(errno);
    }

    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) return fromErrno(errno);
    vector<string> children;
    while (struct dirent* entry = ::readdir(dir)) {
        if (isDot(entry->d_name)) continue;
        children.push_back(entry->d_name);
    }
    ::closedir(dir);

    for (const string& child : children) {
        const FileOpStatus status = removeTreeAt(join(path, child));
        if (status != FileOpStatus::Ok) return status;
    }
    return (::rmdir(path.c_str()) == 0) ? FileOpStatus::Ok : fromErrno(errno);
}

} // namespace

FileOpStatus PosixFileOperations::attributes(const string& path, bool& exists,
                                             bool& isDir, uint64_t& size)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        exists = false;
        isDir = false;
        size = 0;
        // "There is nothing there" is an answer; any other errno is a failure.
        return (errno == ENOENT) ? FileOpStatus::Ok : fromErrno(errno);
    }
    exists = true;
    isDir = S_ISDIR(st.st_mode);
    size = isDir ? 0 : static_cast<uint64_t>(st.st_size);
    return FileOpStatus::Ok;
}

FileOpStatus PosixFileOperations::list(const string& path, vector<FileEntry>& out)
{
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) return fromErrno(errno);

    while (struct dirent* entry = ::readdir(dir)) {
        if (isDot(entry->d_name)) continue;
        const string full = join(path, entry->d_name);
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;   // vanished mid-listing
        FileEntry item;
        item.name = entry->d_name;
        item.isDir = S_ISDIR(st.st_mode);
        item.size = item.isDir ? 0 : static_cast<uint64_t>(st.st_size);
        out.push_back(item);
    }
    ::closedir(dir);
    return FileOpStatus::Ok;
}

FileOpStatus PosixFileOperations::makeDir(const string& path)
{
    if (::mkdir(path.c_str(), 0755) == 0) return FileOpStatus::Ok;
    return fromErrno(errno);
}

FileOpStatus PosixFileOperations::touch(const string& path, bool& created)
{
    struct stat st;
    created = ::stat(path.c_str(), &st) != 0;
    // Append mode creates a missing file and never truncates an existing one: the
    // whole point of `touch` here is that it cannot lose data.
    FILE* file = fopen(path.c_str(), "ab");
    if (file == nullptr) {
        created = false;
        return fromErrno(errno);
    }
    const bool ok = fclose(file) == 0;
    return ok ? FileOpStatus::Ok : fromErrno(errno);
}

FileOpStatus PosixFileOperations::removeFile(const string& path)
{
    if (::unlink(path.c_str()) == 0) return FileOpStatus::Ok;
    return fromErrno(errno);
}

FileOpStatus PosixFileOperations::removeTree(const string& path)
{
    return removeTreeAt(path);
}

FileOpStatus PosixFileOperations::rename(const string& from, const string& to)
{
    if (::rename(from.c_str(), to.c_str()) == 0) return FileOpStatus::Ok;
    return fromErrno(errno);
}

FileOpStatus PosixFileOperations::copy(const string& from, const string& to)
{
    FILE* src = fopen(from.c_str(), "rb");
    if (src == nullptr) return fromErrno(errno);
    FILE* dst = fopen(to.c_str(), "wb");
    if (dst == nullptr) {
        fclose(src);
        return fromErrno(errno);
    }

    FileOpStatus status = FileOpStatus::Ok;
    char buffer[kCopyChunkBytes];
    for (;;) {
        const size_t got = fread(buffer, 1, sizeof(buffer), src);
        if (got > 0 && fwrite(buffer, 1, got, dst) != got) {
            status = fromErrno(errno);
            break;
        }
        if (got < sizeof(buffer)) {
            if (ferror(src)) status = FileOpStatus::IoError;
            break;
        }
    }
    if (status == FileOpStatus::Ok && fflush(dst) != 0) status = fromErrno(errno);

    const bool srcClosed = fclose(src) == 0;
    const bool dstClosed = fclose(dst) == 0;
    if (status == FileOpStatus::Ok && (!srcClosed || !dstClosed)) {
        status = FileOpStatus::IoError;
    }
    if (status != FileOpStatus::Ok) {
        // A half-copied file is not a file: the target is what the operator sees,
        // and leaving it behind would look like a successful copy of less data.
        ::unlink(to.c_str());
    }
    return status;
}

FileOpStatus PosixFileOperations::readText(const string& path, uint64_t limit,
                                           string& out, bool& binary)
{
    binary = false;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return fromErrno(errno);
    if (S_ISDIR(st.st_mode)) return FileOpStatus::IsDirectory;
    // The size is checked before reading: a `cat` of a 3 GB file must not try.
    if (static_cast<uint64_t>(st.st_size) > limit) return FileOpStatus::NoSpace;

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return fromErrno(errno);
    out.clear();
    out.reserve(static_cast<size_t>(st.st_size));

    char buffer[512];
    FileOpStatus status = FileOpStatus::Ok;
    for (;;) {
        const size_t got = fread(buffer, 1, sizeof(buffer), file);
        if (got == 0) {
            if (ferror(file)) status = FileOpStatus::IoError;
            break;
        }
        for (size_t i = 0; i < got; i++) {
            const unsigned char c = static_cast<unsigned char>(buffer[i]);
            // NUL and control bytes that no text file has: the bell, the escape,
            // the form feed. Tab, CR and LF are text.
            if (c == 0 || (c < 0x20 && c != '\t' && c != '\r' && c != '\n')) {
                binary = true;
            }
        }
        out.append(buffer, got);
        if (out.size() > limit) {   // grew past the limit while reading
            status = FileOpStatus::NoSpace;
            break;
        }
        if (got < sizeof(buffer)) break;
    }
    fclose(file);
    return status;
}

FileSpace PosixFileOperations::space(const string& volumeRoot)
{
    FileSpace space;
    // A volume that is not mounted cannot be asked anything: the mount point is a
    // directory of the *other* filesystem in that case, which would answer with its
    // own numbers.
    struct stat st;
    if (::stat(volumeRoot.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return space;

    uint64_t total = 0;
    uint64_t free = 0;
    if (esp_vfs_fat_info(volumeRoot.c_str(), &total, &free) != ESP_OK) return space;
    space.mounted = true;
    space.totalBytes = total;
    space.freeBytes = free;
    return space;
}

} // namespace net
} // namespace dhcp
