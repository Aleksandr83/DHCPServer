#include "SweepFileLogTarget.h"

#include <dirent.h>
#if defined(_WIN32)
#include <direct.h>   // _mkdir on MinGW
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "SweepLogFormat.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {

// Rule 39: the mode the folders are created with, the same the rest of the data
// volume uses.
constexpr int kDirMode = 0777;

/** @brief Create one directory; MinGW's `mkdir` takes no mode argument. */
int makeDir(const string& path)
{
#if defined(_WIN32)
    return ::_mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), kDirMode);
#endif
}

/** @brief Is @p path an existing directory? */
bool isDirectory(const string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

bool SweepFileLogTarget::createFolders(const string& folder)
{
    // One level at a time, outermost first: `mkdir` refuses a path whose parent
    // does not exist yet, and a single call for `log/AutoUpdate` would create
    // nothing at all — the log would then be missing with no error anywhere.
    // Deliberately silent about the reason: this class has no logger (it is
    // host-tested), so it answers false and the caller puts the reason into the
    // journal, where the operator will look for it.
    for (const auto& level : sweepLogFolderChain(folder)) {
        if (isDirectory(level)) continue;
        if (makeDir(level) != 0 && !isDirectory(level)) return false;
    }
    return true;
}

void SweepFileLogTarget::prune(const string& folder, size_t keep)
{
    DIR* dir = opendir(folder.c_str());
    if (dir == nullptr) return;

    vector<string> names;
    while (struct dirent* ent = readdir(dir)) {
        names.push_back(ent->d_name);
    }
    closedir(dir);

    for (const auto& name : sweepLogFilesToRemove(names, keep)) {
        remove((folder + "/" + name).c_str());
    }
}

bool SweepFileLogTarget::open(const string& mountPoint, const string& fileName,
                              size_t keep)
{
    close();
    path_.clear();

    const auto chain = sweepLogFolderChain(mountPoint);
    if (chain.empty() || fileName.empty()) return false;

    if (!createFolders(mountPoint)) return false;
    const string folder = chain.back();

    const string path = folder + "/" + fileName;
    // "ab": an existing name is appended to. Two sweeps in the same minute share
    // a name, and the second one must not erase the first.
    FILE* file = fopen(path.c_str(), "ab");
    if (file == nullptr) return false;

    file_ = file;
    path_ = path;
    sinceFlush_ = 0;
    lines_ = 0;

    // After the file exists, not before it: the new log is one of the
    // ones to keep, and a rule that forgot to count it would leave one
    // file more than the operator asked for, sweep after sweep.
    prune(folder, keep);
    return true;
}

void SweepFileLogTarget::close()
{
    if (file_ == nullptr) return;
    fflush(file_);
    fclose(file_);
    file_ = nullptr;
    sinceFlush_ = 0;
}

bool SweepFileLogTarget::append(LogLevel, const string& line)
{
    if (file_ == nullptr) return false;

    const string text = line + "\n";
    if (fwrite(text.data(), 1, text.size(), file_) != text.size()) return false;
    ++lines_;

    // Batched on purpose: a sweep writes tens of thousands of lines and every
    // flush is a card write. A restart loses at most the last kFlushEveryLines
    // of this log, which is the trade this file was designed with.
    if (++sinceFlush_ >= kFlushEveryLines) {
        if (fflush(file_) != 0) return false;
        sinceFlush_ = 0;
    }
    return true;
}

} // namespace core
} // namespace dhcp
