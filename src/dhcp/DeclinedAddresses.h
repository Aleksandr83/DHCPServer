#ifndef DHCP_DHCP_DECLINEDADDRESSES_H
#define DHCP_DHCP_DECLINEDADDRESSES_H

#include <cstddef>
#include <cstdint>

namespace dhcp {
namespace dhcp {

/**
 * @brief The addresses clients refused with DHCPDECLINE, and the hold-down on them.
 *
 * RFC 2131 asks the server to mark such an address unavailable. A DECLINE means
 * the client ARP-probed the address and **somebody answered** — evidence the
 * server's own probe cannot match, because it is the same probe that had just
 * failed: a machine that does not answer ARP for a moment (busy, asleep, sitting
 * behind a link that is dropping packets) is exactly how an address gets offered
 * twice. Without the hold-down the server offers the same address on the next
 * DISCOVER, the client refuses it again, and the pair loops: on the board of
 * 04.10.2026 one client repeated the cycle every 15 seconds — an OFFER, an ACK, a
 * DECLINE, four log lines and three REST posts to the operator's server each
 * time.
 *
 * The table is a fixed array: it lives on the DHCP packet path, it is touched
 * only by the DHCP task, and it must not allocate. Entries walk out by
 * themselves after @ref kDeclineHoldMs, because a refused address is not
 * condemned for ever — the machine that held it may leave the network.
 *
 * Deliberately free of every ESP-IDF include, so the rule above (and the
 * capacity, and the expiry) can be checked on the development machine.
 */
class DeclinedAddresses {
public:
    /// @brief How long a refused address stays out of the pool.
    static constexpr uint64_t kDeclineHoldMs = 60ULL * 60ULL * 1000ULL;
    /// @brief How many refusals are remembered; the oldest is overwritten.
    static constexpr size_t kCapacity = 32;

    /**
     * @brief Remember that @p ip was refused at @p nowMs.
     *
     * A repeat refusal of an address already held down refreshes its hold-down
     * rather than adding a second entry.
     *
     * @return false when the table was full and the oldest entry had to go.
     */
    bool add(uint32_t ip, uint64_t nowMs)
    {
        expire(nowMs);
        for (size_t i = 0; i < kCapacity; ++i) {
            if (entries_[i].used && entries_[i].ip == ip) {
                entries_[i].sinceMs = nowMs;
                return true;
            }
        }
        for (size_t i = 0; i < kCapacity; ++i) {
            if (!entries_[i].used) {
                entries_[i].ip = ip;
                entries_[i].sinceMs = nowMs;
                entries_[i].used = true;
                return true;
            }
        }
        size_t oldest = 0;
        for (size_t i = 1; i < kCapacity; ++i) {
            if (entries_[i].sinceMs < entries_[oldest].sinceMs) oldest = i;
        }
        entries_[oldest].ip = ip;
        entries_[oldest].sinceMs = nowMs;
        entries_[oldest].used = true;
        return false;
    }

    /// @brief True while @p ip is held down and must not be offered.
    bool isDeclined(uint32_t ip, uint64_t nowMs) const
    {
        for (size_t i = 0; i < kCapacity; ++i) {
            if (!entries_[i].used || entries_[i].ip != ip) continue;
            return (nowMs - entries_[i].sinceMs) < kDeclineHoldMs;
        }
        return false;
    }

    /// @brief Drop every entry whose hold-down has expired.
    void expire(uint64_t nowMs)
    {
        for (size_t i = 0; i < kCapacity; ++i) {
            if (entries_[i].used &&
                (nowMs - entries_[i].sinceMs) >= kDeclineHoldMs) {
                entries_[i].used = false;
            }
        }
    }

    /// @brief How many refusals are still in force at @p nowMs.
    size_t count(uint64_t nowMs) const
    {
        size_t n = 0;
        for (size_t i = 0; i < kCapacity; ++i) {
            if (entries_[i].used &&
                (nowMs - entries_[i].sinceMs) < kDeclineHoldMs) {
                ++n;
            }
        }
        return n;
    }

    /// @brief Forget everything: a restarted server remembers no refusals.
    void clear()
    {
        for (size_t i = 0; i < kCapacity; ++i) entries_[i].used = false;
    }

private:
    struct Entry {
        uint32_t ip = 0;
        uint64_t sinceMs = 0;
        bool used = false;
    };
    Entry entries_[kCapacity];
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DECLINEDADDRESSES_H
