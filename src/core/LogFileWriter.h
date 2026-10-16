#ifndef DHCP_CORE_LOGFILEWRITER_H
#define DHCP_CORE_LOGFILEWRITER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "LogFileFormat.h"

namespace dhcp {
namespace core {

/**
 * @brief One log that lives as a file on the internal volume and is archived to
 *        the card when it is full (stages 191 and 203).
 *
 * The operator's policy is stated in @ref h-file LogFileFormat.h; what matters
 * here is that it is **one** policy. This class is its Template Method: @ref
 * append is the algorithm — make sure the folder is there, rotate when the
 * incoming line no longer fits, write, flush, count — and the two things that
 * differ between one log and another are hooks a subclass answers:
 *
 *   * @ref archiveFolder — where on the card the archives of *this* log go;
 *   * @ref logTitle — how *this* log names itself in the line a failed archive
 *     leaves in `Errors.log`.
 *
 * Everything else — the file's own name in an archive, the range folder, the
 * carried tail, the ten attempts, the flush per line — is decided once, here, so
 * that six logs cannot drift apart in rules about losing lines. Six of them use
 * this class today: the NTP request log, the DNS forwarded log, the error log, the
 * journal, the authorisation log and the DHCP log (`NtpLogWriter`,
 * `DnsForwardedLogWriter`, `ErrorsLogWriter`, `JobsLogWriter`, `AuthLogWriter`,
 * `DhcpLogWriter`), which is what made the split of stage 203 worth doing rather
 * than a shape invented for one caller.
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
 *   * **every line is flushed before the call returns**: the point of these files
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
 * `test/test_logfilewriter.cpp`.
 */
class LogFileWriter {
public:
    /** @brief Is the card there and usable right now? */
    using CardCheck = std::function<bool()>;
    /** @brief Where a failure is reported (the device writes `Errors.log`). */
    using Notice = std::function<void(const std::string&)>;

    /**
     * @brief The live file, the card to archive to, and the two questions.
     *
     * @param livePath     e.g. `/fat/logs/Ntp.log` (folder created on demand);
     *                     its own name is also the name the archive carries.
     * @param cardMountPoint e.g. `/sdcard`; empty when the board has no slot.
     * @param cardMounted  Asked at every rotation, not remembered: a card can be
     *                     inserted and removed while the device runs.
     * @param notice       Called once per failed archive attempt.
     * @param maxBytes     The cap. It is a parameter because the tests rotate a
     *                     few times without writing 64 KB six times over, and
     *                     because the operator chooses it in the web interface
     *                     (stage 204) — see @ref setMaxBytes.
     */
    LogFileWriter(std::string livePath, std::string cardMountPoint,
                  CardCheck cardMounted, Notice notice,
                  size_t maxBytes = kLogFileDefaultMaxBytes);

    virtual ~LogFileWriter() = default;

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
    bool append(const std::string& line, const LogStamp& stamp);

    /**
     * @brief The cap this writer rotates at, in bytes (stage 204).
     *
     * A smaller cap is not a reason to throw the current log away, but it is a
     * reason to archive a file that is **already over** the new one: waiting for
     * the next line would leave the file over its limit until a line arrives,
     * and on a device with nothing to log that is for ever. So the rotation
     * happens here, under the same rules as any other one - the archive is a copy
     * that is finished before the live file is touched, and with no card to
     * archive to the newest lines are carried into a fresh file.
     */
    void setMaxBytes(size_t bytes);
    size_t maxBytes() const { return maxBytes_; }

    /** @brief The live file's path — what a target over this writer describes. */
    const std::string& path() const { return livePath_; }

    /** @brief Lines in the live file right now. */
    uint64_t lines() const { return lines_; }
    /** @brief Archives written to the card so far. */
    uint64_t archives() const { return archives_; }
    /** @brief Rotations that had to carry the tail instead of archiving. */
    uint64_t carryOvers() const { return carryOvers_; }

protected:
    /**
     * @brief Folder of this log's archives on the card, e.g. `log/Archive/Ntp`.
     *
     * Relative to the mount point, and the only thing that tells two logs apart
     * on the operator's card.
     */
    virtual const char* archiveFolder() const = 0;

    /** @brief Name of this log in a failure line, e.g. `NTP log`. */
    virtual std::string logTitle() const = 0;

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
    void rememberStamp(const LogStamp& stamp);

    std::string livePath_;
    /** The live file's own name: what it is called inside an archive too. */
    std::string fileName_;
    std::string cardMount_;
    CardCheck cardMounted_;
    Notice notice_;
    size_t maxBytes_ = kLogFileDefaultMaxBytes;

    /** Failed archive attempts since the file became full. */
    size_t failedAttempts_ = 0;
    /** The first stamp in the live file whose clock was set (the range's start). */
    bool firstRealSeen_ = false;
    LogDate firstReal_;
    /** The newest stamp whose clock was set (the range's end). */
    LogDate lastReal_;
    uint64_t lines_ = 0;
    uint64_t archives_ = 0;
    uint64_t carryOvers_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_LOGFILEWRITER_H
