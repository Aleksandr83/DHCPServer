#ifndef DHCP_CORE_DEVICELOG_H
#define DHCP_CORE_DEVICELOG_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ArchiveFileLogTarget.h"
#include "DeviceLogWriter.h"
#include "ErrorLogCore.h"
#include "FreeRtosErrorQueue.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace dhcp {
namespace core {

/**
 * @brief The device events journal: `logs/Device.log` on a volume.
 *
 * What the box itself did, as opposed to what a service did (`SSH.log`,
 * `Auth.log`, `Ntp.log`) or what went wrong (`Errors.log`): the run started with
 * which version, the previous run ended with which reset cause, who asked for a
 * restart, when the settings were restored from a backup or erased entirely.
 *
 * The arrangement is `SshLog`'s, deliberately: producers format a sentence and
 * enqueue it, one low-priority task drains the queue and flushes every line, and
 * a sentence lost to a full queue is counted and reported with the next line — a
 * FAT write never happens on the task that asked for the restart. Only the file
 * switch exists: the boot lines are already on the UART, which is where a
 * developer reads them, so a terminal echo here would only double them.
 *
 * One switch and one size, on **Settings ▸ General** for now: the operator
 * asked for them there while the device page is still being decided, and moving
 * them later costs one card in the markup.
 */
class DeviceLog {
public:
    using CardMountedCheck = LogFileWriter::CardCheck;

    static DeviceLog& instance();

    /**
     * @brief Point the log at a volume and start the writing task.
     *
     * @param mountPoint     e.g. "/fat" → the file is `<mountPoint>/logs/Device.log`
     * @param cardMountPoint e.g. "/sdcard" → where a full log is archived.
     * @param cardMounted    asked at every rotation, not remembered.
     * @return false when the task could not be created (sentences keep queueing
     *         until the queue fills, then they are counted as drops).
     */
    bool start(const std::string& mountPoint = "/fat",
               const std::string& cardMountPoint = std::string(),
               CardMountedCheck cardMounted = CardMountedCheck());

    /// @brief Keep the sentences in the file. ON by default: a device journal
    /// that has to be switched on before a fault is no journal at all.
    void setLogFile(bool enabled) { logFile_ = enabled; }
    bool logFile() const { return logFile_; }

    /// @brief The cap the file rotates at (one of `LogFileSizes.h`'s three).
    void setFileLogSize(size_t bytes);
    size_t fileLogSize() const { return fileLogSize_; }

    /// @brief The file the sentences go to ("" before start()).
    const std::string& target() const;

    /// @brief Sentences lost so far (switched off is not a loss).
    uint32_t dropped() const;

    /// @brief `boot: DHCPServer 01.03.058.00.26.10.RU started`.
    void noteBoot(const std::string& version);

    /// @brief The same line, naming why the previous run ended.
    void noteBootAfter(const std::string& version, const std::string& reason);

    /// @brief `restart requested from the web interface`.
    void noteRestart(const std::string& source);

    /// @brief `factory reset requested from the console: …`.
    void noteFactoryReset(const std::string& source);

    /// @brief `settings imported from a backup file`.
    void noteSettingsImported();

    /// @brief `the previous run ended inside the SSH server: <stage>`.
    void noteSshCrash(const std::string& stage);

    /// @brief `libssh <function>: <message>` — the library's own words.
    void noteLibssh(const std::string& function, const std::string& message);

private:
    DeviceLog() = default;
    ~DeviceLog();

    DeviceLog(const DeviceLog&) = delete;
    DeviceLog& operator=(const DeviceLog&) = delete;

    static void taskEntry(void* arg);
    void run();

    /// @brief Format and enqueue (the switch is asked here, not by callers).
    bool submit(LogLevel level, const std::string& text);

    FreeRtosErrorQueue queue_;
    std::unique_ptr<ArchiveFileLogTarget> target_;
    std::unique_ptr<ErrorLogCore> core_;
    bool logFile_ = true;
    size_t fileLogSize_ = kLogFileDefaultMaxBytes;
    TaskHandle_t task_ = nullptr;
    uint32_t preStartDropped_ = 0;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_DEVICELOG_H
