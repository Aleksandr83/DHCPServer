#ifndef DHCP_DHCP_NBSTATPROBE_H
#define DHCP_DHCP_NBSTATPROBE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dhcp {
namespace dhcp {

/**
 * @brief NetBIOS node-status probe: "what is this address called?" on a network
 *        where reverse DNS knows nothing.
 *
 * A Windows machine that sends no host name in its DHCP request still answers
 * the NetBIOS node-status query (NBSTAT, UDP 137): one wildcard question for the
 * name table, and the answer lists the names the computer claims for itself.
 * That covers Windows and Samba; a Linux box, a phone or a router will simply
 * not answer — which is an answer too, and the caller says so instead of
 * inventing a name.
 *
 * Build and parse are pure functions on bytes (the message envelope is shared
 * with the reverse-DNS probe through `DnsMessage`), so a hand-made table with
 * group entries, trailing blanks and a truncated tail can all be tested on a
 * host. `query()` is the thin socket wrapper.
 *
 * The name is **not** case-folded: NetBIOS names are upper case by protocol, and
 * turning `OFFICE-PC` into `office-pc` would be inventing data. The operator
 * sees what the computer said and can edit the field.
 */
class NbstatProbe {
public:
    /** @brief How long one attempt waits for an answer (milliseconds). */
    static constexpr uint32_t kTimeoutMs = 400;

    /**
     * @brief The transaction id 0 that buildQuery() must never be given.
     *
     * 0 is a legal id, but it is also what a fresh struct holds, so a query
     * with it is indistinguishable from a bug: nextId() avoids it.
     */
    static constexpr uint16_t kNoId = 0;

    /** @brief Longest query this implementation builds. */
    static constexpr size_t kMaxQueryBytes = 64;

    /** @brief A NetBIOS name is 15 characters plus a suffix byte. */
    static constexpr size_t kNameBytes = 16;
    /** @brief One entry of the answer's name table: name + suffix + flags. */
    static constexpr size_t kEntryBytes = 18;

    /** @brief Suffix of the workstation (computer) name. */
    static constexpr uint8_t kSuffixWorkstation = 0x00;
    /** @brief Set in the entry flags: the name is a group, not a host. */
    static constexpr uint16_t kFlagGroup = 0x8000;

    /**
     * @brief Build the NBSTAT wildcard query.
     * @param id Transaction id the answer must carry back.
     */
    static std::vector<uint8_t> buildQuery(uint16_t id);

    /**
     * @brief Extract the computer name from a node-status answer.
     *
     * The first entry that is a workstation name (suffix 0x00) and not a group
     * wins; if there is none, the first non-group entry is used, because a
     * computer that registered only a server or messenger name still answered
     * with *a* name. Group entries (`__MSBROWSE__` and friends) never win.
     *
     * @return The name with trailing blanks and NULs removed, or "" when the
     *         message is damaged, answers another query, carries no name table,
     *         or holds no usable entry.
     */
    static std::string parseResponse(const uint8_t* buf, size_t len, uint16_t id);

    /**
     * @brief Ask a computer for its name.
     *
     * One attempt, kTimeoutMs of waiting. A host that does not answer, or that
     * answers with an ICMP port-unreachable, gives "".
     *
     * @param ipNet Address to ask (network byte order); 0 = don't try.
     */
    static std::string query(uint32_t ipNet);

    /** @brief A fresh transaction id (random, never 0). */
    static uint16_t nextId();
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_NBSTATPROBE_H
