#include "LogFileWriter.h"

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#if defined(_WIN32)
#include <direct.h>   // _mkdir on MinGW
#endif

using namespace std;

namespace dhcp {
namespace core {

namespace {

// Rule 39: the mode the folders are created with, the same the rest of the data
// volume uses; the chunk the archive is copied in, which is small enough to live
// on the stack of the log task; and the one-character-per-line scan of the copy.
constexpr int kDirMode = 0777;
constexpr size_t kCopyChunkBytes = 1024;

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

/** @brief The folder of @p path ("" when it has none to speak of). */
string folderOf(const string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == string::npos || slash == 0) return string();
    return path.substr(0, slash);
}

/** @brief The file name of @p path — what an archive of it is called too. */
string fileNameOf(const string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == string::npos) return path;
    return path.substr(slash + 1);
}

} // namespace

LogFileWriter::LogFileWriter(string livePath, string cardMountPoint,
                             CardCheck cardMounted, Notice notice, size_t maxBytes)
    : livePath_(move(livePath))
    , fileName_(fileNameOf(livePath_))
    , cardMount_(move(cardMountPoint))
    , cardMounted_(move(cardMounted))
    , notice_(move(notice))
    , maxBytes_(maxBytes)
{
}

string LogFileWriter::readLive() const
{
    FILE* file = fopen(livePath_.c_str(), "rb");
    if (file == nullptr) return string();

    string out;
    char buf[kCopyChunkBytes];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), file)) > 0) out.append(buf, got);
    fclose(file);
    return out;
}

size_t LogFileWriter::liveSize() const
{
    struct stat st {};
    if (stat(livePath_.c_str(), &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;   // a folder under the log's name is no log
    return static_cast<size_t>(st.st_size);
}

bool LogFileWriter::ensureLiveDirectory()
{
    const string dir = folderOf(livePath_);
    if (dir.empty() || isDirectory(dir)) return true;

    // One level only, and only inside something that is already there: the mount
    // point belongs to the storage layer, and creating a *directory* called
    // `/fat` would hide a volume that never mounted behind a healthy-looking log.
    const string parent = folderOf(dir);
    if (parent.empty() || !isDirectory(parent)) return false;
    if (makeDir(dir) != 0 && !isDirectory(dir)) return false;
    return true;
}

void LogFileWriter::rememberStamp(const LogStamp& stamp)
{
    if (!stamp.clockSet) return;
    const LogDate date{stamp.year, stamp.month, stamp.day};
    if (!firstRealSeen_) {
        firstReal_ = date;
        firstRealSeen_ = true;
    }
    lastReal_ = date;
}

bool LogFileWriter::archiveToCard(const string& rangeName)
{
    const auto chain = logFolderChain(cardMount_, archiveFolder(), rangeName);
    if (chain.empty()) return false;

    for (const auto& level : chain) {
        if (isDirectory(level)) continue;
        if (makeDir(level) != 0 && !isDirectory(level)) return false;
    }

    // Copy first, and only touch the live file once the copy is whole: an archive
    // that was half written and a live file that was already emptied would be a
    // log that exists nowhere at all.
    FILE* source = fopen(livePath_.c_str(), "rb");
    if (source == nullptr) return false;

    const string target = chain.back() + "/" + fileName_;
    // "ab": one file per range, and a second rotation inside the same range is
    // more of the same log, not a reason to erase the first one.
    FILE* out = fopen(target.c_str(), "ab");
    if (out == nullptr) {
        fclose(source);
        return false;
    }

    bool ok = true;
    char buf[kCopyChunkBytes];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), source)) > 0) {
        if (fwrite(buf, 1, got, out) != got) { ok = false; break; }
    }
    if (ok && ferror(source) != 0) ok = false;
    if (ok && fflush(out) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    fclose(source);

    if (!ok) {
        // The card took part of the file and then refused the rest: the partial
        // copy is removed, so the range's file never claims to hold a log it only
        // half has. The live file is untouched.
        remove(target.c_str());
    }
    return ok;
}

void LogFileWriter::startFresh(const string& content)
{
    FILE* file = fopen(livePath_.c_str(), "wb");
    if (file == nullptr) {
        // Nothing was written; the next append will find the old file still
        // there and count the loss in lines(), which is the honest answer.
        return;
    }
    if (!content.empty()) fwrite(content.data(), 1, content.size(), file);
    fflush(file);
    fclose(file);

    lines_ = 0;
    for (const char c : content) {
        if (c == '\n') ++lines_;
    }
}

void LogFileWriter::rotateIfNeeded(size_t incomingBytes)
{
    if (liveSize() + incomingBytes + 1 <= maxBytes_) return;   // +1 for the newline

    // A file whose stamps are all 1970 cannot name a folder on the card: the log
    // keeps growing until a real time appears in it (the operator asked for the
    // range to come from the first real date, not from the epoch).
    if (!firstRealSeen_) return;

    const string range = logRangeName(firstReal_, lastReal_);

    if (!cardMounted_ || !cardMounted_()) {
        // No card at all: the operator's rule is a fresh file holding the newest
        // lines — and that is not a failure, so nothing is reported.
        ++carryOvers_;
        startFresh(logTail(readLive(), kLogFileKeepLines));
        // The carried lines keep their own stamps; the range of the next archive
        // starts from the newest date this writer has seen, which is the date of
        // the newest carried line.
        firstReal_ = lastReal_;
        return;
    }

    if (archiveToCard(range)) {
        failedAttempts_ = 0;
        ++archives_;
        startFresh(string());   // empty again, as the operator asked
        firstRealSeen_ = false; // the range of the next file is its own
        lines_ = 0;
        return;
    }

    // The card is there but refused the file. This file is the only copy of those
    // lines, so it stays and the attempt is repeated on the next record — one
    // attempt per record, ten records and then the tail is carried instead.
    ++failedAttempts_;
    string message = logTitle() + " archive failed (" + to_string(failedAttempts_) +
                     " of " + to_string(kLogFileMaxArchiveAttempts) + "): " + cardMount_ +
                     "/" + archiveFolder() + "/" + range + "/" + fileName_ + ", " +
                     to_string(liveSize()) + " bytes kept in " + livePath_;
    if (failedAttempts_ >= kLogFileMaxArchiveAttempts) {
        message += "; giving up and carrying the last " + to_string(kLogFileKeepLines) + " lines";
    }
    if (notice_) notice_(message);

    if (failedAttempts_ >= kLogFileMaxArchiveAttempts) {
        failedAttempts_ = 0;
        ++carryOvers_;
        startFresh(logTail(readLive(), kLogFileKeepLines));
        firstReal_ = lastReal_;
    }
}

void LogFileWriter::setMaxBytes(size_t bytes)
{
    maxBytes_ = bytes;

    // Strictly over, not equal: a file that fills its cap exactly is what the
    // writer aims at, so saving the size that produced it must not archive it.
    // (The rotation itself keeps its own guard about a file whose stamps are all
    // 1970: such a file cannot name a folder on the card, and it keeps growing
    // until a real time appears in it, exactly as it does on any other rotation.)
    if (liveSize() > maxBytes_) rotateIfNeeded(0);
}

bool LogFileWriter::append(const string& line, const LogStamp& stamp)
{
    if (!ensureLiveDirectory()) return false;

    const string text = line + "\n";
    rotateIfNeeded(text.size());

    FILE* file = fopen(livePath_.c_str(), "ab");
    if (file == nullptr) return false;

    const size_t written = fwrite(text.data(), 1, text.size(), file);
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (written != text.size() || !flushed || !closed) return false;

    ++lines_;
    rememberStamp(stamp);
    return true;
}

} // namespace core
} // namespace dhcp
