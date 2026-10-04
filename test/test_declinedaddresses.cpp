/**
 * @file test_declinedaddresses.cpp
 * @brief Host test for the DHCPDECLINE hold-down (rule 23).
 *
 * What the DHCP server does with an address a client refused decides whether the
 * OFFER/ACK/DECLINE loop of 04.10.2026 can happen at all, and the rule is pure
 * bookkeeping over a fixed array — so it is checked here instead of on the board.
 * The header carries no ESP-IDF dependency on purpose (`test_dhcp` cannot be
 * built on the host: it needs the FreeRTOS headers through `DhcpServer`).
 */

#include "dhcp/DeclinedAddresses.h"

#include <cassert>
#include <cstdio>

using namespace dhcp::dhcp;

namespace {

constexpr uint64_t kSecond = 1000ULL;
constexpr uint32_t kIp101 = 0x6501A8C0;   // 192.168.1.101, as the server keys it
constexpr uint32_t kIp102 = 0x6601A8C0;

} // namespace

int main()
{
    static_assert(DeclinedAddresses::kDeclineHoldMs == 3600000ULL,
                  "the hold-down changed: an address is out of the pool for an hour");
    static_assert(DeclinedAddresses::kCapacity >= 16,
                  "the table must hold a room's worth of refusals");

    DeclinedAddresses declined;

    // Nothing is held down while nobody has refused anything.
    assert(!declined.isDeclined(kIp101, 0));
    assert(declined.count(0) == 0);

    // A refusal takes effect at once — the whole point: the next DISCOVER of the
    // same client must not be offered this address again.
    assert(declined.add(kIp101, 10 * kSecond));
    assert(declined.isDeclined(kIp101, 10 * kSecond));
    assert(declined.isDeclined(kIp101, 10 * kSecond + 1));
    assert(declined.count(10 * kSecond) == 1);

    // Only that address: a refusal is about one address, not the pool.
    assert(!declined.isDeclined(kIp102, 10 * kSecond));

    // The hold-down ends on its own, exactly at the limit and not before.
    assert(declined.isDeclined(kIp101, 10 * kSecond + DeclinedAddresses::kDeclineHoldMs - 1));
    assert(!declined.isDeclined(kIp101, 10 * kSecond + DeclinedAddresses::kDeclineHoldMs));
    assert(declined.count(10 * kSecond + DeclinedAddresses::kDeclineHoldMs) == 0);

    // A second refusal of the same address refreshes it instead of filling the
    // table with copies: a client that keeps refusing must keep it held down.
    DeclinedAddresses repeated;
    assert(repeated.add(kIp101, 0));
    assert(repeated.add(kIp101, DeclinedAddresses::kDeclineHoldMs - kSecond));
    assert(repeated.count(DeclinedAddresses::kDeclineHoldMs - kSecond) == 1);
    // Had the first stamp stood, the address would be free by now.
    assert(repeated.isDeclined(kIp101, DeclinedAddresses::kDeclineHoldMs + kSecond));
    assert(!repeated.isDeclined(kIp101, 2 * DeclinedAddresses::kDeclineHoldMs));

    // Expired entries are reused, so a churning network cannot exhaust the table.
    DeclinedAddresses reused;
    for (size_t i = 0; i < DeclinedAddresses::kCapacity; ++i) {
        assert(reused.add(kIp101 + static_cast<uint32_t>(i), 0));
    }
    assert(reused.count(0) == DeclinedAddresses::kCapacity);
    assert(reused.isDeclined(kIp101 + 30, 0));
    // Past the hold-down every slot is free again, so the newest refusal fits
    // without evicting anything.
    assert(reused.add(kIp102, DeclinedAddresses::kDeclineHoldMs));
    assert(reused.count(DeclinedAddresses::kDeclineHoldMs) == 1);
    assert(reused.isDeclined(kIp102, DeclinedAddresses::kDeclineHoldMs));

    // A full table still remembers the newest refusal: the oldest is the one to
    // forget, and it says so rather than dropping the new one silently.
    DeclinedAddresses full;
    for (size_t i = 0; i < DeclinedAddresses::kCapacity; ++i) {
        assert(full.add(kIp101 + static_cast<uint32_t>(i), static_cast<uint64_t>(i) * kSecond));
    }
    assert(!full.add(kIp102, 100 * kSecond));
    assert(full.isDeclined(kIp102, 100 * kSecond));
    assert(!full.isDeclined(kIp101, 100 * kSecond));   // the oldest went
    assert(full.isDeclined(kIp101 + 31, 100 * kSecond));

    // clear() is what a restarted server does: no refusals survive it.
    full.clear();
    assert(full.count(100 * kSecond) == 0);
    assert(!full.isDeclined(kIp102, 100 * kSecond));

    printf("All DeclinedAddresses tests PASSED!\n");
    return 0;
}
