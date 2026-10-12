#ifndef DHCP_TIME_NTPPROBE_H
#define DHCP_TIME_NTPPROBE_H

#include "NtpProbeFormat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dhcp {
namespace time {

/**
 * @brief Asks the configured NTP servers who answers, and who does not (stage 195).
 *
 * The SNTP client is what synchronises the clock; this class only finds out what
 * the client is up against, because the client cannot be asked: its notification
 * brings the time, not the name of the server that sent it. Each server is asked
 * with a request of the device's own (see @ref ntpProbeFormat.h), in order, and
 * the run stops at the first one that answers - which is exactly the sentence the
 * journal then writes: this server said nothing, that one replied.
 *
 * Blocking: the resolver and the socket waits both stop the calling task, so a
 * run belongs on a task of its own and never in an `esp_timer` callback or the
 * lwIP thread. One run costs at most one resolution plus @ref kNtpProbeTimeoutMs
 * per server - it is written for the minutes when the device is already failing,
 * not for every poll.
 */
class NtpProbe {
public:
    /**
     * @param timeoutMs How long one server is given to answer, in milliseconds.
     */
    explicit NtpProbe(uint32_t timeoutMs = kNtpProbeTimeoutMs) : timeoutMs_(timeoutMs) {}

    /**
     * @brief Ask the servers in turn, in the given order, and report each one.
     *
     * The returned vector has one entry per configured server, in order, so the
     * line shows the whole list even when the run stopped early - the servers
     * after the answer are marked @ref NtpProbeVerdict::NotAsked rather than
     * being left out, or the reader would think they were never configured.
     */
    std::vector<NtpProbeEntry> run(const std::vector<std::string>& servers) const;

private:
    /** @brief Resolve one name to an address; false when it does not resolve. */
    bool resolve(const std::string& name, std::string& ipOut) const;

    /** @brief Ask one address; on a reply writes its stratum and returns true. */
    bool ask(const std::string& ip, uint8_t& stratumOut) const;

    /// How long one server has to answer.
    uint32_t timeoutMs_;
};

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_NTPPROBE_H
