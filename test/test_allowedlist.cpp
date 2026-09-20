/**
 * @file test_allowedlist.cpp
 * @brief Unit tests for the DHCP allow-list ("allowed computers"): text codec,
 *        PSRAM MAC hash table and the access policy.
 *
 * The module has no ESP-IDF dependency on purpose (the table works on storage
 * its owner hands in, see DhcpAllowedList.h), so this test runs on a **host**
 * with nothing but a compiler.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -I. \
 *       test/test_allowedlist.cpp src/dhcp/DhcpAllowedList.cpp host_main.cpp \
 *       -o test_allowedlist
 *
 * Without `DHCP_TEST_HOST` the file compiles to an `app_main` that does
 * nothing: the interesting cases need a caller-owned buffer and a freely chosen
 * list size, which the on-device build cannot offer.
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/dhcp/DhcpAllowedList.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (string(a) != string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, string(a).c_str(), string(b).c_str()); return 1; } } while(0)

using dhcp::dhcp::AllowedComputer;
using dhcp::dhcp::DhcpAllowedList;

namespace {

/** Storage for one list, exactly the size the class asks for plus a canary. */
struct Storage {
    Storage() : buf(DhcpAllowedList::storageBytes() + kCanary, 0xAB) {}
    void* data() { return buf.data(); }
    bool canaryIntact() const
    {
        for (size_t i = DhcpAllowedList::storageBytes(); i < buf.size(); i++) {
            if (buf[i] != 0xAB) return false;
        }
        return true;
    }
    static constexpr size_t kCanary = 16;
    vector<unsigned char> buf;
};

AllowedComputer entry(const char* mac, const char* name = "")
{
    AllowedComputer e;
    e.mac = mac;
    e.name = name;
    return e;
}

bool macOf(const char* text, unsigned char out[6])
{
    return DhcpAllowedList::parseMac(text, out);
}

/** A table built from a list, ready to be queried. */
struct Built {
    explicit Built(const vector<AllowedComputer>& list)
    {
        ok = list_.init(storage.data());
        ok = ok && list_.rebuild(list);
    }
    Storage storage;
    DhcpAllowedList list_;
    bool ok = false;
    bool canaryIntact() const { return storage.canaryIntact(); }
};

} // namespace

extern "C" {

/** Text MACs are accepted in the shapes a human types them. */
static int test_mac_codec()
{
    unsigned char mac[6];

    TEST_ASSERT_TRUE(macOf("24:0A:C4:01:23:45", mac));
    TEST_ASSERT_EQ(mac[0], 0x24);
    TEST_ASSERT_EQ(mac[5], 0x45);
    TEST_ASSERT_STR_EQ(DhcpAllowedList::formatMac(mac), "24:0a:c4:01:23:45");
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac("24-0a-c4-01-23-45"),
                       "24:0a:c4:01:23:45");
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac("240ac4012345"),
                       "24:0a:c4:01:23:45");
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac("240a.c401.2345"),
                       "24:0a:c4:01:23:45");

    // Not MAC addresses: empty, too short, too long, a stray character, and
    // an IPv4 address that happens to have the right length.
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac(""), "");
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac("24:0a:c4:01:23"), "");
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac("24:0a:c4:01:23:45:67"), "");
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac("24:0a:c4:01:23:4g"), "");
    TEST_ASSERT_STR_EQ(DhcpAllowedList::normalizeMac("192.168.1.1"), "");
    return 0;
}

/** Round trip through the NVS text format, including a damaged line. */
static int test_codec_roundtrip()
{
    vector<AllowedComputer> list{
        entry("24:0A:C4:01:23:45", "office-pc"),
        entry("AA-BB-CC-DD-EE-FF", ""),           // no name
        entry("001122334455", "tablet"),
    };
    const string text = DhcpAllowedList::serialize(list);
    TEST_ASSERT_STR_EQ(text,
                       "24:0a:c4:01:23:45|office-pc|1\n"
                       "aa:bb:cc:dd:ee:ff||1\n"
                       "00:11:22:33:44:55|tablet|1");

    auto back = DhcpAllowedList::parse(text);
    TEST_ASSERT_EQ(back.size(), 3u);
    TEST_ASSERT_STR_EQ(back[0].mac, "24:0a:c4:01:23:45");
    TEST_ASSERT_STR_EQ(back[0].name, "office-pc");
    TEST_ASSERT_TRUE(back[0].enabled);
    TEST_ASSERT_STR_EQ(back[1].name, "");
    TEST_ASSERT_STR_EQ(back[2].mac, "00:11:22:33:44:55");
    TEST_ASSERT_EQ(DhcpAllowedList::serializedBytes(list), text.size());

    // A damaged line must not take the rest of the list with it, a '|' inside a
    // name must not split it into a second entry (the line stays ONE entry: the
    // third field belongs to the Enable column), and CRLF text (a hand-edited
    // export) must parse.
    auto mixed = DhcpAllowedList::parse(
        "garbage\n24:0a:c4:01:23:45|keep\r\n|\nAA:BB:CC:DD:EE:F0|a|b\r\n");
    TEST_ASSERT_EQ(mixed.size(), 2u);
    TEST_ASSERT_STR_EQ(mixed[0].name, "keep");
    TEST_ASSERT_TRUE(mixed[0].enabled);       // no third field: enabled
    TEST_ASSERT_STR_EQ(mixed[1].mac, "aa:bb:cc:dd:ee:f0");
    TEST_ASSERT_STR_EQ(mixed[1].name, "a");
    TEST_ASSERT_TRUE(mixed[1].enabled);       // "b" is not "0"
    return 0;
}

/** The per-entry Enable checkbox survives the codec, the old format too. */
static int test_codec_enable_flag()
{
    AllowedComputer off = entry("24:0a:c4:01:23:45", "old-pc");
    off.enabled = false;
    AllowedComputer on = entry("24:0a:c4:01:23:46", "new-pc");

    const string text = DhcpAllowedList::serialize({off, on});
    TEST_ASSERT_STR_EQ(text,
                       "24:0a:c4:01:23:45|old-pc|0\n"
                       "24:0a:c4:01:23:46|new-pc|1");

    auto back = DhcpAllowedList::parse(text);
    TEST_ASSERT_EQ(back.size(), 2u);
    TEST_ASSERT_FALSE(back[0].enabled);
    TEST_ASSERT_TRUE(back[1].enabled);

    // An entry written before the column existed (two fields only) is enabled,
    // and a bare MAC address (no separator at all) is enabled as well.
    auto legacy = DhcpAllowedList::parse("24:0a:c4:01:23:45|old-pc\n"
                                         "24:0a:c4:01:23:46");
    TEST_ASSERT_EQ(legacy.size(), 2u);
    TEST_ASSERT_TRUE(legacy[0].enabled);
    TEST_ASSERT_STR_EQ(legacy[0].name, "old-pc");
    TEST_ASSERT_TRUE(legacy[1].enabled);
    return 0;
}

/** The list is bounded: kMaxEntries entries, and 20 characters per name. */
static int test_codec_limits()
{
    vector<AllowedComputer> many;
    const int over = static_cast<int>(DhcpAllowedList::kMaxEntries) + 3;
    for (int i = 0; i < over; i++) {
        char mac[18];
        snprintf(mac, sizeof(mac), "24:0a:c4:01:23:%02x", i);
        many.push_back(entry(mac, "entry"));
    }
    auto text = DhcpAllowedList::serialize(many);
    TEST_ASSERT_EQ(DhcpAllowedList::parse(text).size(), DhcpAllowedList::kMaxEntries);

    string longName(40, 'x');
    vector<AllowedComputer> one{entry("24:0a:c4:01:23:45", longName.c_str())};
    auto trimmed = DhcpAllowedList::parse(DhcpAllowedList::serialize(one));
    TEST_ASSERT_EQ(trimmed.size(), 1u);
    TEST_ASSERT_EQ(trimmed[0].name.size(), DhcpAllowedList::kMaxNameLen);
    return 0;
}

/**
 * The NVS budget and the entry limit are derived from the format, not chosen:
 * "mac|name|enabled" is 40 bytes at its longest, serialize() puts no '\n' after
 * the last entry, and 1024 bytes are exactly what 25 of them cost. Changing one
 * number without the others breaks this test — which is the point.
 */
static int test_blob_budget_matches_the_format()
{
    const size_t perEntry = 17 + 1 + DhcpAllowedList::kMaxNameLen + 1 + 1;
    TEST_ASSERT_EQ(perEntry, 40u);
    TEST_ASSERT_EQ(DhcpAllowedList::kMaxEntries, 25u);
    TEST_ASSERT_EQ(DhcpAllowedList::kMaxNameLen, 20u);
    TEST_ASSERT_EQ(DhcpAllowedList::kMaxBytes, 1024u);

    const size_t worst = DhcpAllowedList::kMaxEntries * perEntry +
                         (DhcpAllowedList::kMaxEntries - 1);
    TEST_ASSERT_EQ(worst, DhcpAllowedList::kMaxBytes);          // the budget, exactly
    const size_t oneMore = (DhcpAllowedList::kMaxEntries + 1) * perEntry +
                           DhcpAllowedList::kMaxEntries;
    TEST_ASSERT_TRUE(oneMore > DhcpAllowedList::kMaxBytes);     // one more: too big

    // The codec agrees with that arithmetic: a full list of the longest names
    // serializes to exactly the budget — not one byte more, not one less.
    string longName(DhcpAllowedList::kMaxNameLen + 5, 'x');
    vector<AllowedComputer> full;
    for (size_t i = 0; i < DhcpAllowedList::kMaxEntries; i++) {
        char mac[18];
        snprintf(mac, sizeof(mac), "24:0a:c4:01:23:%02x", static_cast<unsigned>(i));
        full.push_back(entry(mac, longName.c_str()));
    }
    const string text = DhcpAllowedList::serialize(full);
    TEST_ASSERT_EQ(text.size(), DhcpAllowedList::kMaxBytes);
    TEST_ASSERT_EQ(DhcpAllowedList::parse(text).size(), DhcpAllowedList::kMaxEntries);
    return 0;
}

/** The table answers exactly what was put in, and only that. */
static int test_table_lookup()
{
    Built built({entry("24:0A:C4:01:23:45", "a"), entry("00:11:22:33:44:55", "b")});
    TEST_ASSERT_TRUE(built.ok);
    TEST_ASSERT_TRUE(built.list_.available());
    TEST_ASSERT_EQ(built.list_.count(), 2u);

    unsigned char mac[6];
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:45", mac));
    TEST_ASSERT_TRUE(built.list_.contains(mac));
    TEST_ASSERT_TRUE(macOf("00:11:22:33:44:55", mac));
    TEST_ASSERT_TRUE(built.list_.contains(mac));
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:46", mac));   // one bit off
    TEST_ASSERT_FALSE(built.list_.contains(mac));
    TEST_ASSERT_TRUE(macOf("ff:ff:ff:ff:ff:ff", mac));
    TEST_ASSERT_FALSE(built.list_.contains(mac));
    TEST_ASSERT_TRUE(built.canaryIntact());
    return 0;
}

/** Duplicates and over-long lists are dropped, not stored twice. */
static int test_table_limits_and_duplicates()
{
    // 1. The same MAC twice (in different spellings) is one entry.
    vector<AllowedComputer> dup{
        entry("24:0a:c4:01:23:00", "one"),
        entry("24:0A:C4:01:23:00", "again"),
        entry("24:0a:c4:01:23:01", "two"),
    };
    Built built(dup);
    size_t skipped = 0;
    TEST_ASSERT_TRUE(built.list_.rebuild(dup, &skipped));
    TEST_ASSERT_EQ(built.list_.count(), 2u);
    TEST_ASSERT_EQ(skipped, 1u);

    // 2. More MACs than the table holds: it keeps kMaxEntries and says how many
    //    it dropped.
    vector<AllowedComputer> many;
    const int over = static_cast<int>(DhcpAllowedList::kMaxEntries) + 5;
    for (int i = 0; i < over; i++) {
        char mac[18];
        snprintf(mac, sizeof(mac), "24:0a:c4:01:23:%02x", i);
        many.push_back(entry(mac, "e"));
    }
    skipped = 0;
    TEST_ASSERT_TRUE(built.list_.rebuild(many, &skipped));
    TEST_ASSERT_EQ(built.list_.count(), DhcpAllowedList::kMaxEntries);
    TEST_ASSERT_EQ(skipped, static_cast<size_t>(over) - DhcpAllowedList::kMaxEntries);

    // 3. A junk line never reaches the table.
    skipped = 0;
    TEST_ASSERT_TRUE(built.list_.rebuild({entry("not-a-mac", "junk")}, &skipped));
    TEST_ASSERT_EQ(built.list_.count(), 0u);
    TEST_ASSERT_EQ(skipped, 1u);
    return 0;
}

/** A rebuild replaces the table: the previous list is gone. */
static int test_table_rebuild_swaps()
{
    Built built({entry("24:0a:c4:01:23:01", "old")});
    unsigned char oldMac[6];
    unsigned char newMac[6];
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:01", oldMac));
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:02", newMac));
    TEST_ASSERT_TRUE(built.list_.contains(oldMac));

    // Two rebuilds: the second one reuses the buffer the first one left behind,
    // which is where a stale entry would survive.
    TEST_ASSERT_TRUE(built.list_.rebuild({entry("24:0a:c4:01:23:02", "new")}));
    TEST_ASSERT_FALSE(built.list_.contains(oldMac));
    TEST_ASSERT_TRUE(built.list_.contains(newMac));
    TEST_ASSERT_EQ(built.list_.count(), 1u);

    TEST_ASSERT_TRUE(built.list_.rebuild({entry("24:0a:c4:01:23:01", "back")}));
    TEST_ASSERT_TRUE(built.list_.contains(oldMac));
    TEST_ASSERT_FALSE(built.list_.contains(newMac));

    // An empty list empties the table (the switch stays on, nobody is allowed).
    TEST_ASSERT_TRUE(built.list_.rebuild({}));
    TEST_ASSERT_EQ(built.list_.count(), 0u);
    TEST_ASSERT_FALSE(built.list_.contains(oldMac));
    return 0;
}

/** Without storage the table is unusable and the policy fails open. */
static int test_unavailable_fails_open()
{
    DhcpAllowedList list;
    TEST_ASSERT_FALSE(list.available());
    TEST_ASSERT_FALSE(list.init(nullptr));
    size_t skipped = 7;
    TEST_ASSERT_FALSE(list.rebuild({entry("24:0a:c4:01:23:45", "a")}, &skipped));
    TEST_ASSERT_EQ(skipped, 1u);

    unsigned char mac[6];
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:45", mac));
    TEST_ASSERT_FALSE(list.contains(mac));

    // Switch ON, table unavailable: everybody is served (a memory failure must
    // not take the LAN off the air) -- see the class documentation.
    TEST_ASSERT_TRUE(DhcpAllowedList::isClientAllowed(mac, true, list, {}));
    // Two buffers of kSlots 8-byte slots — the 1024-byte budget doubles this.
    TEST_ASSERT_EQ(DhcpAllowedList::storageBytes(), 1024u);
    return 0;
}

/** Switch OFF: the list is ignored, whatever it holds. */
static int test_policy_switch_off_ignores_list()
{
    Built built({entry("24:0a:c4:01:23:45", "only")});
    unsigned char stranger[6];
    TEST_ASSERT_TRUE(macOf("aa:bb:cc:dd:ee:ff", stranger));
    TEST_ASSERT_FALSE(built.list_.contains(stranger));
    TEST_ASSERT_TRUE(DhcpAllowedList::isClientAllowed(stranger, false, built.list_, {}));
    return 0;
}

/** Switch ON: the list decides, and an enabled binding counts as allowed. */
static int test_policy_switch_on()
{
    Built built({entry("24:0a:c4:01:23:45", "listed")});

    unsigned char listed[6];
    unsigned char bound[6];
    unsigned char disabled[6];
    unsigned char stranger[6];
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:45", listed));
    TEST_ASSERT_TRUE(macOf("aa:bb:cc:dd:ee:01", bound));
    TEST_ASSERT_TRUE(macOf("aa:bb:cc:dd:ee:02", disabled));
    TEST_ASSERT_TRUE(macOf("aa:bb:cc:dd:ee:03", stranger));

    vector<DhcpAllowedList::StaticRef> bindings(2);
    memcpy(bindings[0].mac, bound, 6);
    bindings[0].enabled = true;
    memcpy(bindings[1].mac, disabled, 6);
    bindings[1].enabled = false;   // the per-binding Enable checkbox is off

    TEST_ASSERT_TRUE(DhcpAllowedList::isClientAllowed(listed, true, built.list_, bindings));
    TEST_ASSERT_TRUE(DhcpAllowedList::isClientAllowed(bound, true, built.list_, bindings));
    TEST_ASSERT_FALSE(DhcpAllowedList::isClientAllowed(disabled, true, built.list_, bindings));
    TEST_ASSERT_FALSE(DhcpAllowedList::isClientAllowed(stranger, true, built.list_, bindings));
    return 0;
}

/** A switched-off entry is kept in the list but never enters the table. */
static int test_disabled_entry_is_not_allowed()
{
    AllowedComputer off = entry("24:0a:c4:01:23:45", "old-pc");
    off.enabled = false;

    Built built({off, entry("24:0a:c4:01:23:46", "new-pc")});
    TEST_ASSERT_EQ(built.list_.count(), 1u);        // only the enabled one

    unsigned char offMac[6];
    unsigned char onMac[6];
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:45", offMac));
    TEST_ASSERT_TRUE(macOf("24:0a:c4:01:23:46", onMac));
    TEST_ASSERT_FALSE(built.list_.contains(offMac));
    TEST_ASSERT_TRUE(built.list_.contains(onMac));

    // And the policy refuses it with the switch ON (an enabled static binding
    // may still cover it — that is a different check, see test_policy_switch_on).
    TEST_ASSERT_FALSE(DhcpAllowedList::isClientAllowed(offMac, true, built.list_, {}));
    TEST_ASSERT_TRUE(DhcpAllowedList::isClientAllowed(onMac, true, built.list_, {}));

    // Switching an entry off is not an error: nothing is "skipped" for it, and
    // a list whose every entry is off leaves an empty (but available) table.
    size_t skipped = 99;
    TEST_ASSERT_TRUE(built.list_.rebuild({off}, &skipped));
    TEST_ASSERT_EQ(built.list_.count(), 0u);
    TEST_ASSERT_EQ(skipped, 0u);
    TEST_ASSERT_TRUE(built.list_.available());
    return 0;
}

void app_main()
{
    printf("Running DhcpAllowedList tests...\n");
    int failures = 0;

    failures += test_mac_codec();
    failures += test_codec_roundtrip();
    failures += test_codec_enable_flag();
    failures += test_codec_limits();
    failures += test_blob_budget_matches_the_format();
    failures += test_table_lookup();
    failures += test_table_limits_and_duplicates();
    failures += test_table_rebuild_swaps();
    failures += test_disabled_entry_is_not_allowed();
    failures += test_unavailable_fails_open();
    failures += test_policy_switch_off_ignores_list();
    failures += test_policy_switch_on();

    if (failures == 0) {
        printf("All DhcpAllowedList tests PASSED!\n");
    } else {
        printf("Some DhcpAllowedList tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#else   // !DHCP_TEST_HOST

#include <cstdio>

extern "C" void app_main()
{
    printf("test_allowedlist is a host-only test (build with -DDHCP_TEST_HOST)"
           " - see the file header.\n");
}

#endif  // DHCP_TEST_HOST
