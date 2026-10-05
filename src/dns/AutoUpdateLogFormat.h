#ifndef DHCP_DNS_AUTOUPDATELOGFORMAT_H
#define DHCP_DNS_AUTOUPDATELOGFORMAT_H

#include <cstdint>
#include <string>

#include "CacheFileReader.h"

namespace dhcp {
namespace dns {

/**
 * @file AutoUpdateLogFormat.h
 * @brief How one refreshed record reads in `logs/AutoUpdate.log` (stage 179).
 *
 * The journal (`Jobs.log`) holds the two ends of an operation and deliberately
 * nothing else (stage 173), so "which record, and what came back" had nowhere to
 * go. The operator asked for it, and this is the text of one such line.
 *
 * Free of ESP-IDF, the same way `core/JobLogFormat.h` is: a log line is a promise
 * about what the file will say, and the host test is where a promise is checked.
 * The stamp and the operation tag are added by the log itself
 * (`[2026-10-05 05:10:12] cache_autoupdate: example.com A refreshed (ttl 300 s)`);
 * this header owns the text after the colon.
 */

/**
 * @brief `A` or `AAAA` — the two types the built-in cache holds.
 *
 * The numbers come from the cache reader, which owns the same pair for the file
 * format, so the log cannot drift from what the cache actually stores. A type
 * that is neither is named by its number rather than guessed at: the cache never
 * holds one, and calling it "AAAA" would be a lie in a diagnostic file.
 */
inline std::string autoUpdateTypeName(uint16_t qtype)
{
    if (qtype == CacheFileReader::kTypeA) return "A";
    if (qtype == CacheFileReader::kTypeAaaa) return "AAAA";
    return std::to_string(qtype);
}

/**
 * @brief One record of a cycle: `example.com A refreshed (ttl 300 s)`.
 *
 * A confirmed record carries the fresh TTL, because that is the whole point of
 * the sweep — the record was young again afterwards. A record the upstream did
 * not confirm is kept and retried next cycle (a timeout is not proof that the
 * name is gone), and the line says exactly that instead of printing a TTL that
 * nobody received.
 *
 * @param name      Domain name, as it is stored in the cache.
 * @param qtype     A (1) or AAAA (28).
 * @param refreshed True when the upstream answered with an address.
 * @param ttlSec    The fresh TTL; read only when @p refreshed is true.
 */
inline std::string autoUpdateRecordText(const std::string& name, uint16_t qtype,
                                        bool refreshed, uint32_t ttlSec)
{
    std::string out = name;
    out += ' ';
    out += autoUpdateTypeName(qtype);
    if (!refreshed) {
        out += " not confirmed, record kept";
        return out;
    }
    out += " refreshed (ttl ";
    out += std::to_string(ttlSec);
    out += " s)";
    return out;
}

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_AUTOUPDATELOGFORMAT_H
