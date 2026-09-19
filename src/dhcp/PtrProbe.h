#ifndef DHCP_DHCP_PTRPROBE_H
#define DHCP_DHCP_PTRPROBE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dhcp {
namespace dhcp {

/**
 * @brief Reverse DNS (PTR) probe: "what is this address called?".
 *
 * Knowing a client's MAC is not knowing its name. The device can ask the DNS
 * server that registered the address — normally the router, which learns the
 * names from the DHCP requests it answers — for the PTR record of
 * `<addr>.in-addr.arpa`. That is one UDP packet and no dependency on the client
 * answering (a machine can be asleep: the PTR record is still there).
 *
 * Build and parse are pure functions on bytes and therefore testable on a host
 * (DNS message compression — the answer usually points back into the question —
 * is exactly the part worth testing). `query()` is the thin socket wrapper; it
 * is the only part that cannot run off-target.
 *
 * A probe answers a question, it does not establish a fact: whatever comes back
 * is a claim by some server, so `DhcpClientName::sanitize()` runs over it before
 * anybody else sees it.
 */
class PtrProbe {
public:
    /** @brief How long one attempt waits for an answer (milliseconds). */
    static constexpr uint32_t kTimeoutMs = 400;

    /** @brief DNS message limits this implementation respects. */
    static constexpr size_t kMaxQueryBytes = 64;    // IPv4 reverse name is short
    /**
     * @brief The transaction id 0 that buildQuery() must never be given.
     *
     * 0 is a legal id, but it is also what a fresh struct holds, so a query
     * with it is indistinguishable from a bug: nextId() avoids it.
     */
    static constexpr uint16_t kNoId = 0;

    /**
     * @brief Build the PTR query for an IPv4 address.
     *
     * @param ipNet IPv4 address in network byte order (as the DHCP code keeps it).
     * @param id    Transaction id the answer must carry back.
     * @return The datagram to send; empty when the address is not usable
     *         (0.0.0.0, a broadcast, or the reverse name would not fit).
     */
    static std::vector<uint8_t> buildQuery(uint32_t ipNet, uint16_t id);

    /**
     * @brief Extract the PTR name from an answer.
     *
     * @param buf  Received datagram.
     * @param len  Its length.
     * @param id   Transaction id the query was sent with.
     * @return The name, or "" when the answer is not a usable PTR reply
     *         (wrong id, not a response, error rcode, no PTR record, truncated
     *         packet, invalid compression pointer).
     */
    static std::string parseResponse(const uint8_t* buf, size_t len, uint16_t id);

    /**
     * @brief Ask a DNS server for the PTR record of an address.
     *
     * One attempt, kTimeoutMs of waiting — the caller is a person pressing a
     * button, not a background job.
     *
     * @param ipNet     Address to look up (network byte order).
     * @param serverNet DNS server to ask (network byte order); 0 = don't try.
     * @return The name, or "" when nothing came back.
     */
    static std::string query(uint32_t ipNet, uint32_t serverNet);

    /** @brief A fresh transaction id (random, never 0). */
    static uint16_t nextId();
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_PTRPROBE_H
