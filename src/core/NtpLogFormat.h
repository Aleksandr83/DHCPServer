#ifndef DHCP_CORE_NTPLOGFORMAT_H
#define DHCP_CORE_NTPLOGFORMAT_H

#include <cstdint>
#include <string>

#include "LogFileFormat.h"

namespace dhcp {
namespace core {

/**
 * @file NtpLogFormat.h
 * @brief What the NTP request log says, and where it lives (stages 191–193).
 *
 * The log is one growing file on the internal volume, `/fat/logs/Ntp.log`, whose
 * rotations go to the card as `log/Archive/Ntp/<first>-<last>/Ntp.log`. The
 * shape of that policy — the cap, the archive folder chain, the carried tail —
 * is shared with the DNS forwarded log and is written down in
 * @ref h-file LogFileFormat.h and applied by `LogFileWriter`; what is left here
 * is the part that is about *NTP*: the names of the NTP log's own files, and the
 * sentences the device writes into it when nobody is answering, when the clock
 * cannot be trusted, and when a synchronisation finally happens.
 *
 * Every rule in this header is a promise about what the operator will read on a
 * device he cannot see, and each one is host-tested in
 * `test/test_ntplogformat.cpp`.
 */

/** @brief Name of the live log inside its folder on the internal volume. */
constexpr const char* kNtpLogFileName = "Ntp.log";

/** @brief Folder of the archives on the card, relative to its mount point. */
constexpr const char* kNtpLogArchiveFolder = "log/Archive/Ntp";

/**
 * @brief What a line says when the reply admitted the clock was not verified.
 *
 * LI = 3 / stratum 16 is what the client receives in that case, and this mark is
 * what the *log* adds to it: a file whose whole purpose is to explain a device
 * that will not synchronise must not be silent about the requests it did answer.
 */
constexpr const char* kNtpLogNoClockMark = " (clock not verified)";

/**
 * @brief What the state line says about resolving the NTP server's own name.
 *
 * `Failed` is the one that matters: the device asks its own DNS server (see
 * `EthManager`), which forwards to the configured upstream — so a wrong or
 * unreachable upstream means the NTP client never gets an address, and "no
 * confirmed reply" follows from that alone.
 */
enum class NtpDnsVerdict {
    Ok,       ///< the name resolved; the address follows
    Failed,   ///< it did not resolve
    Pending,  ///< the answer had not arrived by the time the line was due
};

/** @brief `DNS -> 5.61.32.14`, `DNS lookup failed`, `DNS lookup still pending`. */
inline std::string ntpDnsVerdictText(NtpDnsVerdict verdict,
                                     const std::string& resolvedIp = std::string())
{
    switch (verdict) {
        case NtpDnsVerdict::Ok:     return "DNS -> " + resolvedIp;
        case NtpDnsVerdict::Failed: return "DNS lookup failed";
        case NtpDnsVerdict::Pending: break;
    }
    return "DNS lookup still pending";
}

/**
 * @brief The resolver chain, said out loud when the client starts (stage 193).
 *
 * The device resolves through itself — `EthManager` points the system resolver at
 * its own address so that `.lo` names work — and the built-in DNS server forwards
 * external names to the upstream below. That makes the upstream a single point of
 * failure for the device's own name resolution, and this line is what turns a
 * silent "no confirmed reply" into something a reader can check.
 *
 * @param deviceIp    The device's own address (the resolver it asks).
 * @param upstreamDns What that server forwards to (`DnsConfig::externalDns`).
 * @param gateway     The route out of the LAN, i.e. what makes the request leave.
 */
inline std::string ntpResolverChainText(const std::string& deviceIp,
                                        const std::string& upstreamDns,
                                        const std::string& gateway)
{
    const std::string device = deviceIp.empty() ? std::string("not set") : deviceIp;
    const std::string upstream = upstreamDns.empty() ? std::string("not set") : upstreamDns;
    const std::string route = gateway.empty() ? std::string("not set") : gateway;
    return "resolver: this device (" + device + "), upstream " + upstream +
           ", gateway " + route;
}

/**
 * @brief The periodic "still not synchronised" line, with the DNS verdict on it.
 *
 * One line has to carry the whole situation, because the reader has no console
 * and one line is what he gets: how long the device has been waiting, which
 * server it is waiting for, and whether it can even resolve that server's name.
 */
inline std::string ntpSyncStateText(uint32_t waitedSec, const std::string& server,
                                    const std::string& dnsVerdict)
{
    return "still not synchronised: " + std::to_string(waitedSec) +
           " s since the client started, server=" + server +
           ", no confirmed reply yet (" + dnsVerdict + ")";
}

/**
 * @brief The wait before the next "still not synchronised" line (stage 192).
 *
 * The operator's schedule: at most three lines a minute apart — a synchronisation
 * that will happen usually happens in seconds, and a minute is enough to see that
 * it did not — then every 2, 5 and 10 minutes, and 10 minutes from then on. The
 * point of the growth is the filesystem: at a line a minute a device that can
 * never synchronise would write 1440 lines a day into the log that is meant to
 * describe the problem, and every one of them would say the same thing.
 *
 * @param index How many such lines have already been written since the client
 *              started (0 for the first one).
 */
inline uint32_t ntpSyncStateDelaySec(std::size_t index)
{
    // Rule 39: the steps of the schedule, named where they are used.
    constexpr uint32_t kQuickSec = 60;      // the first three lines
    constexpr std::size_t kQuickCount = 3;
    constexpr uint32_t kSlowSec = 120;
    constexpr uint32_t kSlowerSec = 300;
    constexpr uint32_t kSlowestSec = 600;   // and the ceiling, for as long as it takes

    if (index < kQuickCount) return kQuickSec;
    if (index == kQuickCount) return kSlowSec;
    if (index == kQuickCount + 1) return kSlowerSec;
    return kSlowestSec;
}

/**
 * @brief What the success line says about how far the clock was off.
 *
 * The device cannot ask the server what the offset was — ESP-IDF's SNTP sets the
 * clock before telling us — so this is an **estimate** from the last reading of
 * the clock that the writer took: what the clock said then, advanced by the time
 * that has passed since, against what it says now. It is honest about its own
 * limits: with no real time in the earlier reading (a device that never had a
 * clock) there is nothing to compare with, and the line says that instead of
 * printing a shift of half a century.
 *
 * @param prevUtcSec    The earlier reading, in UTC epoch seconds.
 * @param prevUptimeSec Uptime seconds at that reading.
 * @param nowUtcSec     The clock as it is now (after the synchronisation).
 * @param nowUptimeSec  Uptime seconds now.
 * @param prevClockSet  False when that earlier reading was not a real date.
 */
inline std::string ntpLogClockShiftText(int64_t prevUtcSec, uint32_t prevUptimeSec,
                                        int64_t nowUtcSec, uint32_t nowUptimeSec,
                                        bool prevClockSet)
{
    if (!prevClockSet) return "the clock had no real time yet";

    const int64_t elapsed = static_cast<int64_t>(nowUptimeSec) -
                            static_cast<int64_t>(prevUptimeSec);
    const int64_t shift = nowUtcSec - (prevUtcSec + elapsed);
    if (shift > 0) return "the clock was " + std::to_string(shift) + " s behind";
    if (shift < 0) return "the clock was " + std::to_string(-shift) + " s ahead";
    return "the clock was already right";
}

/**
 * @brief One event line of the log (the client starting, a synchronisation…).
 *
 * Same shape as a request line — stamp, then the sentence — so that a file whose
 * reader is looking for "what happened at 20:14" does not have two formats to
 * parse.
 */
inline std::string ntpLogEventLine(const LogStamp& stamp, const std::string& text)
{
    return logStampPrefix(stamp) + " " + text;
}

/**
 * @brief One line of the NTP request log.
 *
 * The whole line is built here, so the shape a reader sees on the card is decided
 * in one place that is tested: `[2026.10.12 19:22:14] NTP request from 192.168.1.5
 * -> stratum 3`.
 *
 * @param clockVerified False for a request the server answered with "the clock is
 *        not trustworthy" (LI = 3, stratum 16). Those lines are the interesting
 *        ones when the device cannot synchronise — they are the proof that a
 *        client is asking at all — so they are written with @ref kNtpLogNoClockMark
 *        rather than dropped. The stratum passed in is the one the reply carried,
 *        so the line never claims a stratum the client never saw.
 */
inline std::string ntpLogLine(const LogStamp& stamp, const std::string& client,
                              unsigned stratum, bool clockVerified = true)
{
    return logStampPrefix(stamp) + " NTP request from " + client +
           " -> stratum " + std::to_string(stratum) +
           (clockVerified ? std::string() : std::string(kNtpLogNoClockMark));
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_NTPLOGFORMAT_H
