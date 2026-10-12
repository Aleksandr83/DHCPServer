/**
 * @file test_ntpfallback.cpp
 * @brief Unit tests for the server list of the SNTP client and for the probe
 *        that says which server answered (stage 195).
 *
 * The device used to ask exactly one server, and one dead node in a pool was
 * enough to leave it without a clock for as long as that address stayed in the
 * cached answer: the client takes the first address lwIP returns and lwIP does
 * not rotate the addresses of one answer. Two things had to become checkable
 * without a network, which is what this file does:
 *   * how the "External NTP server" setting turns into an ordered list (either
 *     separator, trimming, at most `kNtpMaxServers`, a bounded text), and
 *   * what the probe sends and which answers it accepts - a 48-byte mode-3
 *     request of our own, and a reply only when it echoes our own transmit
 *     stamp, so a stray packet into that socket cannot be read as an answer.
 * The wording of the journal line is pinned here too: it is the only thing the
 * operator sees of all this.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_ntpfallback.cpp -o test_ntpfallback
 */
#include "src/core/NtpServers.h"
#include "src/time/NtpProbeFormat.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

using dhcp::core::kNtpMaxServers;
using dhcp::core::kNtpServersTextMax;
using dhcp::core::ntpServersSplit;
using dhcp::core::ntpServersText;

using dhcp::time::kNtpProbeTimeoutMs;
using dhcp::time::NtpProbeEntry;
using dhcp::time::NtpProbeVerdict;
using dhcp::time::ntpProbeBuildRequest;
using dhcp::time::ntpProbeEntryText;
using dhcp::time::ntpProbeIsReply;
using dhcp::time::ntpProbeLine;
using dhcp::time::ntpProbeVerdictName;
using dhcp::time::ntpMakeLiVnMode;
using dhcp::time::kNtpModeClient;
using dhcp::time::kNtpModeServer;
using dhcp::time::kNtpPacketBytes;
using dhcp::time::unixToNtp;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const string& what)
{
    ++g_checks;
    if (ok) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        ++g_failed;
    }
}

static string join(const vector<string>& parts)
{
    string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += " | ";
        out += part;
    }
    return out;
}

// ─── The setting: text → list → text ────────────────

static void test_split()
{
    check(join(ntpServersSplit("pool.ntp.org")) == "pool.ntp.org",
          "one name is one server");
    check(join(ntpServersSplit("pool.ntp.org, time.google.com")) ==
              "pool.ntp.org | time.google.com",
          "a comma separates, and the spaces around the names are not part of them");
    check(join(ntpServersSplit("a;b;c")) == "a | b | c",
          "a semicolon separates as well");
    check(join(ntpServersSplit(" a ,; b ")) == "a | b",
          "mixed separators, empty entries and padding collapse into two names");
    check(join(ntpServersSplit(",;,")) == "",
          "separators alone name no server at all");
    check(join(ntpServersSplit("")) == "", "an empty setting names no server");
}

static void test_cap()
{
    const auto four = ntpServersSplit("a, b, c, d");
    check(four.size() == kNtpMaxServers, "at most three servers are kept");
    check(join(four) == "a | b | c", "and they are the first three, in order");
}

static void test_text()
{
    check(ntpServersText({"pool.ntp.org", "time.google.com"}) ==
              "pool.ntp.org, time.google.com",
          "the canonical text joins with a comma and one space");
    check(ntpServersText({"a", "", "b"}) == "a, b",
          "an empty entry is not written out");
    check(ntpServersText({}) == "", "an empty list is an empty setting");
    check(ntpServersText(ntpServersSplit(" a ,; b ")) == "a, b",
          "splitting and joining again is what the device stores");
    check(ntpServersText({"a", "b", "c", "d"}) == "a, b, c",
          "the text form keeps the same three servers the split keeps");
}

static void test_text_is_bounded()
{
    const string monster(kNtpServersTextMax + 50, 'x');
    const string text = ntpServersText({monster});
    check(text.size() == kNtpServersTextMax,
          "a name longer than the setting can hold is cut at the limit");
    const string two = ntpServersText({string(60, 'a'), string(60, 'b')});
    check(two.size() <= kNtpServersTextMax,
          "and a list cannot grow past the limit either");
}

// ─── The probe packet ───────────────────────────────

/** The packet the probe would send for a known moment. */
static vector<uint8_t> requestPacket(uint32_t unixSec, uint32_t unixUsec)
{
    vector<uint8_t> packet(kNtpPacketBytes, 0);
    ntpProbeBuildRequest(packet.data(), packet.size(), unixSec, unixUsec);
    return packet;
}

/** An answer to that request, as a server would build it. */
static vector<uint8_t> replyPacket(const vector<uint8_t>& request, uint8_t stratum)
{
    vector<uint8_t> reply = request;
    // Originate = the client's transmit stamp, which is what identifies the
    // answer as being for us (RFC 5905).
    memcpy(&reply[24], &request[40], 8);
    reply[0] = ntpMakeLiVnMode(0, 4, kNtpModeServer);
    reply[1] = stratum;
    return reply;
}

static void test_request_shape()
{
    const auto req = requestPacket(1791700000u, 0u);
    check(req.size() == kNtpPacketBytes, "the request is the 48 bytes of RFC 5905");
    check((req[0] & 0x07u) == kNtpModeClient, "and it is a client request (mode 3)");
    check(((req[0] >> 3) & 0x07u) == 4, "of version 4");
    check((req[0] >> 6) == 0, "with no leap warning");

    uint32_t wantSec = 0, wantFrac = 0;
    unixToNtp(1791700000u, 0u, wantSec, wantFrac);
    uint32_t gotSec = 0;
    memcpy(&gotSec, &req[40], 4);
    uint32_t gotFrac = 0;
    memcpy(&gotFrac, &req[44], 4);
    check(gotSec == dhcp::time::toBe32(wantSec), "the transmit stamp is NTP seconds");
    check(gotFrac == dhcp::time::toBe32(wantFrac), "and an NTP fraction");

    check(req[1] == 0, "a client sends no stratum");
    for (size_t i = 2; i < 40; i++) {
        if (req[i] != 0) {
            check(false, "the rest of the request is zero, byte " + to_string(i));
            return;
        }
    }
    check(true, "the rest of the request is zero");
}

static void test_reply_acceptance()
{
    const auto req = requestPacket(1791700000u, 500000u);
    uint8_t stratum = 0xFF;

    check(ntpProbeIsReply(replyPacket(req, 2).data(), kNtpPacketBytes,
                          req.data(), req.size(), &stratum) && stratum == 2,
          "a mode-4 answer echoing our stamp is accepted, with its stratum");
    check(ntpProbeIsReply(replyPacket(req, 0).data(), kNtpPacketBytes,
                          req.data(), req.size(), &stratum) && stratum == 0,
          "a kiss-of-death (stratum 0) is an answer too - the server did reply");

    auto wrongStamp = replyPacket(req, 2);
    wrongStamp[24] ^= 0x01;   // the originate stamp of somebody else's request
    check(!ntpProbeIsReply(wrongStamp.data(), kNtpPacketBytes, req.data(), req.size()),
          "an answer to another request is not ours");

    auto isRequest = replyPacket(req, 2);
    isRequest[0] = ntpMakeLiVnMode(0, 4, kNtpModeClient);
    check(!ntpProbeIsReply(isRequest.data(), kNtpPacketBytes, req.data(), req.size()),
          "a client request arriving on our socket is not an answer");

    auto shorter = replyPacket(req, 2);
    shorter.resize(kNtpPacketBytes - 1);
    check(!ntpProbeIsReply(shorter.data(), shorter.size(), req.data(), req.size()),
          "a datagram shorter than an NTP packet is not an answer");

    check(!ntpProbeIsReply(nullptr, 0, req.data(), req.size()),
          "and nothing at all is not an answer either");
}

// ─── The journal line ───────────────────────────────

static NtpProbeEntry entry(const string& name, const string& ip, NtpProbeVerdict v,
                           uint8_t stratum = 0)
{
    NtpProbeEntry e;
    e.name = name;
    e.ip = ip;
    e.verdict = v;
    e.stratum = stratum;
    return e;
}

static void test_entry_text()
{
    check(ntpProbeEntryText(entry("time.google.com", "216.239.35.8", NtpProbeVerdict::Replied, 1),
                            kNtpProbeTimeoutMs) ==
              "time.google.com (216.239.35.8) - replied (stratum 1)",
          "a server that answered is reported with the address and the stratum");
    check(ntpProbeEntryText(entry("pool.ntp.org", "51.250.53.172", NtpProbeVerdict::NoReply),
                            kNtpProbeTimeoutMs) ==
              "pool.ntp.org (51.250.53.172) - no reply in 3 s",
          "a silent server is named with the address that was asked and the wait");
    check(ntpProbeEntryText(entry("broken.example", "", NtpProbeVerdict::Unresolved),
                            kNtpProbeTimeoutMs) == "broken.example - name did not resolve",
          "a name that does not resolve does not pretend to have an address");
    check(ntpProbeEntryText(entry("ntp1.vniiftri.ru", "89.109.251.21", NtpProbeVerdict::NotAsked),
                            kNtpProbeTimeoutMs) ==
              "ntp1.vniiftri.ru - not asked (an earlier server replied)",
          "a server after the answer is reported as not asked");
    check(ntpProbeEntryText(entry("bad.example", "10.0.0.1", NtpProbeVerdict::Replied, 0),
                            kNtpProbeTimeoutMs) ==
              "bad.example (10.0.0.1) - replied (stratum 0, kiss-of-death)",
          "and a kiss-of-death is named as such, not as a healthy server");
}

static void test_line_text()
{
    const vector<NtpProbeEntry> entries = {
        entry("pool.ntp.org", "51.250.53.172", NtpProbeVerdict::NoReply),
        entry("time.google.com", "216.239.35.8", NtpProbeVerdict::Replied, 1),
        entry("ntp1.vniiftri.ru", "89.109.251.21", NtpProbeVerdict::NotAsked),
    };
    check(ntpProbeLine(entries, kNtpProbeTimeoutMs) ==
              "NTP probe: pool.ntp.org (51.250.53.172) - no reply in 3 s; "
              "time.google.com (216.239.35.8) - replied (stratum 1); "
              "ntp1.vniiftri.ru - not asked (an earlier server replied)",
          "one line carries the whole run, in the order the servers were asked");
    check(ntpProbeLine({}, kNtpProbeTimeoutMs) == "NTP probe: ",
          "with no server there is nothing to report beyond the prefix");
}

static void test_verdict_names()
{
    // The "Check" button on the page reads these names out of the REST answer, so
    // they are a contract between the device and the page: renaming one here
    // without the page would break the button in silence.
    check(string(ntpProbeVerdictName(NtpProbeVerdict::Replied)) == "replied",
          "a reply is reported as \"replied\"");
    check(string(ntpProbeVerdictName(NtpProbeVerdict::NoReply)) == "no_reply",
          "silence as \"no_reply\"");
    check(string(ntpProbeVerdictName(NtpProbeVerdict::Unresolved)) == "unresolved",
          "a name that never resolved as \"unresolved\"");
    check(string(ntpProbeVerdictName(NtpProbeVerdict::NotAsked)) == "not_asked",
          "a server left alone as \"not_asked\"");
    // Four verdicts, four different names: two of them sharing one would make the
    // page say the wrong thing about a server.
    const string names[] = {
        ntpProbeVerdictName(NtpProbeVerdict::Replied),
        ntpProbeVerdictName(NtpProbeVerdict::NoReply),
        ntpProbeVerdictName(NtpProbeVerdict::Unresolved),
        ntpProbeVerdictName(NtpProbeVerdict::NotAsked),
    };
    bool unique = true;
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = i + 1; j < 4; j++) {
            if (names[i] == names[j]) unique = false;
        }
    }
    check(unique, "and no two verdicts share a name");
}

int main()
{
    printf("Running NtpFallback tests...\n");
    test_split();
    test_cap();
    test_text();
    test_text_is_bounded();
    test_request_shape();
    test_reply_acceptance();
    test_entry_text();
    test_line_text();
    test_verdict_names();

    if (g_failed == 0) {
        printf("All %d NtpFallback checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d NtpFallback checks FAILED!\n", g_failed, g_checks);
    return 1;
}
