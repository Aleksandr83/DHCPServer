#ifndef DHCP_DHCP_DHCPALLOWEDLIST_H
#define DHCP_DHCP_DHCPALLOWEDLIST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dhcp {
namespace dhcp {

/**
 * @brief One entry of the DHCP allow-list ("allowed computers").
 */
struct AllowedComputer {
    std::string mac;   // normalized "24:0a:c4:01:23:45"
    std::string name;  // optional friendly name (<= kMaxNameLen)
    // Per-entry Enable checkbox (the same column Static Bindings has). A
    // disabled entry stays in the list — the operator keeps its MAC and name
    // for later — but it does not allow anything: it is left out of the hash
    // table, so a client whose only entry is switched off is refused.
    bool enabled = true;
};

/**
 * @brief DHCP allow-list: NVS codec, MAC hash table and the access policy.
 *
 * DHCP -> General has a switch ("assign addresses only to allowed computers").
 * While it is OFF the list is ignored entirely and addresses are handed out as
 * before. While it is ON a client that is neither in the list nor covered by an
 * ENABLED static binding gets no address at all.
 *
 * The list itself is a short text blob in NVS ("mac|name|enabled" per line, see
 * serialize/parse — the same codec is used by the settings export). For the
 * DHCP hot path it is mirrored into a fixed-size MAC hash table (open
 * addressing, linear probing) so a lookup is one hash plus a few memcmp calls
 * instead of a walk over strings. Entries whose Enable checkbox is off stay in
 * the list but never enter the table (see AllowedComputer).
 *
 * The table lives in memory supplied by its owner (`init(storage)`), which on
 * the target is a PSRAM block (see DhcpServer). Keeping the allocation outside
 * makes this module free of ESP-IDF headers, so it is fully testable on the
 * host.
 *
 * Concurrency: ONE writer (the HTTP task: rebuild() after a save) and ONE
 * reader (the DHCP task: contains()). The table is a double buffer — a rebuild
 * always fills the *inactive* half and then flips `active_` with a single
 * aligned 32-bit store, so the reader sees either the whole old table or the
 * whole new one; neither half is ever freed, so no reader can follow a dangling
 * pointer.
 */
class DhcpAllowedList {
public:
    /** @brief Maximum number of allowed computers (NVS blob budget, 1024 B). */
    static constexpr size_t kMaxEntries = 25;
    /** @brief Maximum length of a friendly name (longer names are truncated). */
    static constexpr size_t kMaxNameLen = 20;
    /** @brief Maximum serialized size of the list in NVS (bytes).
     *
     * One entry is `mac|name|enabled` = 17 + 1 + 20 + 1 + 1 = 40 bytes at its
     * longest, and serialize() joins entries with '\n' and adds none at the
     * end, so kMaxEntries need 25 * 40 + 24 = 1024 bytes — the budget exactly,
     * while 26 would need 1065. Keep the two constants in step: the limit is
     * derived from this arithmetic, not chosen. */
    static constexpr size_t kMaxBytes = 1024;
    /** @brief Hash table slots — a power of two, >= 2x kMaxEntries. */
    static constexpr size_t kSlots = 64;

    /**
     * @brief Minimal view of a static binding used by the policy check.
     *
     * The DHCP server keeps its own (richer) binding table; only the MAC and
     * the per-binding "enabled" flag matter for the allow-list policy.
     */
    struct StaticRef {
        uint8_t mac[6];
        bool enabled;
    };

    // ─── Codec (NVS text blob / settings export) ─────────────

    /**
     * @brief Serialize the list to the NVS text format: "mac|name|enabled"
     *        per line ('enabled' is "1"/"0").
     *
     * Invalid MACs are skipped, names are sanitized (separators replaced,
     * truncated) and at most kMaxEntries entries are written. The older
     * two-field line ("mac|name") is still read by parse() as enabled.
     */
    static std::string serialize(const std::vector<AllowedComputer>& list);

    /** @brief Size serialize() would produce for this list (bytes). */
    static size_t serializedBytes(const std::vector<AllowedComputer>& list);

    /** @brief Parse the NVS text format; invalid lines are skipped. */
    static std::vector<AllowedComputer> parse(const std::string& text);

    /**
     * @brief Normalize a MAC address to "24:0a:c4:01:23:45" (lower case).
     * @return "" when the text is not a MAC address.
     */
    static std::string normalizeMac(const std::string& mac);

    /**
     * @brief Parse a MAC address from text into 6 bytes.
     *
     * Accepts ":" / "-" / "." separated and separator-free forms, in any case.
     * @return false when the text does not contain exactly 12 hex digits.
     */
    static bool parseMac(const std::string& mac, uint8_t out[6]);

    /** @brief Format 6 bytes as "24:0a:c4:01:23:45" (lower case). */
    static std::string formatMac(const uint8_t mac[6]);

    // ─── Hash table ──────────────────────────────────────────

    /** @brief Bytes the owner must supply to init() (both buffers). */
    static size_t storageBytes();

    /**
     * @brief Bind the (owner-allocated) storage and clear both buffers.
     *
     * Must not be called while the DHCP task may be running: use rebuild()
     * for every later update.
     */
    bool init(void* storage);

    /**
     * @brief Rebuild the table from a parsed list and flip it active.
     *
     * Only ENABLED entries are inserted; a disabled one is not an error and is
     * not reported through `skipped`.
     *
     * @param list     Entries to insert (invalid MACs and duplicates dropped).
     * @param skipped  Output: enabled entries that could not be inserted.
     * @return false when the storage was never bound (no PSRAM).
     */
    bool rebuild(const std::vector<AllowedComputer>& list, size_t* skipped = nullptr);

    /** @brief True when this MAC is in the active table. */
    bool contains(const uint8_t mac[6]) const;

    /** @brief Number of MACs in the active table (enabled entries only). */
    size_t count() const { return count_; }

    /** @brief True when the storage is bound and the table is usable. */
    bool available() const { return slots_[0] != nullptr; }

    // ─── Policy ──────────────────────────────────────────────

    /**
     * @brief May this client be given a dynamic address?
     *
     * @param mac        Client MAC (6 bytes as they arrive on the wire).
     * @param allowOnly  The DHCP -> General switch.
     * @param list       Allow-list (must be available() to be consulted).
     * @param bindings   Static bindings; an ENABLED one counts as allowed.
     *
     * With allowOnly == false the list is ignored. With allowOnly == true and an
     * unusable table (PSRAM allocation failed) the check FAILS OPEN — refusing
     * every client because of a memory failure would take the whole LAN off the
     * air, which is worse than serving an address to a stranger.
     */
    static bool isClientAllowed(const uint8_t mac[6], bool allowOnly,
                                const DhcpAllowedList& list,
                                const std::vector<StaticRef>& bindings);

private:
    struct Slot {
        uint8_t mac[6];
        uint8_t used;      // 0 = never written (ends a probe chain)
        uint8_t reserved;  // padding: one slot is exactly 8 bytes
    };

    static uint32_t hashMac(const uint8_t mac[6]);
    /** @brief First empty slot in the probe chain, or nullptr when full. */
    static Slot* freeSlot(Slot* table, const uint8_t mac[6]);
    static void clearTable(Slot* table);
    static std::string sanitizeName(const std::string& name);

    Slot* slots_[2] = { nullptr, nullptr };
    // Flipped by rebuild() (one store) and read by contains() — the double
    // buffer above is what makes the lock-free read safe.
    volatile int active_ = 0;
    size_t count_ = 0;
};

} // namespace dhcp
} // namespace dhcp

#endif // DHCP_DHCP_DHCPALLOWEDLIST_H
