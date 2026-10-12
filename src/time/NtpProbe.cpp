#include "NtpProbe.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "esp_log.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

using namespace std;

namespace dhcp {
namespace time {

namespace {

constexpr const char* TAG = "NtpProbe";

/// Room for the 48-byte answer and whatever a server adds to it.
constexpr size_t kResponseBytes = 64;

/// The service name is not used: the port is set from kNtpPort, which is the
/// one number both the client inside the device and the servers outside it use.
} // namespace

bool NtpProbe::resolve(const string& name, string& ipOut) const
{
    // The netdb call rather than lwIP's dns_gethostbyname: that one must run on
    // the lwIP thread, and this runs on the probe's own task. It is the same
    // resolver either way, which is what makes the probe's verdict meaningful
    // for the SNTP client too.
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* found = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        if (found != nullptr) freeaddrinfo(found);
        return false;
    }

    char text[INET_ADDRSTRLEN] = {0};
    const auto* addr = reinterpret_cast<const struct sockaddr_in*>(found->ai_addr);
    const bool ok = inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr;
    freeaddrinfo(found);
    if (!ok) return false;

    ipOut = text;
    return true;
}

bool NtpProbe::ask(const string& ip, uint8_t& stratumOut) const
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "no socket for the NTP probe: %s", strerror(errno));
        return false;
    }

    struct timeval wait;
    wait.tv_sec = static_cast<time_t>(timeoutMs_ / kNtpProbeMsPerSec);
    wait.tv_usec = static_cast<suseconds_t>((timeoutMs_ % kNtpProbeMsPerSec) * kNtpProbeMsPerSec);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(kNtpPort);
    if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) {
        close(sock);
        return false;
    }

    struct timeval now;
    gettimeofday(&now, nullptr);
    uint8_t request[kNtpPacketBytes] = {0};
    ntpProbeBuildRequest(request, sizeof(request), static_cast<uint32_t>(now.tv_sec),
                         static_cast<uint32_t>(now.tv_usec));

    bool replied = false;
    const int sent = sendto(sock, request, sizeof(request), 0,
                            reinterpret_cast<const struct sockaddr*>(&to), sizeof(to));
    if (sent == static_cast<int>(sizeof(request))) {
        uint8_t answer[kResponseBytes];
        // A timeout is ERR_EWOULDBLOCK with a NULL return: silence, which is a
        // result here and not an error.
        const int got = recvfrom(sock, answer, sizeof(answer), 0, nullptr, nullptr);
        if (got > 0) {
            replied = ntpProbeIsReply(answer, static_cast<size_t>(got), request,
                                      sizeof(request), &stratumOut);
        }
    }
    close(sock);
    return replied;
}

vector<NtpProbeEntry> NtpProbe::run(const vector<string>& servers) const
{
    vector<NtpProbeEntry> entries;
    bool answered = false;

    for (const string& name : servers) {
        NtpProbeEntry entry;
        entry.name = name;

        if (answered) {
            entry.verdict = NtpProbeVerdict::NotAsked;
        } else if (!resolve(name, entry.ip)) {
            entry.verdict = NtpProbeVerdict::Unresolved;
        } else {
            uint8_t stratum = 0;
            if (ask(entry.ip, stratum)) {
                entry.verdict = NtpProbeVerdict::Replied;
                entry.stratum = stratum;
                answered = true;
            } else {
                entry.verdict = NtpProbeVerdict::NoReply;
            }
        }
        entries.push_back(entry);
    }
    return entries;
}

} // namespace time
} // namespace dhcp
