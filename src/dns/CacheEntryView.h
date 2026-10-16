/**
 * @file CacheEntryView.h
 * @brief What the cache view page asks the PSRAM cache for, and what one record
 *        of it looks like (stage 209).
 *
 * The viewer shows thousands of records, so the two decisions that keep it cheap
 * are here rather than in the walk: **which** record a query matches (a substring
 * of a lower-case name and, optionally, one of the two types the cache holds) and
 * **how many** records a page may hold. Nothing in this header touches the arena
 * or ESP-IDF, so a host test can hand it the names it wants to ask about.
 *
 * The name filter is lower-cased **once**, by the caller, because the cache stores
 * names lower-cased and the walk visits every record in the pool. Lowering inside
 * the comparison would copy a string per record for no gain — and a filter compared
 * against a name that is already lower-case is exactly what a substring search
 * needs.
 */
#ifndef DHCP_DNS_CACHEENTRYVIEW_H
#define DHCP_DNS_CACHEENTRYVIEW_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dhcp {
namespace dns {

/** @brief The two record types the PSRAM cache stores (RFC 1035 numbers). */
constexpr uint16_t kCacheTypeA = 1;
constexpr uint16_t kCacheTypeAAAA = 28;

/** @brief The addresses one cache record can hold (`Node` in the cache). */
constexpr size_t kCacheViewMaxA = 16;
constexpr size_t kCacheViewMaxAAAA = 8;

/** @brief Records the viewer shows at once, and the ceiling it will not pass. */
constexpr size_t kCacheViewPageSize = 100;
constexpr size_t kCacheViewMaxPageSize = 200;

/**
 * @brief One record of the cache, as the page shows it.
 *
 * `remaining` and `expired` are the same arithmetic seen from two sides: a record
 * whose TTL has run out is still in the pool until the purge reaches it (and, with
 * "Ignore TTL" on, is served on purpose), so it is shown with no time left and a
 * `expired` flag rather than hidden — a viewer that hides records would disagree
 * with the counters right above it on the page it was opened from.
 */
struct CacheEntryView {
    std::string name;
    uint16_t qtype = 0;
    uint8_t a4Count = 0;
    uint32_t a4[kCacheViewMaxA] = {};      // IPv4 addresses, wire order
    uint8_t a6Count = 0;
    uint8_t a6[kCacheViewMaxAAAA][16] = {};  // IPv6 addresses, wire order
    uint32_t ttl = 0;        // the TTL the answer carried, seconds
    uint32_t remaining = 0;  // seconds left (0 when nothing is left)
    uint32_t age = 0;        // seconds since the record was stored
    uint32_t uses = 0;       // times the record has been served
    bool expired = false;    // held past its TTL
};

/** @brief What the operator asked the viewer for. */
struct CacheEntryQuery {
    std::string name;      // lower-case substring; empty = every name
    uint16_t qtype = 0;    // 0 = both types, otherwise A or AAAA
    size_t offset = 0;     // matching records to skip
    size_t limit = kCacheViewPageSize;
};

/** @brief One page of records, plus the numbers a pager is drawn from. */
struct CacheEntryPage {
    bool available = false;  // the cache exists (PSRAM was found)
    size_t total = 0;        // records matching the filter
    size_t offset = 0;
    size_t limit = 0;
    size_t scanned = 0;      // records the walk looked at
    std::vector<CacheEntryView> items;
};

/** @brief ASCII-lower-case text; a byte above ASCII is left alone. */
inline std::string cacheViewLowerName(const std::string& text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/**
 * @brief The type a filter asks for.
 *
 * False means "not one of the two types": an empty filter (the page's "both") and
 * a word that is not a type both come back that way, and the caller decides what
 * to do with it. Reporting a match with a zero type would make an unknown filter
 * silently mean "A", which is a lie about which records are missing from the list.
 */
inline bool cacheEntryTypeFromText(const std::string& text, uint16_t& out)
{
    size_t first = 0;
    size_t last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t')) ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
    const std::string trimmed = text.substr(first, last - first);

    if (trimmed.empty()) return false;
    const std::string lower = cacheViewLowerName(trimmed);
    if (lower == "a") {
        out = kCacheTypeA;
        return true;
    }
    if (lower == "aaaa") {
        out = kCacheTypeAAAA;
        return true;
    }
    return false;
}

/** @brief The name of a stored type for the page ("A", "AAAA", "type 33"). */
inline std::string cacheEntryTypeText(uint16_t qtype)
{
    if (qtype == kCacheTypeA) return "A";
    if (qtype == kCacheTypeAAAA) return "AAAA";
    return "type " + std::to_string(qtype);
}

/**
 * @brief True when a record is what @p query asks for.
 *
 * @param[in] name  The record's name **as the cache holds it** (lower-cased);
 *                  a null pointer matches only an empty filter.
 * @param[in] qtype The record's type.
 * @param[in] query The filter, whose name is already lower-cased.
 */
inline bool cacheEntryMatches(const char* name, uint16_t qtype,
                              const CacheEntryQuery& query)
{
    if (query.qtype != 0 && qtype != query.qtype) return false;
    if (query.name.empty()) return true;
    if (name == nullptr) return false;
    return std::strstr(name, query.name.c_str()) != nullptr;
}

/** @brief A page size the cache will honour: 0 means "unset", and there is a ceiling. */
inline size_t cacheViewClampLimit(size_t limit)
{
    if (limit == 0) return kCacheViewPageSize;
    return limit > kCacheViewMaxPageSize ? kCacheViewMaxPageSize : limit;
}

/** @brief How many pages @p total records make; an empty cache is one empty page. */
inline size_t cacheViewPageCount(size_t total, size_t limit)
{
    if (limit == 0) return 1;
    const size_t pages = (total + limit - 1) / limit;
    return pages == 0 ? 1 : pages;
}

/** @brief The 1-based page an @p offset lands on. */
inline size_t cacheViewPageNumber(size_t offset, size_t limit)
{
    if (limit == 0) return 1;
    return offset / limit + 1;
}

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_CACHEENTRYVIEW_H
