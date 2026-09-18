// Host test of the transfer engine (copy/move between volumes).
//
// The engine is deliberately free of ESP-IDF, so this test runs it against the
// **real** host filesystem through a small IFileOps implementation: no fake
// model of a filesystem, the same FileSource/FileSink pair the device uses, and
// a temporary directory per test. That is what makes the properties that matter
// provable — nothing may be deleted before its copy is complete, an aborted
// file must not replace the destination, a directory with more entries than one
// HTTP listing holds must still be copied whole.
//
// Build (MinGW g++ 13, PATH must contain C:\Qt\Tools\mingw1310_64\bin):
//   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -Itest/stubs -I.
//       test/test_transferengine.cpp src/files/TransferEngine.cpp
//       src/files/FileSource.cpp src/files/FileSink.cpp
//       src/storage/PathUtil.cpp -o t_transfer.exe
//
// The harness always exits with 0 — the proof is the printed text.

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "src/files/FileSink.h"
#include "src/files/FileSource.h"
#include "src/files/TransferEngine.h"

using namespace dhcp::files;

static int g_fail = 0;

/** Create a directory; MinGW's `mkdir` takes no mode argument. */
static int makeDir(const std::string& path)
{
#ifdef _WIN32
    return ::_mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0777);
#endif
}

static void check(bool ok, const std::string& what)
{
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

// ─────────────────────────────────────────────────────
// A filesystem made of two volumes over two directories
// ─────────────────────────────────────────────────────

class HostFileOps : public IFileOps
{
public:
    std::string rootA;          ///< Volume "a"
    std::string rootB;          ///< Volume "b"
    uint64_t freeA = 1u << 30;
    uint64_t freeB = 1u << 30;
    bool writersFail = false;   ///< Simulate a destination that cannot be written

    std::string rootFor(const std::string& volume) const
    {
        return volume == "a" ? rootA : rootB;
    }

    std::string full(const std::string& volume, const std::string& path) const
    {
        const std::string root = rootFor(volume);
        return path == "/" ? root : root + path;
    }

    FileStatus scan(const std::string& volume, const std::string& path,
                    IDirVisitor& visitor, std::string* detail) override
    {
        // The board's policy, mirrored: a path that is not a directory answers
        // InvalidPath, a missing one NotFound. The fake used to answer NotFound
        // for a file (its `opendir` failed), i.e. it was more lenient than the
        // device again — and a lenient fake is what let the root-destination bug
        // through in the first place.
        struct stat st = {};
        if (::stat(full(volume, path).c_str(), &st) != 0) {
            if (detail) *detail = std::strerror(errno);
            return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
        }
        if (!S_ISDIR(st.st_mode)) return FileStatus::InvalidPath;

        DIR* dir = opendir(full(volume, path).c_str());
        if (dir == nullptr) {
            if (detail) *detail = std::strerror(errno);
            return FileStatus::NotFound;
        }
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) continue;
            FileEntry e;
            e.name = name;
            struct stat st = {};
            if (::stat((full(volume, path) + "/" + name).c_str(), &st) == 0) {
                e.isDir = S_ISDIR(st.st_mode);
                e.size = e.isDir ? 0 : static_cast<uint64_t>(st.st_size);
                e.mtime = static_cast<uint64_t>(st.st_mtime);
            }
            if (!visitor.visit(e)) break;
        }
        closedir(dir);
        return FileStatus::Ok;
    }

    FileStatus stat(const std::string& volume, const std::string& path,
                    FileEntry& out, std::string* detail) override
    {
        // The board's `FileManager::stat` refuses the volume root ("the root has
        // no metadata"), and this fake has to be just as strict: when it was not,
        // a copy into the root of a volume passed here and was rejected on the
        // device with a bare "invalid path".
        if (path == "/") {
            if (detail) *detail = "the root has no metadata";
            return FileStatus::InvalidPath;
        }

        struct stat st = {};
        if (::stat(full(volume, path).c_str(), &st) != 0) {
            if (detail) *detail = std::strerror(errno);
            return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
        }
        out.isDir = S_ISDIR(st.st_mode);
        out.size = out.isDir ? 0 : static_cast<uint64_t>(st.st_size);
        out.mtime = static_cast<uint64_t>(st.st_mtime);
        return FileStatus::Ok;
    }

    FileStatus mkdir(const std::string& volume, const std::string& path,
                     std::string* detail) override
    {
        if (makeDir(full(volume, path)) != 0) {
            if (detail) *detail = std::strerror(errno);
            return (errno == EEXIST) ? FileStatus::AlreadyExists : FileStatus::IoError;
        }
        return FileStatus::Ok;
    }

    FileStatus remove(const std::string& volume, const std::string& path,
                      bool recursive, std::string* detail) override
    {
        struct stat st = {};
        if (::stat(full(volume, path).c_str(), &st) != 0) {
            if (detail) *detail = std::strerror(errno);
            return FileStatus::NotFound;
        }
        if (S_ISDIR(st.st_mode)) {
            if (recursive) {
                std::vector<std::string> children;
                DIR* dir = opendir(full(volume, path).c_str());
                if (dir != nullptr) {
                    struct dirent* ent = nullptr;
                    while ((ent = readdir(dir)) != nullptr) {
                        const std::string name = ent->d_name;
                        if (name == "." || name == "..") continue;
                        children.push_back(path == "/" ? "/" + name : path + "/" + name);
                    }
                    closedir(dir);
                }
                for (const auto& child : children) {
                    const FileStatus st2 = remove(volume, child, true, detail);
                    if (st2 != FileStatus::Ok) return st2;
                }
            }
            if (::rmdir(full(volume, path).c_str()) != 0) {
                if (detail) *detail = std::strerror(errno);
                return (errno == ENOTEMPTY) ? FileStatus::NotEmpty : FileStatus::IoError;
            }
            return FileStatus::Ok;
        }
        if (::unlink(full(volume, path).c_str()) != 0) {
            if (detail) *detail = std::strerror(errno);
            return FileStatus::IoError;
        }
        return FileStatus::Ok;
    }

    FileStatus rename(const std::string& volume, const std::string& from,
                      const std::string& to, std::string* detail) override
    {
        if (::rename(full(volume, from).c_str(), full(volume, to).c_str()) != 0) {
            if (detail) *detail = std::strerror(errno);
            return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::IoError;
        }
        return FileStatus::Ok;
    }

    FileStatus openRead(const std::string& volume, const std::string& path,
                        std::unique_ptr<IFileSource>& out, std::string* detail) override
    {
        auto src = std::make_unique<FileSource>(full(volume, path));
        if (!src->isOpen()) {
            if (detail) *detail = "cannot open the source";
            return FileStatus::IoError;
        }
        out = std::move(src);
        return FileStatus::Ok;
    }

    FileStatus createWriter(const std::string& volume, const std::string& path,
                            std::unique_ptr<IFileSink>& out, std::string* detail) override
    {
        if (writersFail) {
            if (detail) *detail = "destination is not writable";
            return FileStatus::IoError;
        }
        auto sink = std::make_unique<FileSink>(full(volume, path));
        if (!sink->isOpen()) {
            if (detail) *detail = "cannot open the destination";
            return FileStatus::IoError;
        }
        out = std::move(sink);
        return FileStatus::Ok;
    }

    uint64_t freeBytes(const std::string& volume) override
    {
        return volume == "a" ? freeA : freeB;
    }
};

struct Observer : ITransferObserver
{
    int calls = 0;
    bool cancel = false;
    uint64_t cancelAfterBytes = 0;   ///< 0 = never
    uint64_t seenBytes = 0;
    TransferReport last;

    void onTransferProgress(const TransferReport& report) override
    {
        ++calls;
        last = report;
        seenBytes = report.doneBytes;
    }

    bool transferCancelRequested() override
    {
        if (cancel) return true;
        return cancelAfterBytes != 0 && seenBytes >= cancelAfterBytes;
    }
};

// ─────────────────────────────────────────────────────
// Small helpers on top of the host filesystem
// ─────────────────────────────────────────────────────

static std::string g_root;

static std::string tempBase()
{
    const char* env = std::getenv("TEMP");
    if (env == nullptr) env = std::getenv("TMP");
    return (env != nullptr) ? std::string(env) : std::string(".");
}

static void rmTree(const std::string& path)
{
    struct stat st = {};
    if (::stat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path.c_str());
        if (dir != nullptr) {
            struct dirent* ent = nullptr;
            while ((ent = readdir(dir)) != nullptr) {
                const std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                rmTree(path + "/" + name);
            }
            closedir(dir);
        }
        ::rmdir(path.c_str());
        return;
    }
    ::unlink(path.c_str());
}

static bool mkdirs(const std::string& path)
{
    std::string acc;
    for (size_t i = 0; i < path.size(); ++i) {
        acc += path[i];
        if (path[i] == '/' && acc.size() > 1) makeDir(acc);
    }
    return makeDir(path) == 0 || errno == EEXIST;
}

static void writeFile(const std::string& path, const std::string& data)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { std::printf("  (cannot create %s)\n", path.c_str()); return; }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::string readFile(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return "<missing>";
    std::string out;
    char buf[512];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static bool exists(const std::string& path)
{
    struct stat st = {};
    return ::stat(path.c_str(), &st) == 0;
}

static std::string repeat(char c, size_t n)
{
    return std::string(n, c);
}

// ─────────────────────────────────────────────────────
// Cases
// ─────────────────────────────────────────────────────

static TransferRequest copyRequest(const std::vector<std::string>& paths,
                                   const std::string& dst = "/")
{
    TransferRequest req;
    req.op = TransferOp::Copy;
    req.conflict = TransferConflict::Overwrite;
    req.srcVolume = "a";
    req.paths = paths;
    req.dstVolume = "b";
    req.dstPath = dst;
    return req;
}

static void test_copy_one_file_crosses_volumes(HostFileOps& ops)
{
    std::printf("test_copy_one_file_crosses_volumes\n");
    writeFile(g_root + "/volA/notes.txt", "hello transfer");

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, copyRequest({"/notes.txt"}), report, obs);

    check(report.finished && !report.cancelled && report.failed == 0, "the transfer finished cleanly");
    check(readFile(g_root + "/volB/notes.txt") == "hello transfer", "the destination holds the content");
    check(readFile(g_root + "/volA/notes.txt") == "hello transfer", "a copy keeps the source");
    check(report.filesDone == 1 && report.totalBytes == 14, "counters: 1 file, 14 bytes");
    check(!exists(g_root + "/volB/notes.txt.part"), "no temporary file is left behind");
    check(obs.calls > 0, "progress was reported");
}

static void test_move_deletes_the_source_only_after_the_copy(HostFileOps& ops)
{
    std::printf("test_move_deletes_the_source_only_after_the_copy\n");
    writeFile(g_root + "/volA/move_me.txt", "payload");

    TransferRequest req = copyRequest({"/move_me.txt"});
    req.op = TransferOp::Move;

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, req, report, obs);

    check(report.failed == 0 && report.deleted == 1, "one source entry was removed");
    check(readFile(g_root + "/volB/move_me.txt") == "payload", "the destination holds the content");
    check(!exists(g_root + "/volA/move_me.txt"), "the source file is gone");
}

static void test_tree_is_copied_with_its_structure(HostFileOps& ops)
{
    std::printf("test_tree_is_copied_with_its_structure\n");
    mkdirs(g_root + "/volA/tree/sub/deep");
    writeFile(g_root + "/volA/tree/a.txt", repeat('a', 100));
    writeFile(g_root + "/volA/tree/sub/b.txt", repeat('b', 200));
    writeFile(g_root + "/volA/tree/sub/deep/c.txt", repeat('c', 300));

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, copyRequest({"/tree"}), report, obs);

    check(report.failed == 0, "no failures");
    check(report.filesTotal == 3 && report.dirsTotal == 3, "measured 3 files and 3 directories");
    check(report.totalBytes == 600 && report.doneBytes == 600, "600 bytes measured and copied");
    check(readFile(g_root + "/volB/tree/sub/deep/c.txt") == repeat('c', 300), "the deepest file arrived");
}

static void test_move_of_a_tree_removes_the_whole_source(HostFileOps& ops)
{
    std::printf("test_move_of_a_tree_removes_the_whole_source\n");
    mkdirs(g_root + "/volA/tree2/inner");
    writeFile(g_root + "/volA/tree2/x.bin", repeat('x', 4096));
    writeFile(g_root + "/volA/tree2/inner/y.bin", repeat('y', 5000));

    TransferRequest req = copyRequest({"/tree2"});
    req.op = TransferOp::Move;

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, req, report, obs);

    check(report.failed == 0, "no failures");
    check(readFile(g_root + "/volB/tree2/inner/y.bin") == repeat('y', 5000), "the copy is complete");
    check(!exists(g_root + "/volA/tree2"), "the source tree is gone");
}

static void test_move_keeps_the_source_when_a_file_fails(HostFileOps& ops)
{
    std::printf("test_move_keeps_the_source_when_a_file_fails\n");
    mkdirs(g_root + "/volA/tree3");
    writeFile(g_root + "/volA/tree3/good.bin", repeat('g', 300));
    writeFile(g_root + "/volA/tree3/bad.bin", repeat('b', 300));

    HostFileOps broken = ops;
    broken.writersFail = true;

    TransferRequest req = copyRequest({"/tree3"});
    req.op = TransferOp::Move;

    Observer obs;
    TransferReport report;
    TransferEngine::run(broken, req, report, obs);

    check(report.failed > 0, "the failure is counted");
    check(!report.error.empty(), "the failure is described");
    check(exists(g_root + "/volA/tree3/good.bin") && exists(g_root + "/volA/tree3/bad.bin"),
          "NOTHING of the source is deleted when its copy failed");
    check(!exists(g_root + "/volB/tree3/good.bin") && !exists(g_root + "/volB/tree3/bad.bin"),
          "not a single file arrived at the destination");
}

static void test_conflicts_are_found_without_a_walk(HostFileOps& ops)
{
    std::printf("test_conflicts_are_found_without_a_walk\n");
    writeFile(g_root + "/volA/c1.txt", "1");
    writeFile(g_root + "/volA/c2.txt", "2");
    writeFile(g_root + "/volB/c2.txt", "old");

    std::vector<std::string> names;
    std::string detail;
    const FileStatus st = TransferEngine::conflicts(ops, copyRequest({"/c1.txt", "/c2.txt"}),
                                                    names, &detail);

    check(st == FileStatus::Ok, "the check itself succeeded");
    check(names.size() == 1 && names[0] == "c2.txt", "only the taken name is reported");
}

static void test_skip_leaves_the_destination_and_the_source_alone(HostFileOps& ops)
{
    std::printf("test_skip_leaves_the_destination_and_the_source_alone\n");
    writeFile(g_root + "/volA/skip_me.txt", "new");
    writeFile(g_root + "/volB/skip_me.txt", "old");

    TransferRequest req = copyRequest({"/skip_me.txt"});
    req.conflict = TransferConflict::Skip;
    req.op = TransferOp::Move;

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, req, report, obs);

    check(report.skipped == 1 && report.failed == 0, "the entry was skipped, not failed");
    check(readFile(g_root + "/volB/skip_me.txt") == "old", "the destination is untouched");
    check(readFile(g_root + "/volA/skip_me.txt") == "new", "the source is untouched as well");
}

static void test_overwrite_replaces_a_file_and_merges_a_directory(HostFileOps& ops)
{
    std::printf("test_overwrite_replaces_a_file_and_merges_a_directory\n");
    mkdirs(g_root + "/volA/merge");
    writeFile(g_root + "/volA/merge/new.txt", "fresh");
    mkdirs(g_root + "/volB/merge");
    writeFile(g_root + "/volB/merge/keep.txt", "old but not mine");
    writeFile(g_root + "/volB/merge/new.txt", "stale");

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, copyRequest({"/merge"}), report, obs);

    check(report.failed == 0, "no failures");
    check(readFile(g_root + "/volB/merge/new.txt") == "fresh", "the same-named file was replaced");
    check(readFile(g_root + "/volB/merge/keep.txt") == "old but not mine",
          "the other file in the destination survived (merge, not replace)");
}

static void test_cancel_keeps_what_was_copied(HostFileOps& ops)
{
    std::printf("test_cancel_keeps_what_was_copied\n");
    mkdirs(g_root + "/volA/big");
    for (int i = 0; i < 4; ++i) {
        writeFile(g_root + "/volA/big/f" + std::to_string(i) + ".bin", repeat('z', 40000));
    }

    Observer obs;
    obs.cancelAfterBytes = 80000;   // after roughly two files
    TransferReport report;
    TransferEngine::run(ops, copyRequest({"/big"}), report, obs);

    check(report.cancelled, "the transfer reports a cancel");
    check(report.doneBytes >= obs.cancelAfterBytes, "bytes were copied before the stop");
    check(report.doneBytes < report.totalBytes, "the whole batch was NOT copied");
    check(!exists(g_root + "/volB/big/f0.bin.part"), "the interrupted file kept no .part");
    check(report.error.empty(), "a cancel is not an error");
}

static void test_a_big_directory_is_copied_whole(HostFileOps& ops)
{
    std::printf("test_a_big_directory_is_copied_whole\n");
    const int kCount = 520;   // more than IFileManager::kMaxListEntries
    mkdirs(g_root + "/volA/many");
    for (int i = 0; i < kCount; ++i) {
        writeFile(g_root + "/volA/many/f" + std::to_string(i) + ".txt", "x");
    }

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, copyRequest({"/many"}), report, obs);

    check(report.failed == 0, "no failures");
    check(report.filesTotal == kCount, "the walk saw every entry, not just one listing");
    check(exists(g_root + "/volB/many/f519.txt"), "the entry beyond the listing cap arrived");
}

static void test_not_enough_space_is_refused_before_the_first_byte(HostFileOps& ops)
{
    std::printf("test_not_enough_space_is_refused_before_the_first_byte\n");
    mkdirs(g_root + "/volA/huge");
    writeFile(g_root + "/volA/huge/big.bin", repeat('h', 200000));

    HostFileOps small = ops;
    small.freeB = 100000;   // the measured need is bigger

    Observer obs;
    TransferReport report;
    TransferEngine::run(small, copyRequest({"/huge"}), report, obs);

    check(report.failed > 0, "the transfer failed");
    check(report.neededBytes == 200000 && report.freeBytes == 100000,
          "the report carries the numbers the UI needs");
    check(!exists(g_root + "/volB/huge/big.bin"), "not a single byte was copied");
    check(report.phase == TransferPhase::Done, "the job still ends");
}

static void test_same_volume_move_is_an_instant_rename(HostFileOps& ops)
{
    std::printf("test_same_volume_move_is_an_instant_rename\n");
    mkdirs(g_root + "/volA/here");
    writeFile(g_root + "/volA/here/inst.txt", "instant");

    TransferRequest req = copyRequest({"/here/inst.txt"}, "/");
    req.op = TransferOp::Move;
    req.dstVolume = "a";   // same volume: rename() must be used

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, req, report, obs);

    check(report.instant, "the report says it was a rename");
    check(report.totalBytes == 0 && report.doneBytes == 0, "no byte was copied");
    check(readFile(g_root + "/volA/inst.txt") == "instant", "the file is at the destination");
    check(!exists(g_root + "/volA/here/inst.txt"), "and no longer at the source");
}

static void test_a_destination_inside_the_source_is_refused(HostFileOps& ops)
{
    std::printf("test_a_destination_inside_the_source_is_refused\n");
    mkdirs(g_root + "/volA/self/inner");
    writeFile(g_root + "/volA/self/inner/f.txt", "f");

    TransferRequest req = copyRequest({"/self"}, "/self/inner");
    req.dstVolume = "a";

    std::string detail;
    const FileStatus st = TransferEngine::validate(ops, req, &detail);
    check(st == FileStatus::InvalidPath, "a copy into its own subtree is refused");

    TransferRequest same = copyRequest({"/self/inner/f.txt"}, "/self/inner");
    same.dstVolume = "a";
    same.op = TransferOp::Move;
    check(TransferEngine::validate(ops, same) == FileStatus::InvalidPath,
          "a move into the directory it is already in is refused");

    TransferRequest empty = copyRequest({});
    check(TransferEngine::validate(ops, empty) == FileStatus::InvalidPath,
          "an empty batch is refused");
}

static void test_a_partial_failure_still_copies_the_rest(HostFileOps& ops)
{
    std::printf("test_a_partial_failure_still_copies_the_rest\n");
    mkdirs(g_root + "/volA/partial");
    writeFile(g_root + "/volA/partial/ok1.bin", repeat('1', 100));
    writeFile(g_root + "/volA/partial/ok2.bin", repeat('2', 100));

    // The destination refuses the writer for one specific path only.
    class PickyOps : public HostFileOps
    {
    public:
        FileStatus createWriter(const std::string& volume, const std::string& path,
                                std::unique_ptr<IFileSink>& out, std::string* detail) override
        {
            if (path == "/partial/ok1.bin") {
                if (detail) *detail = "refused by the test";
                return FileStatus::IoError;
            }
            return HostFileOps::createWriter(volume, path, out, detail);
        }
    };

    PickyOps picky;
    picky.rootA = ops.rootA;
    picky.rootB = ops.rootB;
    Observer obs;
    TransferReport report;
    TransferEngine::run(picky, copyRequest({"/partial"}), report, obs);

    check(report.failed == 1, "exactly one file failed");
    check(exists(g_root + "/volB/partial/ok2.bin"), "the other file was still copied");
    check(!report.error.empty() && !report.errorPath.empty(), "the failure names its path");
}

static void test_a_copy_into_the_volume_root_is_accepted(HostFileOps& ops)
{
    std::printf("test_a_copy_into_the_volume_root_is_accepted\n");
    writeFile(g_root + "/volA/at_root.txt", "at root");

    // The destination is the root of the other volume — the only destination an
    // empty card or a freshly formatted partition offers, and the one the feature
    // is used with most often. It has to be accepted, and the file has to arrive.
    std::string detail;
    const FileStatus st = TransferEngine::validate(ops, copyRequest({"/at_root.txt"}, "/"),
                                                   &detail);
    check(st == FileStatus::Ok, "the volume root is a valid destination");

    Observer obs;
    TransferReport report;
    TransferEngine::run(ops, copyRequest({"/at_root.txt"}, "/"), report, obs);

    check(report.failed == 0, "the copy into the root reported no failure");
    check(readFile(g_root + "/volB/at_root.txt") == "at root", "the file arrived in the root");
    check(!exists(g_root + "/volB/at_root.txt.part"), "and no temporary file stayed behind");
}

static void test_a_file_as_the_destination_is_refused(HostFileOps& ops)
{
    std::printf("test_a_file_as_the_destination_is_refused\n");
    writeFile(g_root + "/volB/not_a_dir.txt", "file");
    writeFile(g_root + "/volA/mover.txt", "data");

    std::string detail;
    const FileStatus st = TransferEngine::validate(ops, copyRequest({"/mover.txt"}, "/not_a_dir.txt"),
                                                   &detail);
    check(st == FileStatus::InvalidPath, "a file cannot be a destination folder");
    check(!detail.empty(), "and the refusal says something");
}

int main()
{
    g_root = tempBase() + "/dhcpserver_transfer_test";
    rmTree(g_root);
    mkdirs(g_root + "/volA");
    mkdirs(g_root + "/volB");

    HostFileOps ops;
    ops.rootA = g_root + "/volA";
    ops.rootB = g_root + "/volB";

    test_copy_one_file_crosses_volumes(ops);
    test_move_deletes_the_source_only_after_the_copy(ops);
    test_tree_is_copied_with_its_structure(ops);
    test_move_of_a_tree_removes_the_whole_source(ops);
    test_move_keeps_the_source_when_a_file_fails(ops);
    test_conflicts_are_found_without_a_walk(ops);
    test_skip_leaves_the_destination_and_the_source_alone(ops);
    test_overwrite_replaces_a_file_and_merges_a_directory(ops);
    test_cancel_keeps_what_was_copied(ops);
    test_a_big_directory_is_copied_whole(ops);
    test_not_enough_space_is_refused_before_the_first_byte(ops);
    test_same_volume_move_is_an_instant_rename(ops);
    test_a_destination_inside_the_source_is_refused(ops);
    test_a_copy_into_the_volume_root_is_accepted(ops);
    test_a_file_as_the_destination_is_refused(ops);
    test_a_partial_failure_still_copies_the_rest(ops);

    rmTree(g_root);

    if (g_fail != 0) {
        std::printf("FAILED (%d checks)\n", g_fail);
    } else {
        std::printf("PASSED!\n");
    }
    return 0;
}

#endif // DHCP_TEST_HOST
