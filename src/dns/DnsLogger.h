#ifndef DHCP_DNS_DNSLOGGER_H
#define DHCP_DNS_DNSLOGGER_H

#include <string>
#include <map>
#include <memory>
#include <vector>
#include <cstdint>

#include "DnsForwardedLogWriter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace dhcp {
namespace dns {

/**
 * @brief Where a DNS query was resolved from (logging category).
 */
enum class DnsLogSource {
    LOCAL,      // resolved from custom local hosts (user-assigned)
    CACHE,      // resolved from cache
    FORWARDED   // resolved via the external DNS server
};

/**
 * @brief DNS query logger.
 *
 * Logs DNS queries to:
 *   1. Terminal (ESP_LOGI) if enabled (and the source filter is on)
 *   2. External REST service (HTTP POST) if configured and enabled
 *   3. A file on the internal volume (stage 204): `<fat>/logs/Forwarded.log`,
 *      holding the answers that came from outside, archived to the card as
 *      `log/Archive/Dns/<first>-<last>/Forwarded.log` when it is full. Its own
 *      switch, its own `Forwarded` filter and its own size, because a file that
 *      is kept for a fortnight and a terminal that is watched for a minute are
 *      not the same reader.
 */
class DnsLogger {
public:
    DnsLogger();
    ~DnsLogger() = default;

    /**
     * @brief Enable/disable terminal logging.
     */
    void setLogTerminal(bool enabled);

    /**
     * @brief Enable/disable the per-category terminal filters.
     */
    void setLogForwarded(bool enabled);
    void setLogLocal(bool enabled);
    void setLogCache(bool enabled);

    /**
     * @brief Enable/disable the "sent to REST" terminal filter.
     *
     * When enabled, terminal logging only shows queries that were also
     * sent to the external REST log service.
     */
    void setLogRestSent(bool enabled);

    /**
     * @brief Get per-category filter state.
     */
    bool logForwarded() const { return logForwarded_; }
    bool logLocal() const { return logLocal_; }
    bool logCache() const { return logCache_; }
    bool logRestSent() const { return logRestSent_; }

    /**
     * @brief Enable/disable REST logging.
     */
    void setLogRest(bool enabled);

    /**
     * @brief Set the external REST logging URL.
     */
    void setLogUrl(const std::string& url);

    /**
     * @brief Set HTTP Basic auth credentials for REST logging.
     *
     * @param enabled Whether to send the Authorization header.
     * @param user    Basic auth username.
     * @param pass    Basic auth password.
     */
    void setLogAuth(bool enabled, const std::string& user,
                    const std::string& pass);

    bool logAuthEnabled() const { return logAuthEnabled_; }
    const std::string& logAuthUser() const { return logAuthUser_; }
    const std::string& logAuthPassword() const { return logAuthPassword_; }

    /**
     * @brief Provide the local hosts map (domain → IPs) so the REST URL
     *        hostname can be resolved against it before sending.
     *
     * The pointer must stay valid for the lifetime of the logger (it is
     * owned by the DNS server). Pass nullptr to disable resolution.
     */
    void setLocalHosts(const std::map<std::string, std::vector<std::string>>* hosts);

    /**
     * @brief Stop the async REST sender task.
     *
     * Wakes the sender (if it is blocked on the queue), waits for it to exit
     * and marks the REST sender as stopped. No-op if it is not running.
     */
    void stopRestSender();

    /**
     * @brief Log a DNS query.
     *
     * @param domain     The queried domain name.
     * @param type       DNS query type (1=A, 28=AAAA).
     * @param clientAddr Source IP in string form.
     * @param clientMac  Client MAC ("xx:xx:xx:xx:xx:xx") or empty if unknown.
     * @param source     Where the query was resolved from.
     * @param outcome    How it ended — see @ref DnsQueryOutcome; the text of the
     *                   line is decided there, because the difference between "the
     *                   name does not exist" and "we gave up waiting" is the whole
     *                   reason the enumeration exists
     * @param answer     The resolved IP (empty when there is no answer).
     */
    void logQuery(const std::string& domain, uint16_t type,
                  const std::string& clientAddr,
                  const std::string& clientMac,
                  DnsLogSource source,
                  DnsQueryOutcome outcome, const std::string& answer);

    /**
     * @brief Enable/disable the file log of forwarded answers (stage 204).
     *
     * Starts or stops the writing task; the file itself appears on the first line
     * that is written, so switching this on with nothing to log leaves no empty
     * file behind.
     */
    void setLogFile(bool enabled);
    bool logFile() const { return logFile_; }

    /**
     * @brief Which answers the **file** keeps: the forwarded ones, or none.
     *
     * Deliberately its own switch and not the terminal's `Forwarded` filter: the
     * operator may want the terminal quiet during an investigation while the file
     * keeps the record, and one switch for two destinations would make turning one
     * off silently stop the other. Today the file holds forwarded answers only,
     * which is what he asked for: a name this device resolved itself is in the
     * local-hosts list, not in a file meant to show what came back from outside.
     */
    void setLogFileForwarded(bool enabled);
    bool logFileForwarded() const { return logFileForwarded_; }

    /**
     * @brief The cap the file rotates at, in bytes (stage 204).
     *
     * The three sizes the page offers, no free number (see
     * `core/LogFileSizes.h`); applied to a writer that is already running.
     */
    void setFileLogSize(size_t bytes);
    size_t fileLogSize() const { return fileLogSize_; }

    /** @brief Is the card mounted right now? Asked at every rotation. */
    using CardMountedCheck = core::LogFileWriter::CardCheck;

    /**
     * @brief Tell the logger where the file log lives (stage 204).
     *
     * The paths are known where the volumes are — `main.cpp` — and not here: this
     * class writes where it is told to. Called once at boot, before the settings
     * are applied, so the order of the two does not matter.
     *
     * @param fatMountPoint  e.g. `/fat` → the live file is
     *                       `<it>/logs/Forwarded.log`.
     * @param cardMountPoint e.g. `/sdcard` → where a full file is archived; ""
     *                       when the board has no card slot at all.
     * @param cardMounted    Asked at every rotation, not remembered: a card can
     *                       be inserted and removed while the device runs.
     */
    void setFileLogVolumes(const std::string& fatMountPoint,
                           const std::string& cardMountPoint,
                           CardMountedCheck cardMounted);

private:
    void logToTerminal(const std::string& domain, uint16_t type,
                       const std::string& clientAddr,
                       const std::string& clientMac,
                       DnsLogSource source,
                       DnsQueryOutcome outcome, const std::string& answer);
    void logToRest(const std::string& domain, uint16_t type,
                   const std::string& clientAddr,
                   const std::string& clientMac,
                   DnsLogSource source,
                   DnsQueryOutcome outcome, const std::string& answer);
    void logToFile(const std::string& domain, uint16_t type,
                   const std::string& clientAddr,
                   const std::string& clientMac,
                   DnsLogSource source,
                   DnsQueryOutcome outcome, const std::string& answer);

    // ─── Async REST logging (fire-and-forget) ──────
    // Log records go into a bounded ring buffer (FreeRTOS queue); on overflow
    // the oldest record is overwritten. A dedicated sender task drains the
    // buffer and POSTs each record with a timeout, so a slow REST server
    // never blocks the DNS task.
    // Rule 39: the record the page sees is built from these buffers.
    static constexpr size_t kDomainBytes = 128;
    static constexpr size_t kClientTextBytes = 48;
    static constexpr size_t kMacTextBytes = 32;
    static constexpr size_t kAnswerTextBytes = 64;

    struct RestLogRecord {
        uint32_t ts = 0;      // uptime ms (diagnostics)
        uint16_t type = 0;    // DNS query type (1=A, 28=AAAA)
        // How the query ended (`DnsQueryOutcome`), not a boolean: the file line
        // and the terminal sentence have to say *why* there is no answer, and the
        // record is what carries the reason from the query path to the writer
        // (stage 216). The REST body still sends `resolved`, derived from it.
        uint8_t outcome = 0;
        bool stop = false;    // internal: stop marker for the sender task
        uint8_t source = 0;   // 0=local, 1=cache, 2=forwarded
        char domain[kDomainBytes] = {0};
        char client[kClientTextBytes] = {0};
        char mac[kMacTextBytes] = {0};   // client MAC ("xx:xx:xx:xx:xx:xx", empty if unknown)
        char answer[kAnswerTextBytes] = {0};
    };
    static constexpr int kRestQueueDepth = 48;     // ring buffer capacity
    static constexpr int kRestSenderStack = 8192;  // sender task stack (TLS)
    static constexpr int kRestSenderPriority = 3;

    void updateRestSenderState();
    void ensureRestSender();
    static void restSenderTask(void* arg);
    std::string resolveUrlHost(const std::string& url) const;
    void sendRestLog(const RestLogRecord& rec);
    std::string buildRestJson(const RestLogRecord& rec) const;

    // ─── Async file logging (fire-and-forget, stage 204) ───────
    // The same arrangement as the REST half: one record goes into a bounded queue
    // from the DNS task and one task writes it, so a FAT write - or the archive
    // that copies a full file to the card - never blocks a DNS answer.
    //
    // Rule 39/DRY: the **same** record type serves both destinations, because both
    // need the same fields and a second struct with the same seven members is a
    // copy that can drift. `RestLogRecord` is a fixed-size record, which is what
    // makes it safe to hand through a queue.
    void updateFileLogState();
    void ensureFileWriter();
    void stopFileWriter();
    static void fileLogTask(void* arg);
    void writeFileLog(const RestLogRecord& rec);

    bool logTerminal_ = false;
    bool logForwarded_ = true;
    bool logLocal_ = true;
    bool logCache_ = true;
    bool logRestSent_ = false;
    bool logRest_ = false;
    std::string logUrl_;
    bool logAuthEnabled_ = false;
    std::string logAuthUser_;
    std::string logAuthPassword_;

    // File logging: off until the setting says otherwise, and mute until the
    // volumes are known (see setFileLogVolumes).
    bool logFile_ = false;
    bool logFileForwarded_ = true;
    bool fileVolumesReady_ = false;
    std::string fatMountPoint_;
    std::string cardMountPoint_;
    CardMountedCheck cardMounted_;
    size_t fileLogSize_ = core::kLogFileDefaultMaxBytes;
    std::unique_ptr<DnsForwardedLogWriter> fileWriter_;
    uint32_t writeFailures_ = 0;
    uint32_t fileDropped_ = 0;

    QueueHandle_t restQueue_ = nullptr;
    TaskHandle_t restTask_ = nullptr;
    volatile bool restStopRequested_ = false;
    uint32_t restDropped_ = 0;   // count of records dropped on overflow
    const std::map<std::string, std::vector<std::string>>* localHosts_ = nullptr;

    QueueHandle_t fileQueue_ = nullptr;
    TaskHandle_t fileTask_ = nullptr;
    volatile bool fileStopRequested_ = false;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_DNSLOGGER_H
