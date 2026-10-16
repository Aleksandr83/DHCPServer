#include "DhcpFileLog.h"

#include <ctime>

#include "../core/ErrorLog.h"
#include "../core/LogFileSizes.h"

#include "esp_log.h"
#include "esp_timer.h"

using namespace std;

namespace dhcp {
namespace dhcp {

namespace {

constexpr const char* kTag = "DhcpFileLog";
/** Priority below the servers and below the web server: nobody waits for a log. */
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
/** Large enough for the stdio path plus the FATFS/VFS calls underneath it. */
constexpr uint32_t kTaskStack = 4096;
/** How long the writing task waits in the queue when there is nothing to write. */
constexpr uint32_t kWaitMs = 1000;
/** The tag every line carries: `[2026-10-16 21:30:00] dhcp: …`. */
constexpr const char* kLineTag = "dhcp";

/** @brief Seconds since boot, used when the clock is not set yet. */
uint32_t uptimeSec()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

/**
 * @brief The shape of a line: the stamp in brackets, then `dhcp:`, then the words.
 *
 * The same shape the authorisation log and the journal use (`ErrorLogCore`'s
 * formatter, supplied by the owner), so a reader of these files does not have two
 * formats to learn.
 */
string dhcpLine(core::LogLevel, const char* tag, const string& message)
{
    return "[" + core::ErrorLogCore::formatStamp(
                      static_cast<uint64_t>(time(nullptr)), uptimeSec()) +
           "] " + (tag ? tag : kLineTag) + ": " +
           core::ErrorLogCore::clampMessage(message);
}

} // namespace

bool DhcpFileLog::start(const string& mountPoint, const string& cardMountPoint,
                        CardMountedCheck cardMounted)
{
    if (task_ != nullptr) return true;   // already started

    string dir = mountPoint;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    // The file is a `LogFileWriter` (through the adapter), so a full DHCP log is
    // copied to the card like the other file logs. A failed archive is reported
    // into `Errors.log` — the file the operator reads when something is wrong —
    // and not into this one, which is the log of what the server did, not of
    // faults.
    auto writer = make_unique<DhcpLogWriter>(
        dir + "/logs/Dhcp.log", cardMountPoint, move(cardMounted),
        [](const string& message) {
            core::ErrorLog::instance().errorf(kTag, "%s", message.c_str());
        },
        fileLogSize_);
    target_ = make_unique<core::ArchiveFileLogTarget>(move(writer));
    core_ = make_unique<core::ErrorLogCore>(queue_, *target_, &uptimeSec, dhcpLine);

    if (!queue_.ready()) {
        ESP_LOGE(kTag, "no queue — the DHCP log cannot be started");
        return false;
    }

    const BaseType_t res = xTaskCreate(taskEntry, "dhcp_log", kTaskStack, this,
                                       kTaskPriority, &task_);
    if (res != pdTRUE) {
        task_ = nullptr;
        ESP_LOGE(kTag, "failed to create the DHCP log task");
        return false;
    }
    ESP_LOGI(kTag, "DHCP log started at %s", target_->description().c_str());
    return true;
}

void DhcpFileLog::taskEntry(void* arg)
{
    static_cast<DhcpFileLog*>(arg)->run();
    vTaskDelete(nullptr);
}

void DhcpFileLog::run()
{
    for (;;) {
        // Waits in the queue, writes what is there, goes back to waiting. The
        // target flushes every line, so a restart cannot lose one.
        core_->drain(kWaitMs);
    }
}

void DhcpFileLog::setFileLogSize(size_t bytes)
{
    fileLogSize_ = bytes;
    if (target_) target_->writer().setMaxBytes(bytes);
}

const string& DhcpFileLog::target() const
{
    static const string empty;
    return target_ ? target_->description() : empty;
}

uint32_t DhcpFileLog::dropped() const
{
    return preStartDropped_ + (core_ ? core_->dropped() : 0);
}

void DhcpFileLog::note(core::LogLevel level, const string& text)
{
    // The switch is asked here and not by the caller: one place decides whether
    // this log keeps anything, and the DHCP task never has to know. A line that
    // arrives while the log is off is not a loss — it was not meant to be written.
    if (!logFile_) return;
    if (!core_) { ++preStartDropped_; return; }

    core_->submit(level, kLineTag, text);
}

} // namespace dhcp
} // namespace dhcp
