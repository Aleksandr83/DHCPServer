#include "TransferEngine.h"

#include <cstddef>
#include <utility>

#include "../storage/PathUtil.h"

using namespace std;

namespace dhcp {
namespace files {

namespace {

using storage::PathUtil;

/** @brief One directory pair of the walk: where it comes from, where it goes. */
struct DirPair {
    string src;
    string dst;
};

/** @brief Totals of one source entry (a file, or a whole tree). */
struct Measure {
    uint64_t bytes = 0;
    uint32_t files = 0;
    uint32_t dirs = 0;
};

/** @brief Append @p name to a normalized directory path. */
string joinRel(const string& dir, const string& name)
{
    return (dir == "/") ? ("/" + name) : (dir + "/" + name);
}

/**
 * @brief Everything one transfer needs, plus the progress throttling.
 *
 * The report is written through this object rather than passed around, because
 * every step has to keep the same invariants: a failure is recorded once (with
 * the path it happened on), a cancel is never recorded as a failure, and the
 * observer only hears about progress when enough bytes have moved to be worth a
 * mutex and a JSON round-trip.
 */
class Runner {
public:
    Runner(IFileOps& ops, const TransferRequest& req, TransferReport& report,
           ITransferObserver& observer)
        : ops_(ops), req_(req), report_(report), observer_(observer) {}

    IFileOps& ops() { return ops_; }
    const TransferRequest& req() const { return req_; }
    TransferReport& report() { return report_; }
    string& detail() { return detail_; }

    /** @brief Publish progress (throttled unless @p force). */
    void progress(bool force)
    {
        if (!force && report_.doneBytes - lastReported_ < TransferEngine::kProgressChunk) return;
        lastReported_ = report_.doneBytes;
        observer_.onTransferProgress(report_);
    }

    /** @brief True when the operator asked to stop (also marks the report). */
    bool cancelRequested()
    {
        if (!observer_.transferCancelRequested()) return false;
        report_.cancelled = true;
        return true;
    }

    /** @brief True once the run must stop (cancelled). */
    bool stopped() const { return report_.cancelled; }

    /** @brief Record one failure; the first one is the one the UI reports. */
    void fail(const string& path, FileStatus status, const string& why)
    {
        ++report_.failed;
        if (!report_.error.empty()) return;
        report_.error = messageFor(status);
        if (!why.empty()) report_.error += ": " + why;
        report_.errorPath = path;
    }

private:
    IFileOps& ops_;
    const TransferRequest& req_;
    TransferReport& report_;
    ITransferObserver& observer_;
    uint64_t lastReported_ = 0;
    string detail_;
};

/** @brief Adds up one directory while walking it (no listing is materialized). */
class MeasureVisitor : public IDirVisitor {
public:
    MeasureVisitor(Measure& measure, vector<string>& queue, string dir)
        : measure_(measure), queue_(queue), dir_(move(dir)) {}

    bool visit(const FileEntry& entry) override
    {
        if (entry.isDir) {
            ++measure_.dirs;
            queue_.push_back(joinRel(dir_, entry.name));
        } else {
            ++measure_.files;
            measure_.bytes += entry.size;
        }
        return true;
    }

private:
    Measure& measure_;
    vector<string>& queue_;
    string dir_;
};

/**
 * @brief Measure @p top (a file, or a tree) by walking it with a queue.
 *
 * A queue instead of recursion keeps the memory bounded by the number of
 * *directories* on one level, not by the size of the tree: a card with hundred
 * thousand files in a single directory is walked with one entry in memory at a
 * time, which is also why @ref IFileOps::scan exists (see @ref IDirVisitor).
 */
FileStatus measureEntry(Runner& r, const string& top, Measure& out)
{
    FileEntry entry;
    const FileStatus st = r.ops().stat(r.req().srcVolume, top, entry, &r.detail());
    if (st != FileStatus::Ok) return st;

    if (!entry.isDir) {
        ++out.files;
        out.bytes += entry.size;
        return FileStatus::Ok;
    }

    ++out.dirs;
    vector<string> queue;   // directories still to walk, parents first
    queue.push_back(top);

    for (size_t i = 0; i < queue.size(); ++i) {
        // The path is copied before the walk: the visitor appends to the same
        // vector, so an element of it must not be held as a reference meanwhile.
        const string dir = queue[i];
        r.report().current = dir;
        const uint32_t before = out.files + out.dirs;
        MeasureVisitor visitor(out, queue, dir);
        const FileStatus stDir = r.ops().scan(r.req().srcVolume, dir, visitor, &r.detail());
        if (stDir != FileStatus::Ok) return stDir;
        // The walk of a large card takes minutes and moves no bytes, so the byte
        // throttle above would never fire: publish every 64 entries instead, which
        // is what lets the page say "measuring … 1 234 entries" instead of
        // looking hung.
        if ((out.files + out.dirs) / 64 != before / 64) r.progress(true);
    }
    return FileStatus::Ok;
}

/** @brief Copies one file, watching for space, cancellation and read errors. */
bool copyFile(Runner& r, const string& src, const string& dst)
{
    FileEntry entry;
    if (r.ops().stat(r.req().srcVolume, src, entry, &r.detail()) == FileStatus::Ok &&
        !entry.isDir) {
        // Refuse this file before the first byte rather than filling the volume
        // halfway: a full destination is the normal case when the internal
        // partition (~21 MB) is the target of a card's directory.
        if (r.ops().freeBytes(r.req().dstVolume) < entry.size + TransferEngine::kFreeSpaceReserve) {
            r.fail(src, FileStatus::NoSpace, "not enough free space on the destination");
            return false;
        }
    }

    unique_ptr<IFileSource> in;
    FileStatus st = r.ops().openRead(r.req().srcVolume, src, in, &r.detail());
    if (st != FileStatus::Ok) {
        r.fail(src, st, r.detail());
        return false;
    }

    unique_ptr<IFileSink> out;
    st = r.ops().createWriter(r.req().dstVolume, dst, out, &r.detail());
    if (st != FileStatus::Ok) {
        r.fail(src, st, r.detail());
        return false;
    }

    uint8_t buffer[TransferEngine::kFileChunk];
    for (;;) {
        const size_t got = in->read(buffer, sizeof(buffer));
        if (got == 0) {
            if (in->error()) {
                out->abort();
                r.fail(src, FileStatus::IoError, "cannot read the source file");
                return false;
            }
            break;   // end of file
        }
        if (!out->write(buffer, got)) {
            out->abort();
            r.fail(src, FileStatus::IoError, "cannot write to the destination");
            return false;
        }
        r.report().doneBytes += got;
        r.progress(false);
        if (r.cancelRequested()) {
            // The destination never sees a half file: the sink writes `.part`
            // and abort() removes it.
            out->abort();
            return false;
        }
    }

    if (!out->commit()) {
        out->abort();
        r.fail(src, FileStatus::IoError, "cannot publish the destination file");
        return false;
    }

    ++r.report().filesDone;
    r.progress(true);
    return true;
}

/** @brief Copies the contents of one directory into an existing directory. */
class TreeVisitor : public IDirVisitor {
public:
    TreeVisitor(Runner& r, vector<DirPair>& queue, string src, string dst)
        : r_(r), queue_(queue), src_(move(src)), dst_(move(dst)) {}

    bool visit(const FileEntry& entry) override
    {
        string dstChild;
        if (!PathUtil::normalizeChild(dst_, entry.name, dstChild)) {
            // The name came off the volume, so it is valid there, but it may not
            // be a name this API can address. Refusing it is the honest answer:
            // creating it would produce an entry the explorer cannot show.
            r_.fail(joinRel(src_, entry.name), FileStatus::InvalidPath,
                    "the name cannot be used on the destination");
            ok_ = false;
            return true;
        }

        const string srcChild = joinRel(src_, entry.name);
        if (entry.isDir) {
            const FileStatus st = r_.ops().mkdir(r_.req().dstVolume, dstChild);
            if (st != FileStatus::Ok && st != FileStatus::AlreadyExists) {
                r_.fail(srcChild, st, r_.detail());
                ok_ = false;
                return true;
            }
            ++r_.report().dirsDone;
            queue_.push_back({srcChild, dstChild});
            return true;
        }

        r_.report().current = srcChild;
        if (!copyFile(r_, srcChild, dstChild)) ok_ = false;
        return true;
    }

    bool ok() const { return ok_; }

private:
    Runner& r_;
    vector<DirPair>& queue_;
    string src_;
    string dst_;
    bool ok_ = true;
};

/**
 * @brief Copy one top-level entry (file or tree), returns true when complete.
 *
 * @p dstTop must already exist as a directory when the source is one: the
 * caller creates it, because it also decides what a taken name means.
 */
bool copyEntry(Runner& r, const string& srcTop, const string& dstTop,
               bool srcIsDir)
{
    if (!srcIsDir) {
        r.report().current = srcTop;
        return copyFile(r, srcTop, dstTop);
    }

    vector<DirPair> queue;
    queue.push_back({srcTop, dstTop});
    bool ok = true;
    for (size_t i = 0; i < queue.size(); ++i) {
        if (r.stopped()) return false;
        // See measureEntry: the visitor appends to this very vector, so both
        // paths are copied out before the directory is walked.
        const string srcDir = queue[i].src;
        const string dstDir = queue[i].dst;
        TreeVisitor visitor(r, queue, srcDir, dstDir);
        const FileStatus st = r.ops().scan(r.req().srcVolume, srcDir, visitor, &r.detail());
        if (st != FileStatus::Ok) {
            r.fail(srcDir, st, r.detail());
            ok = false;
            continue;
        }
        if (!visitor.ok()) ok = false;
    }
    return ok;
}

/** @brief Mark the report as finished. */
void finishReport(TransferReport& report)
{
    report.busy = false;
    report.finished = true;
    report.phase = TransferPhase::Done;
    report.current.clear();
}

/** @brief Move inside one volume: `rename()`, no byte is copied. */
void runInstant(Runner& r, const string& dstDir)
{
    for (const auto& raw : r.req().paths) {
        if (r.cancelRequested()) return;

        string src;
        if (!PathUtil::normalize(raw, src)) continue;

        const string name = PathUtil::basename(src);
        string target;
        if (!PathUtil::normalizeChild(dstDir, name, target)) {
            r.fail(src, FileStatus::InvalidPath, "the destination name is not usable");
            continue;
        }

        FileEntry entry;
        const bool taken = r.ops().stat(r.req().srcVolume, target, entry) == FileStatus::Ok;
        if (taken) {
            // A directory cannot be merged by a rename, and replacing one would
            // destroy a tree: that is the one case the operator has to solve by
            // hand (or by a cross-volume move, which copies and merges).
            if (entry.isDir) {
                r.fail(src, FileStatus::AlreadyExists,
                       "the destination already holds a directory with that name");
                continue;
            }
            if (r.req().conflict == TransferConflict::Skip) {
                ++r.report().skipped;
                continue;
            }
            const FileStatus stDel = r.ops().remove(r.req().srcVolume, target, false,
                                                   &r.detail());
            if (stDel != FileStatus::Ok) {
                r.fail(src, stDel, r.detail());
                continue;
            }
        }

        const FileStatus st = r.ops().rename(r.req().srcVolume, src, target, &r.detail());
        if (st != FileStatus::Ok) {
            r.fail(src, st, r.detail());
            continue;
        }
        ++r.report().deleted;
        if (entry.isDir) {
            ++r.report().dirsDone;
        } else {
            ++r.report().filesDone;
        }
        r.progress(true);
    }
}

} // namespace

const char* transferPhaseName(TransferPhase phase)
{
    switch (phase) {
    case TransferPhase::Measuring: return "measuring";
    case TransferPhase::Copying:   return "copying";
    case TransferPhase::Done:      return "done";
    case TransferPhase::Idle:      break;
    }
    return "idle";
}

FileStatus TransferEngine::validate(IFileOps& ops, const TransferRequest& req,
                                    string* detail)
{
    if (req.paths.empty()) {
        if (detail) *detail = "no entries were selected";
        return FileStatus::InvalidPath;
    }
    if (req.srcVolume.empty() || req.dstVolume.empty()) {
        if (detail) *detail = "a volume is missing";
        return FileStatus::InvalidPath;
    }

    string dstDir;
    if (!PathUtil::normalize(req.dstPath, dstDir)) {
        if (detail) *detail = "the destination path is not valid";
        return FileStatus::InvalidPath;
    }

    for (const auto& raw : req.paths) {
        string path;
        if (!PathUtil::normalize(raw, path)) {
            // Name the offender: "invalid path" alone leaves the operator with a
            // file they can see, a button that refuses and no idea why.
            if (detail) *detail = "the path cannot be used by the API: " + raw;
            return FileStatus::InvalidPath;
        }
        if (path == "/") {
            if (detail) *detail = "a whole volume cannot be transferred";
            return FileStatus::InvalidPath;
        }

        FileEntry entry;
        const FileStatus st = ops.stat(req.srcVolume, path, entry, detail);
        if (st != FileStatus::Ok) return st;   // NotFound / NotMounted / Busy

        if (req.srcVolume == req.dstVolume) {
            // Moving an entry into the directory it already lives in does
            // nothing; copying it there would mean "copy onto itself".
            if (dstDir == PathUtil::parent(path)) {
                if (detail) *detail = "the entry is already in that directory";
                return FileStatus::InvalidPath;
            }
            // A directory must never be moved inside itself: the copy would walk
            // into what it is writing (/a → /a/b), and the tree would be detached.
            if (dstDir.rfind(path + "/", 0) == 0) {
                if (detail) *detail = "the destination is inside the source";
                return FileStatus::InvalidPath;
            }
        }
    }

    // The destination directory has to exist: transfers never create the folder
    // the operator picked, they only write into it.
    //
    // Probed with a walk, **not** with `stat`: the manager's `stat` refuses the
    // root of a volume ("the root has no metadata"), and the root is exactly the
    // destination this feature is pointed at most often — an empty card or a
    // freshly formatted partition has nothing else to offer. The probe visits at
    // most one entry and proves the path is a directory on the way (a file
    // answers InvalidPath). This is why the fake in the host test refuses the root
    // as well: it used to be more permissive than the board, and the copy into a
    // root therefore passed in tests and failed on the device.
    class DirProbe : public IDirVisitor {
    public:
        bool visit(const FileEntry&) override { return false; }
    };

    DirProbe probe;
    const FileStatus dstStatus = ops.scan(req.dstVolume, dstDir, probe, detail);
    if (dstStatus == FileStatus::InvalidPath && detail != nullptr && detail->empty()) {
        *detail = "the destination is not a directory";
    }
    return dstStatus;
}

FileStatus TransferEngine::conflicts(IFileOps& ops, const TransferRequest& req,
                                     vector<string>& names,
                                     string* detail)
{
    names.clear();

    const FileStatus st = validate(ops, req, detail);
    if (st != FileStatus::Ok) return st;

    string dstDir;
    PathUtil::normalize(req.dstPath, dstDir);

    for (const auto& raw : req.paths) {
        string path;
        if (!PathUtil::normalize(raw, path)) continue;

        string target;
        if (!PathUtil::normalizeChild(dstDir, PathUtil::basename(path), target)) continue;

        FileEntry entry;
        if (ops.stat(req.dstVolume, target, entry, nullptr) == FileStatus::Ok) {
            names.push_back(PathUtil::basename(path));
        }
    }
    return FileStatus::Ok;
}

void TransferEngine::run(IFileOps& ops, const TransferRequest& req,
                         TransferReport& report, ITransferObserver& observer)
{
    report = TransferReport{};
    report.op = req.op;
    report.srcVolume = req.srcVolume;
    report.dstVolume = req.dstVolume;
    report.dstPath = req.dstPath;
    report.busy = true;
    report.phase = TransferPhase::Measuring;
    observer.onTransferProgress(report);

    Runner r(ops, req, report, observer);

    const FileStatus valid = validate(ops, req, &r.detail());
    if (valid != FileStatus::Ok) {
        r.fail(req.dstPath, valid, r.detail());
        finishReport(report);
        observer.onTransferProgress(report);
        return;
    }

    string dstDir;
    PathUtil::normalize(req.dstPath, dstDir);
    report.dstPath = dstDir;

    // A move inside one volume is a rename: instant, and it cannot lose data.
    if (req.op == TransferOp::Move && req.srcVolume == req.dstVolume) {
        report.instant = true;
        runInstant(r, dstDir);
        finishReport(report);
        observer.onTransferProgress(report);
        return;
    }

    // ── 1. Measure ──────────────────────────────────────────────────────────
    // Everything the operator is about to move has to fit before the first byte
    // is written: discovering a full volume halfway through is the failure this
    // step exists to prevent. Entries that will be skipped are not measured —
    // they are not copied.
    for (const auto& raw : req.paths) {
        string path;
        if (!PathUtil::normalize(raw, path)) continue;

        string target;
        if (PathUtil::normalizeChild(dstDir, PathUtil::basename(path), target)) {
            FileEntry entry;
            if (req.conflict == TransferConflict::Skip &&
                ops.stat(req.dstVolume, target, entry, nullptr) == FileStatus::Ok) {
                // Not measured: it will not be copied. The skip itself is counted
                // once, in the copy pass — this walk only decides what has to fit.
                continue;
            }
            // Overwriting a same-named directory merges into it, so its existing
            // content is not counted either way: the measurement is conservative.
        }

        Measure measure;
        const FileStatus st = measureEntry(r, path, measure);
        if (st != FileStatus::Ok) {
            r.fail(path, st, r.detail());
            continue;
        }
        report.totalBytes += measure.bytes;
        report.filesTotal += measure.files;
        report.dirsTotal += measure.dirs;
    }

    report.neededBytes = report.totalBytes;
    report.freeBytes = ops.freeBytes(req.dstVolume);
    observer.onTransferProgress(report);

    if (report.freeBytes < report.neededBytes + kFreeSpaceReserve) {
        r.fail(dstDir, FileStatus::NoSpace, "not enough free space on the destination");
        finishReport(report);
        observer.onTransferProgress(report);
        return;
    }

    // ── 2. Copy ─────────────────────────────────────────────────────────────
    report.phase = TransferPhase::Copying;
    observer.onTransferProgress(report);

    for (const auto& raw : req.paths) {
        if (report.cancelled) break;

        string path;
        if (!PathUtil::normalize(raw, path)) continue;

        const string name = PathUtil::basename(path);
        string target;
        if (!PathUtil::normalizeChild(dstDir, name, target)) {
            r.fail(path, FileStatus::InvalidPath, "the destination name is not usable");
            continue;
        }

        FileEntry srcEntry;
        if (ops.stat(req.srcVolume, path, srcEntry, &r.detail()) != FileStatus::Ok) {
            r.fail(path, FileStatus::NotFound, r.detail());
            continue;
        }

        FileEntry dstEntry;
        const bool taken = ops.stat(req.dstVolume, target, dstEntry, nullptr) == FileStatus::Ok;
        if (taken) {
            if (req.conflict == TransferConflict::Skip) {
                ++report.skipped;
                continue;
            }
            if (srcEntry.isDir && !dstEntry.isDir) {
                // A file cannot become a directory: the operator has to remove it.
                r.fail(path, FileStatus::AlreadyExists,
                       "the destination holds a file with that name");
                continue;
            }
        }

        if (srcEntry.isDir) {
            const FileStatus st = ops.mkdir(req.dstVolume, target);
            if (st != FileStatus::Ok && st != FileStatus::AlreadyExists) {
                r.fail(path, st, r.detail());
                continue;
            }
            if (st == FileStatus::Ok) ++report.dirsDone;
        }

        const bool complete = copyEntry(r, path, target, srcEntry.isDir);
        if (report.cancelled) break;

        report.current = path;
        if (!complete) continue;

        // A move deletes the source entry only now: its copy is complete, and a
        // failure anywhere inside the tree left the whole source in place.
        if (req.op == TransferOp::Move) {
            const FileStatus stDel = ops.remove(req.srcVolume, path, srcEntry.isDir,
                                                &r.detail());
            if (stDel != FileStatus::Ok) {
                r.fail(path, stDel, r.detail());
                continue;
            }
            ++report.deleted;
        }
        observer.onTransferProgress(report);
    }

    finishReport(report);
    observer.onTransferProgress(report);
}

} // namespace files
} // namespace dhcp
