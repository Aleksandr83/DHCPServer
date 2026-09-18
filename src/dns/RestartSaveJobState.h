#ifndef DHCP_DNS_RESTARTSAVEJOBSTATE_H
#define DHCP_DNS_RESTARTSAVEJOBSTATE_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace dns {

/**
 * @brief State of one "write a file before the restart" job.
 *
 * Two jobs have this shape — Statistica.dat and cache.dat — and the same page
 * reads both, which is why the small state machine lives in one place. What it
 * has to get right is exactly what that page depends on:
 *
 *   * **single-flight** — a second request while one runs must not start a
 *     second writer on the same file. It reports `Busy`, and the caller waits
 *     for the running job instead of starting its own.
 *   * **a verdict, not a boolean** — "busy went false" cannot tell a written
 *     file from a failed write, nor either of them from "there was nothing to
 *     write". The page has to say which of those happened, so the verdict is an
 *     enum and it survives until the next job overwrites it.
 *   * **no inherited luck** — a new job clears the previous verdict the moment
 *     it starts, so a page polling during the new run never reads the outcome of
 *     the one before it.
 *
 * Deliberately free of ESP-IDF so those three rules can be tested on the host:
 * every bug this project had in this area (a verdict that lied, a step that
 * stayed silent) lived in this kind of logic, not in the file I/O.
 *
 * Not thread-safe by itself — the owner guards it with its own mutex, one per
 * job (DnsServer does; the two jobs never share a task, a mutex or a verdict).
 */
class RestartSaveJobState {
public:
    /** What a finished job did. */
    enum class Verdict {
        None,     // nothing has finished yet — no verdict to report
        Ok,       // the file is on the card
        Skipped,  // the operator has the switch off; nothing was written
        Failed,
    };

    /** What a request to start decided. */
    enum class StartResult {
        Started,  // a job is running now (busy() is true)
        Busy,     // one was already running; the caller waits for that one
        Skipped,  // the switch is off — no job, and none is needed
        Failed,   // the owner could not start it (e.g. no task could be created)
    };

    /**
     * @brief Ask to start a job.
     *
     * @param enabled false when the operator's switch for this file is off.
     * @return Started when a job now runs, Busy when one already did, Skipped
     * when the switch is off, Failed when the owner must refuse to start.
     */
    StartResult request(bool enabled);

    /**
     * @brief The job ended with @p verdict: stop being busy and remember it.
     *
     * A finished job always has its say, even when something else (a switch
     * turned off while it ran, another request) touched the state in between:
     * the file is either on the card or it is not, and only the job knows.
     *
     * @param detail why it failed, in the device's own words ("cannot publish
     *        the file"). It exists because a page that can only say "failed"
     *        sends the operator to a serial console he may not have.
     */
    void finish(Verdict verdict, const std::string& detail = "");

    /**
     * @brief The owner accepted a start and then could not create the task.
     * The file was not written, so that is the verdict.
     */
    void abortStart();

    bool busy() const { return busy_; }
    Verdict verdict() const { return verdict_; }
    const std::string& detail() const { return detail_; }

private:
    bool        busy_ = false;
    Verdict     verdict_ = Verdict::None;
    std::string detail_;
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_RESTARTSAVEJOBSTATE_H
