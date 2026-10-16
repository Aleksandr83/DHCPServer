/**
 * @file CacheViewJson.h
 * @brief The answer the cache view page parses (stage 209).
 *
 * One JSON document per request: the numbers the pager is drawn from, and the
 * records of the page that was asked for. It is built here rather than inside the
 * request handler because a page of a hundred records is a promise to the browser
 * — `a4`/`a6` are arrays, a name is escaped, `page`/`pages` cannot disagree with
 * `offset`/`limit`/`total` — and a host test can hold it to that promise without a
 * device (see `test/test_cacheviewformat.cpp`).
 *
 * The addresses are printed from the **bytes** the cache holds (wire order, exactly
 * what `inet_pton` wrote), so `core::IpText.h` is the one place that decides how an
 * address is spelled.
 */
#ifndef DHCP_WEB_CACHEVIEWJSON_H
#define DHCP_WEB_CACHEVIEWJSON_H

#include <string>

#include "../dns/CacheEntryView.h"

namespace dhcp {
namespace web {

/** @brief One record as a JSON object. */
std::string cacheEntryJson(const dns::CacheEntryView& entry);

/**
 * @brief The whole answer: where this page is, how many there are, and its records.
 *
 * `available` is the field a device without PSRAM answers with: the page then says
 * so in words instead of drawing an empty cache, which is the difference between
 * "the cache is empty" and "this device has no cache".
 */
std::string cacheEntryPageJson(const dns::CacheEntryPage& page);

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_CACHEVIEWJSON_H
