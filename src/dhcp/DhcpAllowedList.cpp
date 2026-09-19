#include "DhcpAllowedList.h"

#include <cstdio>
#include <cstring>
#include <cctype>

namespace dhcp {
namespace dhcp {

// ─── Text helpers ───────────────────────────────────

static std::string trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// Rule 39: the FNV-1a hash of a client name — the standard offset basis and
// the standard prime, one multiply and one xor per character.
constexpr uint32_t kFnvOffsetBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

// Rule 39: "xx:xx:xx:xx:xx:xx" plus the NUL, the room a MAC address needs.
constexpr size_t kMacTextLen = 18;

static int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ─── MAC address codec ──────────────────────────────

bool DhcpAllowedList::parseMac(const std::string& mac, uint8_t out[6])
{
    uint8_t digits[12];
    size_t n = 0;
    for (char c : mac) {
        if (c == ':' || c == '-' || c == '.') continue;
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        int v = hexNibble(static_cast<char>(c));
        if (v < 0 || n >= 12) return false;
        digits[n++] = static_cast<uint8_t>(v);
    }
    if (n != 12) return false;
    for (size_t i = 0; i < 6; i++) {
        out[i] = static_cast<uint8_t>((digits[i * 2] << 4) | digits[i * 2 + 1]);
    }
    return true;
}

std::string DhcpAllowedList::formatMac(const uint8_t mac[6])
{
    char buf[kMacTextLen];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

std::string DhcpAllowedList::normalizeMac(const std::string& mac)
{
    uint8_t bytes[6];
    if (!parseMac(mac, bytes)) return std::string();
    return formatMac(bytes);
}

// ─── List codec ─────────────────────────────────────

std::string DhcpAllowedList::sanitizeName(const std::string& name)
{
    std::string out = trim(name);
    for (char& c : out) {
        // '|' separates the fields and '\n' the entries — a name must not be
        // able to break the format it is stored in.
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    }
    if (out.size() > kMaxNameLen) out.resize(kMaxNameLen);
    return out;
}

std::string DhcpAllowedList::serialize(const std::vector<AllowedComputer>& list)
{
    std::string out;
    size_t written = 0;
    for (const auto& entry : list) {
        if (written >= kMaxEntries) break;
        uint8_t bytes[6];
        if (!parseMac(entry.mac, bytes)) continue;
        if (!out.empty()) out += '\n';
        out += formatMac(bytes);
        out += '|';
        out += sanitizeName(entry.name);
        out += '|';
        out += (entry.enabled ? "1" : "0");
        written++;
    }
    return out;
}

size_t DhcpAllowedList::serializedBytes(const std::vector<AllowedComputer>& list)
{
    return serialize(list).size();
}

std::vector<AllowedComputer> DhcpAllowedList::parse(const std::string& text)
{
    std::vector<AllowedComputer> out;
    size_t pos = 0;
    while (pos <= text.size() && out.size() < kMaxEntries) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, (eol == std::string::npos)
                                              ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;

        line = trim(line);
        if (line.empty()) continue;

        AllowedComputer entry;
        size_t sep = line.find('|');
        if (sep == std::string::npos) {
            entry.mac = normalizeMac(line);
        } else {
            entry.mac = normalizeMac(line.substr(0, sep));
            std::string rest = line.substr(sep + 1);
            size_t sep2 = rest.find('|');
            if (sep2 == std::string::npos) {
                entry.name = sanitizeName(rest);
            } else {
                entry.name = sanitizeName(rest.substr(0, sep2));
                // "mac|name|enabled"; without the third field (an entry written
                // by an older firmware or hand-edited) the entry is enabled.
                entry.enabled = (trim(rest.substr(sep2 + 1)) != "0");
            }
        }
        if (entry.mac.empty()) continue;  // damaged line — drop it, keep the rest
        out.push_back(entry);
    }
    return out;
}

// ─── Hash table ─────────────────────────────────────

size_t DhcpAllowedList::storageBytes()
{
    return 2 * kSlots * sizeof(Slot);
}

uint32_t DhcpAllowedList::hashMac(const uint8_t mac[6])
{
    // FNV-1a over the 6 address bytes.
    uint32_t h = kFnvOffsetBasis;
    for (size_t i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= kFnvPrime;
    }
    return h;
}

void DhcpAllowedList::clearTable(Slot* table)
{
    std::memset(table, 0, kSlots * sizeof(Slot));
}

DhcpAllowedList::Slot* DhcpAllowedList::freeSlot(Slot* table, const uint8_t mac[6])
{
    size_t idx = hashMac(mac) & (kSlots - 1);
    for (size_t probe = 0; probe < kSlots; probe++) {
        Slot* slot = &table[idx];
        if (!slot->used) return slot;                          // insert here
        if (std::memcmp(slot->mac, mac, 6) == 0) return nullptr; // duplicate
        idx = (idx + 1) & (kSlots - 1);
    }
    return nullptr;  // table full (cannot happen at kMaxEntries < kSlots/2)
}

bool DhcpAllowedList::init(void* storage)
{
    if (!storage) return false;
    Slot* base = static_cast<Slot*>(storage);
    slots_[0] = base;
    slots_[1] = base + kSlots;
    clearTable(slots_[0]);
    clearTable(slots_[1]);
    count_ = 0;
    active_ = 0;
    return true;
}

bool DhcpAllowedList::rebuild(const std::vector<AllowedComputer>& list, size_t* skipped)
{
    if (!available()) {
        if (skipped) *skipped = list.size();
        return false;
    }

    const int next = 1 - active_;
    Slot* dest = slots_[next];
    clearTable(dest);

    size_t inserted = 0;
    size_t dropped = 0;
    for (const auto& entry : list) {
        if (!entry.enabled) continue;   // switched off: kept in NVS, allows nothing
        uint8_t mac[6];
        if (!parseMac(entry.mac, mac)) { dropped++; continue; }
        if (inserted >= kMaxEntries) { dropped++; continue; }
        Slot* slot = freeSlot(dest, mac);
        if (!slot) { dropped++; continue; }
        std::memcpy(slot->mac, mac, 6);
        slot->used = 1;
        inserted++;
    }

    count_ = inserted;
    active_ = next;   // single store: the reader switches tables atomically
    if (skipped) *skipped = dropped;
    return true;
}

bool DhcpAllowedList::contains(const uint8_t mac[6]) const
{
    const Slot* table = slots_[active_];
    if (!table) return false;

    size_t idx = hashMac(mac) & (kSlots - 1);
    for (size_t probe = 0; probe < kSlots; probe++) {
        const Slot* slot = &table[idx];
        if (!slot->used) return false;                        // end of chain
        if (std::memcmp(slot->mac, mac, 6) == 0) return true;
        idx = (idx + 1) & (kSlots - 1);
    }
    return false;
}

// ─── Policy ─────────────────────────────────────────

bool DhcpAllowedList::isClientAllowed(const uint8_t mac[6], bool allowOnly,
                                     const DhcpAllowedList& list,
                                     const std::vector<StaticRef>& bindings)
{
    if (!allowOnly) return true;          // the list is ignored when switched off
    if (!list.available()) return true;   // no PSRAM — fail open, see the header

    if (list.contains(mac)) return true;
    for (const auto& ref : bindings) {
        if (ref.enabled && std::memcmp(ref.mac, mac, 6) == 0) return true;
    }
    return false;
}

} // namespace dhcp
} // namespace dhcp
