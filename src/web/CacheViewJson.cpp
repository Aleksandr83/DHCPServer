#include "CacheViewJson.h"

#include "JsonWriter.h"
#include "../core/IpText.h"

using namespace std;

namespace dhcp {
namespace web {

string cacheEntryJson(const dns::CacheEntryView& entry)
{
    string json;
    json.reserve(256 + entry.name.size());
    json += "{\"name\":\"" + JsonWriter::escape(entry.name) + "\",\"type\":\"" +
            dns::cacheEntryTypeText(entry.qtype) + "\",\"a4\":[";
    for (uint8_t i = 0; i < entry.a4Count && i < dns::kCacheViewMaxA; ++i) {
        if (i) json += ",";
        json += "\"" + core::ip4Text(reinterpret_cast<const uint8_t*>(&entry.a4[i])) + "\"";
    }
    json += "],\"a6\":[";
    for (uint8_t i = 0; i < entry.a6Count && i < dns::kCacheViewMaxAAAA; ++i) {
        if (i) json += ",";
        json += "\"" + core::ip6Text(entry.a6[i]) + "\"";
    }
    json += "],\"ttl\":" + std::to_string(entry.ttl) +
            ",\"remaining\":" + std::to_string(entry.remaining) +
            ",\"age\":" + std::to_string(entry.age) +
            ",\"uses\":" + std::to_string(entry.uses) +
            ",\"expired\":" + (entry.expired ? "true" : "false") + "}";
    return json;
}

string cacheEntryPageJson(const dns::CacheEntryPage& page)
{
    string json;
    json.reserve(128 + page.items.size() * 192);
    json += string("{\"available\":") + (page.available ? "true" : "false");
    json += ",\"total\":" + std::to_string(page.total);
    json += ",\"offset\":" + std::to_string(page.offset);
    json += ",\"limit\":" + std::to_string(page.limit);
    json += ",\"page\":" + std::to_string(dns::cacheViewPageNumber(page.offset, page.limit));
    json += ",\"pages\":" + std::to_string(dns::cacheViewPageCount(page.total, page.limit));
    json += ",\"scanned\":" + std::to_string(page.scanned);
    json += ",\"items\":[";
    for (size_t i = 0; i < page.items.size(); ++i) {
        if (i) json += ",";
        json += cacheEntryJson(page.items[i]);
    }
    json += "]}";
    return json;
}

} // namespace web
} // namespace dhcp
