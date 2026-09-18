#ifndef DHCP_FILES_TRANSFERENGINE_H
#define DHCP_FILES_TRANSFERENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "IFileOps.h"

namespace dhcp {
namespace files {

/** @brief What a transfer does with its source entries. */
enum class TransferOp {
    Copy,   ///< Leaves the source in place
    Move    ///< Removes each source entry once its copy is complete
};

/** @brief What to do when the destination already holds an entry of that name. */
enum class TransferConflict {
    Ask,        ///< Stop and let the operator decide (the REST layer answers 409)
    Overwrite,  ///< Replace files, merge directories
    Skip        ///< Leave the destination entry alone and do not touch the source
};

/** @brief Phase of the transfer job, as reported to the UI. */
enum class TransferPhase {
    Idle,       ///< Nothing has run yet
    Measuring,  ///< Walking the source to learn how much has to be copied
    Copying,    ///< Moving bytes
    Done        ///< Finished (see @ref TransferReport::cancelled / `failed`)
};

/** @brief Name of a @ref TransferPhase (the value the REST API and UI use). */
const char* transferPhaseName(TransferPhase phase);

/**
 * @brief One transfer request: a batch of entries, source volume → destination.
 *
 * Paths are volume-relative and normalized by the engine, so the REST layer
 * passes what the screen sent. @ref paths holds **top-level** entries (files or
 * directories) taken from one directory; directories are walked recursively by
 * the engine.
 */
struct TransferRequest {
    TransferOp op = TransferOp::Copy;
    TransferConflict conflict = TransferConflict::Ask;
    std::string srcVolume;            ///< Volume id the entries come from
    std::vector<std::string> paths;   ///< Volume-relative source paths
    std::string dstVolume;            ///< Volume id the entries go to
    std::string dstPath;              ///< Volume-relative destination directory
};

/**
 * @brief Live state of the transfer job, and its result once it has finished.
 *
 * The same object is used three ways, which is why every number is here: the
 * job task updates it, the REST layer serialises a snapshot of it while the
 * transfer runs, and it is what the UI shows when it is over (`files_done`,
 * `skipped`, `failed`, `deleted`, the first error and the path it happened on).
 * Keeping one truth means the progress bar, the scheduler row and the summary
 * after the run can never disagree.
 */
struct TransferReport {
    TransferPhase phase = TransferPhase::Idle;
    bool busy = false;         ///< The job is running right now
    bool finished = false;     ///< The job has ended (normally, at an error or cancelled)
    bool cancelled = false;    ///< Ended because @ref ITransferObserver asked it to
    bool instant = false;      ///< Same-volume move: entries were renamed, no byte was copied
    TransferOp op = TransferOp::Copy;

    std::string srcVolume;     ///< Volume the entries came from
    std::string dstVolume;     ///< Volume they went to
    std::string dstPath;       ///< Destination directory
    std::string current;       ///< Path being worked on (empty when idle)

    uint64_t doneBytes = 0;    ///< Bytes copied (or planned, while measuring)
    uint64_t totalBytes = 0;   ///< Bytes to copy, measured before the copy starts
    uint64_t neededBytes = 0;  ///< What the measurement asked for (see @ref freeBytes)
    uint64_t freeBytes = 0;    ///< Free space of the destination when measured

    uint32_t filesDone = 0;    ///< Files copied
    uint32_t filesTotal = 0;   ///< Files to copy
    uint32_t dirsDone = 0;     ///< Directories created
    uint32_t dirsTotal = 0;    ///< Directories to create
    uint32_t skipped = 0;      ///< Top-level entries left alone because the name was taken
    uint32_t failed = 0;       ///< Entries (or files inside a tree) that could not be copied
    uint32_t deleted = 0;      ///< Source entries removed after a successful `move`

    std::string error;         ///< First failure, in English ("" when none)
    std::string errorPath;     ///< Path that failure happened on
};

/**
 * @brief Progress and stop requests of a running transfer.
 *
 * Implemented by the job's owner (`FileManager`), which publishes the report to
 * the REST layer and reports the same numbers to `core::JobRegistry`, so the
 * scheduler page can stop the transfer from there as well as from the Files
 * page.
 */
class ITransferObserver {
public:
    virtual ~ITransferObserver() = default;

    /** @brief Called at most every @ref TransferEngine::kProgressChunk bytes. */
    virtual void onTransferProgress(const TransferReport& report) = 0;

    /** @brief True when the operator asked to stop (checked between chunks). */
    virtual bool transferCancelRequested() = 0;
};

/**
 * @brief Copies and moves files and directories between volumes.
 *
 * One volume-to-volume transfer is a *stream*: read a window from the source,
 * write it to the destination, keep the counters moving. Three properties of
 * the device shape the design:
 *
 *  - **`rename()` cannot cross volumes.** FatFs is one filesystem per mounted
 *    volume, so a move between the internal FAT partition and the card is a
 *    copy followed by a delete — and a same-volume move is left to `rename()`,
 *    which is instant and cannot lose data (@ref TransferReport::instant).
 *  - **The destination is usually smaller.** The internal partition is ~21 MB
 *    and a card is measured in gigabytes, so copying the wrong way can fill it
 *    up: the source is measured first and the transfer is refused *before the
 *    first byte* when it does not fit (`neededBytes` / `freeBytes`), instead of
 *    failing halfway with a full volume.
 *  - **Nothing may be lost silently.** A source entry is deleted only after its
 *    copy is complete, an aborted file never replaces the destination (the sink
 *    writes `.part` and publishes on commit), and a whole source tree is
 *    removed only when every file inside it was copied.
 *
 * Deliberately free of ESP-IDF: the engine is host-tested over a real
 * temporary directory (see `test/test_transferengine.cpp`).
 */
class TransferEngine {
public:
    /**
     * @brief Bytes between two progress reports and two cancel checks.
     *
     * The file window is @ref kFileChunk; a bigger report interval keeps the
     * mutex and the job registry out of the inner loop.
     */
    static constexpr uint32_t kProgressChunk = 64 * 1024;

    /** @brief Read/write window of one file (stack buffer of the job task). */
    static constexpr uint32_t kFileChunk = 4096;

    /** @brief Space left unused on the destination (index blocks, FAT slack). */
    static constexpr uint64_t kFreeSpaceReserve = 4096;

    /**
     * @brief Check the request against both volumes before anything happens.
     *
     * Rejects an empty batch, an unknown/absent volume, a bad path (through
     * `storage::PathUtil`), a destination inside the source, and a move into
     * the directory the entry is already in (a no-op the UI also refuses).
     */
    static FileStatus validate(IFileOps& ops, const TransferRequest& req,
                               std::string* detail = nullptr);

    /**
     * @brief Top-level names of @ref TransferRequest::paths that are taken.
     *
     * Only the selected entries are looked at, not their contents — that is
     * what "ask once per entry" means, and it is also what keeps this answer
     * cheap enough to run inside an HTTP handler: N `stat()` calls, no walk.
     */
    static FileStatus conflicts(IFileOps& ops, const TransferRequest& req,
                               std::vector<std::string>& names,
                               std::string* detail = nullptr);

    /**
     * @brief Run the whole transfer: measure, then copy (and delete on `move`).
     *
     * Blocking; runs in the job's own task. @p report is updated in place and
     * ends up holding the result.
     */
    static void run(IFileOps& ops, const TransferRequest& req,
                    TransferReport& report, ITransferObserver& observer);
};

} // namespace files
} // namespace dhcp

#endif // DHCP_FILES_TRANSFERENGINE_H
