#ifndef DHCP_TIME_NTPPROBEFORMAT_H
#define DHCP_TIME_NTPPROBEFORMAT_H

#include "time/NtpMessage.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dhcp {
namespace time {

/**
 * @brief What the probe asks and what the journal writes about it (stage 195).
 *
 * The SNTP client itself cannot say which of the configured servers answered:
 * the notification callback brings the time and nothing else, and which server
 * lwIP is asking at that moment is its own private state. So the device asks the
 * question itself, with a request of its own, and this header is the pure half of
 * that: the 48 bytes sent, the rule that decides whether a datagram is the answer
 * to them, and the one line the operator reads.
 *
 * A reply is only accepted when it echoes the transmit stamp of *our* request.
 * Without that check any datagram arriving on the socket - a stray answer to
 * somebody else's poll, or a client request that happened to be sent to the same
 * port - would be reported as "this server answers", which is exactly the claim
 * the line is read for.
 */

/// How long one server is given to answer before it counts as silent.
///
/// Three seconds is not the SNTP client's own patience - lwIP waits 15 s for a
/// reply - because this is a diagnostic that runs while the client is already
/// failing: a server that needs longer than this has not answered the question
/// the operator is asking.
constexpr uint32_t kNtpProbeTimeoutMs = 3000;

/// Milliseconds in a second: the wait is configured in ms and named in the log in s.
constexpr uint32_t kNtpProbeMsPerSec = 1000;

/// The protocol version of the request the probe sends.
constexpr uint8_t kNtpProbeVersion = 4;

/// How an entry of a probe run ended.
enum class NtpProbeVerdict {
    Replied,     ///< a valid NTP reply came back
    NoReply,     ///< the address answered nothing within the timeout
    Unresolved,  ///< the name did not resolve at all, so nothing could be asked
    NotAsked,    ///< an earlier server answered, so this one was left alone
};

/// One server of a run, as the journal names it.
struct NtpProbeEntry {
    std::string name;                              ///< the configured server
    std::string ip;                                ///< what it resolved to, "" if it did not
    NtpProbeVerdict verdict = NtpProbeVerdict::NotAsked;
    uint8_t stratum = 0;                           ///< of a reply (0 = kiss-of-death)
};

/// Prefix and separator of the single line a run writes.
constexpr const char* kNtpProbeLinePrefix = "NTP probe: ";
constexpr const char* kNtpProbeEntrySeparator = "; ";

/**
 * @brief Build the request: 48 bytes, mode 3, version 4, one transmit stamp.
 *
 * The stamp is the only field that carries information - the server echoes it -
 * so the rest of the packet is zero, which is also what makes it obvious in a
 * packet capture that this came from the probe and not from the client.
 */
inline void ntpProbeBuildRequest(uint8_t* out, size_t len,
                                 uint32_t unixSec, uint32_t unixUsec)
{
    if (out == nullptr || len < kNtpPacketBytes) return;

    NtpPacket request;
    memset(&request, 0, sizeof(request));
    request.liVnMode = ntpMakeLiVnMode(kNtpLeapNoWarning, kNtpProbeVersion, kNtpModeClient);

    uint32_t ntpSec = 0;
    uint32_t ntpFrac = 0;
    unixToNtp(unixSec, unixUsec, ntpSec, ntpFrac);
    request.txTsSec = toBe32(ntpSec);
    request.txTsFrac = toBe32(ntpFrac);

    memcpy(out, &request, sizeof(request));
}

/**
 * @brief Is this datagram the answer to that request?
 *
 * @param reply      Received bytes.
 * @param replyLen   How many of them arrived.
 * @param request    The bytes that were sent (@ref ntpProbeBuildRequest).
 * @param requestLen Their count.
 * @param stratumOut The stratum of an accepted reply, when the caller wants it:
 *                   0 means kiss-of-death, i.e. the server answered "go away",
 *                   which is still an answer and is named as such in the log.
 */
inline bool ntpProbeIsReply(const uint8_t* reply, size_t replyLen,
                            const uint8_t* request, size_t requestLen,
                            uint8_t* stratumOut = nullptr)
{
    if (reply == nullptr || request == nullptr) return false;
    if (replyLen < kNtpPacketBytes || requestLen < kNtpPacketBytes) return false;

    NtpPacket answer;
    memcpy(&answer, reply, sizeof(answer));
    if (ntpMode(answer) != kNtpModeServer) return false;
    if (ntpVersion(answer) < kNtpVersionMin) return false;

    NtpPacket sent;
    memcpy(&sent, request, sizeof(sent));
    // The originate stamp of a reply is the client's transmit stamp. It is the
    // one field that ties this datagram to our request, and the whole reason
    // this check exists rather than trusting whatever arrives.
    if (answer.origTsSec != sent.txTsSec || answer.origTsFrac != sent.txTsFrac) {
        return false;
    }

    if (stratumOut != nullptr) *stratumOut = answer.stratum;
    return true;
}

/**
 * @brief The machine-readable name of a verdict.
 *
 * The journal writes a sentence (`ntpProbeEntryText`); the REST test endpoint
 * hands these names to the page, which words them itself in the operator's
 * language. Keeping the two apart is what lets the page translate "no reply"
 * without parsing a sentence.
 */
inline const char* ntpProbeVerdictName(NtpProbeVerdict verdict)
{
    switch (verdict) {
        case NtpProbeVerdict::Replied:    return "replied";
        case NtpProbeVerdict::NoReply:    return "no_reply";
        case NtpProbeVerdict::Unresolved: return "unresolved";
        case NtpProbeVerdict::NotAsked:   break;
    }
    return "not_asked";
}

/** @brief One entry of the run, as the journal names it. */
inline std::string ntpProbeEntryText(const NtpProbeEntry& entry, uint32_t timeoutMs)
{
    // A name that never resolved has no address to show, and a server that was
    // not asked has no result - so those two are named before the address and
    // the wait, which would both be wrong for them.
    if (entry.verdict == NtpProbeVerdict::Unresolved) {
        return entry.name + " - name did not resolve";
    }
    if (entry.verdict == NtpProbeVerdict::NotAsked) {
        return entry.name + " - not asked (an earlier server replied)";
    }

    std::string out = entry.name;
    if (!entry.ip.empty()) out += " (" + entry.ip + ")";

    if (entry.verdict != NtpProbeVerdict::Replied) {
        return out + " - no reply in " + std::to_string(timeoutMs / kNtpProbeMsPerSec) + " s";
    }
    if (entry.stratum == kNtpStratumKissOfDeath) {
        return out + " - replied (stratum 0, kiss-of-death)";
    }
    return out + " - replied (stratum " + std::to_string(entry.stratum) + ")";
}

/** @brief The whole run on one line, in the order the servers were asked. */
inline std::string ntpProbeLine(const std::vector<NtpProbeEntry>& entries,
                                uint32_t timeoutMs)
{
    std::string out = kNtpProbeLinePrefix;
    bool first = true;
    for (const NtpProbeEntry& entry : entries) {
        if (!first) out += kNtpProbeEntrySeparator;
        out += ntpProbeEntryText(entry, timeoutMs);
        first = false;
    }
    return out;
}

} // namespace time
} // namespace dhcp

#endif // DHCP_TIME_NTPPROBEFORMAT_H
