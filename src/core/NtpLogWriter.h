#ifndef DHCP_CORE_NTPLOGWRITER_H
#define DHCP_CORE_NTPLOGWRITER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "NtpLogFormat.h"

namespace dhcp {
namespace core {

/**
 * @brief The NTP request log as a file: appending, capping, archiving (stage 191).
 *
 * The policy is the operator's, and it is deliberately not the `.1` rename the
 * other logs use — a full file is *sent* somewhere and the live one starts again
 * (see @ref h-file NtpLogFormat.h for the rules and their reasons).
 *
 * Three properties are worth stating out loud, because each one is a decision
 * about losing something:
 *
 *   * **the archive is a copy that is finished before the live file is
 *     touched** — a half-copied archive with an emptied live file would be a log
 *     that exists nowhere;
 *   * **a failed archive keeps the file** — it is the only copy of those lines,
 *     and the operator preferred a growing file to a lost one, with the failure
 *     reported (once per attempt) through @ref Notice;
 *   * **every line is flushed before the call returns**: the point of this file
 *     is to describe what happened before a restart, and a buffered line is
 *     exactly what a restart loses.
 *
 * Nothing is created blindly. The live file's folder is created **once**, and
 * only inside a parent that already exists: the mount point belongs to the
 * storage layer, and inventing a directory under that name would hide a volume
 * that never mounted behind a log that looks healthy.
 *
 * Free of ESP-IDF on purpose — the card is a callback and the reported failure is
 * a callback, so the whole policy runs on a host — and exercised there by
 * `test/test_ntplogwriter.cpp`.
 */
class NtpLogWriter {
public:
    /** @brief Is the card there and usable right now? */
    using CardCheck = std::function<bool()>;
    /** @brief Where a failure is reported (the device writes `Errors.log`). */
    using Notice = std::function<void(const std::string&)>;

    /**
     * @brief The live file, the card to archive to, and the two questions.
     *
     * @param livePath     e.g. `/fat/logs/Ntp.log` (folder created on demand).
     * @param cardMountPoint e.g. `/sdcard`; empty when the board has no slot.
     * @param cardMounted  Asked at every rotation, not remembered: a card can be
     *                     inserted and removed while the device runs.
     * @param notice       Called once per failed archive attempt.
     * @param maxBytes     The cap; a parameter because the tests rotate a few
     *                     times without writing 64 KB six times over.
     */
    NtpLogWriter(std::string livePath, std::string cardMountPoint,
                 CardCheck cardMounted, Notice notice,
                 size_t maxBytes = kNtpLogMaxBytes);

    /**
     * @brief Append one line (no newline in @p line), rotating first if it no
     *        longer fits.
     *
     * The rotation decision is made against the stamps of what is already in the
     * file, so the range of an archive is the range of the lines inside it, and
     * not of the line that happened to overflow it.
     *
     * @return false when the line could not be written at all.
     */
    bool append(const std::string& line, const NtpLogStamp& stamp);

    /** @brief Lines in the live file right now. */
    uint64_t lines() const { return lines_; }
    /** @brief Archives written to the card so far. */
    uint64_t archives() const { return archives_; }
    /** @brief Rotations that had to carry the tail instead of archiving. */
    uint64_t carryOvers() const { return carryOvers_; }

private:
    /** @brief Rotate when the incoming line would take the file over the cap. */
    void rotateIfNeeded(size_t incomingBytes);
    /** @brief Copy the live file into the range's folder; false if anything failed. */
    bool archiveToCard(const std::string& rangeName);
    /** @brief Start the live file again with @p content (empty = a fresh file). */
    void startFresh(const std::string& content);
    /** @brief Create the live file's folder, one level inside its parent. */
    bool ensureLiveDirectory();
    /** @brief Size of the live file (0 when it is not there). */
    size_t liveSize() const;
    /** @brief Read the live file whole (the cap keeps it small enough). */
    std::string readLive() const;
    /** @brief Remember a stamp: the first real one names the range. */
    void rememberStamp(const NtpLogStamp& stamp);

    std::string livePath_;
    std::string cardMount_;
    CardCheck cardMounted_;
    Notice notice_;
    size_t maxBytes_ = kNtpLogMaxBytes;

    /** Failed archive attempts since the file became full. */
    size_t failedAttempts_ = 0;
    /** The first stamp in the live file whose clock was set (the range's start). */
    bool firstRealSeen_ = false;
    NtpLogDate firstReal_;
    /** The newest stamp whose clock was set (the range's end). */
    NtpLogDate lastReal_;
    uint64_t lines_ = 0;
    uint64_t archives_ = 0;
    uint64_t carryOvers_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_NTPLOGWRITER_H
