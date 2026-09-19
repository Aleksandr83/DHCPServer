#ifndef DHCP_WEB_ROUTETABLE_H
#define DHCP_WEB_ROUTETABLE_H

#include <cstddef>
#include <cstdint>

// The HTTP request type, declared but never defined here: this header is also
// compiled on a host (see test/test_routetable.cpp), so it must not pull in
// ESP-IDF. `esp_err_t` is `int` in ESP-IDF, which is what makes RouteHandler
// below the exact type `httpd_uri_t::handler` wants — the route table needs no
// casts anywhere.
struct httpd_req;

namespace dhcp {
namespace web {

/** @brief HTTP method of one route. */
enum class RouteMethod : uint8_t { Get, Post };

/** @brief The handler signature the HTTP server expects (`esp_err_t` = `int`). */
using RouteHandler = int (*)(httpd_req*);

/** @brief One route: where it lives, which method it answers and who handles it. */
struct WebRoute {
    const char* uri;
    RouteMethod method;
    RouteHandler handler;
};

/**
 * @brief Checks the web server's route table, and the size of the HTTP server's
 *        handler table derived from it.
 *
 * The route table itself lives in `WebServer.cpp`, next to the handlers it
 * names; the rules about it live here so they can be run on a host. They exist
 * because a table is data, and two kinds of mistake in it are silent by nature:
 * a route with no URI or no handler can only ever answer 404, and a repeated
 * pair of (URI, method) shadows the earlier route without saying anything.
 *
 * The other half of the class is @ref slotsFor, and it closes a real defect: the
 * HTTP server hands out one handler slot per registered URI, and until stage 136
 * the limit was the constant 81 while the table held **82** routes — so the last
 * route, `/pages/version.html`, was registered nowhere and that page answered
 * 404 after the update. Deriving the limit from the table means the two numbers
 * are one number and cannot disagree again.
 */
class RouteTable {
public:
    /** @brief No index / no entry (kept for callers that scan indices). */
    static constexpr size_t kNone = static_cast<size_t>(-1);

    /**
     * @brief Is this entry registrable at all?
     * @return false when the URI is missing or empty, or the handler is null.
     */
    static bool isValid(const WebRoute& route);

    /**
     * @brief Does an earlier entry already use this (URI, method)?
     *
     * The same URI with *another* method is not a repeat: `/api/time/settings`
     * answers both GET and POST, and those are two routes.
     *
     * @param routes Table the entry belongs to.
     * @param index  Index of the entry to check; 0 has nothing before it.
     */
    static bool repeatsEarlier(const WebRoute* routes, size_t index);

    /**
     * @brief Handler slots the HTTP server needs for `count` routes.
     *
     * Exactly one per route: a limit larger than the table is slack nobody
     * checks, and a limit smaller than the table is the stage-136 defect.
     */
    static constexpr size_t slotsFor(size_t count) { return count; }
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_ROUTETABLE_H
