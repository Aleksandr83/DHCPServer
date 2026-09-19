#include "RouteTable.h"

namespace dhcp {
namespace web {

namespace {

/** @brief Exact URI comparison (a prefix is a different URI: /api/status2). */
bool sameUri(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr) return false;   // two holes are not a repeat
    for (size_t i = 0; ; i++) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
}

} // namespace

bool RouteTable::isValid(const WebRoute& route)
{
    // An empty URI can never match a request, and a null handler would take the
    // server down the moment somebody asked for it.
    return route.uri != nullptr && route.uri[0] != '\0' && route.handler != nullptr;
}

bool RouteTable::repeatsEarlier(const WebRoute* routes, size_t index)
{
    if (!routes || index == 0) return false;

    const WebRoute& entry = routes[index];
    for (size_t j = 0; j < index; j++) {
        if (routes[j].method != entry.method) continue;   // another method: another route
        if (sameUri(routes[j].uri, entry.uri)) return true;
    }
    return false;
}

} // namespace web
} // namespace dhcp
