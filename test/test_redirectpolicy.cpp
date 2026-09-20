/**
 * @file test_redirectpolicy.cpp
 * @brief Unit tests for the plain-HTTP → HTTPS redirect rules (stage 167).
 *
 * The gate that applies these rules lives in WebServer.cpp and needs ESP-IDF;
 * the rules themselves do not, and they are the part worth running: the address
 * a redirect sends a browser to is built from a header the client sent, and the
 * same text is then written into a response header and into an HTML attribute.
 * A name taken over unchecked is how a client ends up choosing where the device
 * sends it — so the pieces that decide (the status line, the address, the escape)
 * are pure functions, and this is where they are checked.
 *
 * There is nothing to compare against in the past: before stage 167 the plain
 * server had no gate at all, so this test does not compile against `HEAD` — the
 * header it includes did not exist there.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_redirectpolicy.cpp host_main.cpp -o test_redirectpolicy
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>
#include <string_view>

#include "../src/web/RedirectPolicy.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using dhcp::web::redirect::htmlEscaped;
using dhcp::web::redirect::kStatusMovedPermanently;
using dhcp::web::redirect::kStatusPermanentRedirect;
using dhcp::web::redirect::needed;
using dhcp::web::redirect::statusLineForMethod;
using dhcp::web::redirect::targetFrom;

namespace {

// Any date far enough away / behind to make a test's intent obvious.
constexpr int64_t kNow = 1700000000;             // 2023-11-14
constexpr int64_t kFuture = 2000000000;          // 2033-05-18
constexpr int64_t kPast = 1000000000;            // 2001-09-09
constexpr uint16_t kTlsPort = 443;

/** A pair that is not being served is never a reason to redirect. */
int test_no_redirect_without_a_listener()
{
    TEST_ASSERT_FALSE(needed(false, kFuture, kNow));
    return 0;
}

/** A pair that was never judged usable carries no date, and no date means no redirect. */
int test_no_redirect_without_a_deadline()
{
    TEST_ASSERT_FALSE(needed(true, 0, kNow));
    TEST_ASSERT_FALSE(needed(true, -1, kNow));
    return 0;
}

/** A listener plus an unexpired pair is the whole condition. */
int test_redirect_while_the_pair_is_valid()
{
    TEST_ASSERT_TRUE(needed(true, kFuture, kNow));
    return 0;
}

/** An expired pair turns the redirect off by itself, at the second it expires. */
int test_redirect_stops_when_the_pair_expires()
{
    TEST_ASSERT_FALSE(needed(true, kPast, kNow));
    TEST_ASSERT_FALSE(needed(true, kNow, kNow));    // the last second is not valid
    TEST_ASSERT_TRUE(needed(true, kNow + 1, kNow));
    return 0;
}

/** A GET repeats itself over TLS; every other method has to arrive as itself. */
int test_status_line_follows_the_method()
{
    TEST_ASSERT_TRUE(string(statusLineForMethod("GET")) == kStatusMovedPermanently);
    TEST_ASSERT_TRUE(string(statusLineForMethod("HEAD")) == kStatusMovedPermanently);
    TEST_ASSERT_TRUE(string(statusLineForMethod("POST")) == kStatusPermanentRedirect);
    TEST_ASSERT_TRUE(string(statusLineForMethod("PUT")) == kStatusPermanentRedirect);
    TEST_ASSERT_TRUE(string(statusLineForMethod("DELETE")) == kStatusPermanentRedirect);
    TEST_ASSERT_TRUE(string(statusLineForMethod("PATCH")) == kStatusPermanentRedirect);
    return 0;
}

/** The name the client used is the name it is sent to again. */
int test_target_keeps_the_name_the_client_used()
{
    TEST_ASSERT_STREQ(targetFrom("dhcpserver.local", "/pages/certs.html", kTlsPort),
                      "https://dhcpserver.local/pages/certs.html");
    TEST_ASSERT_STREQ(targetFrom("dns.lo", "/", kTlsPort), "https://dns.lo/");
    return 0;
}

/** A port in `Host` belongs to the port the client reached, not to the TLS one. */
int test_target_replaces_the_plain_port()
{
    TEST_ASSERT_STREQ(targetFrom("dns.lo:80", "/", kTlsPort), "https://dns.lo/");
    TEST_ASSERT_STREQ(targetFrom("192.168.1.10:8080", "/api/status", kTlsPort),
                      "https://192.168.1.10/api/status");
    return 0;
}

/** An IPv6 literal is a host in a URL only with its brackets. */
int test_target_keeps_ipv6_brackets()
{
    TEST_ASSERT_STREQ(targetFrom("[fe80::1]", "/", kTlsPort), "https://[fe80::1]/");
    TEST_ASSERT_STREQ(targetFrom("[fe80::1]:80", "/", kTlsPort), "https://[fe80::1]/");
    return 0;
}

/** A TLS port other than 443 has to be named in the address. */
int test_target_names_a_nonstandard_tls_port()
{
    TEST_ASSERT_STREQ(targetFrom("dns.lo", "/", 8443), "https://dns.lo:8443/");
    return 0;
}

/** No usable name, no address: the caller then answers the request as always. */
int test_target_needs_a_usable_name()
{
    TEST_ASSERT_STREQ(targetFrom("", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom(":80", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom("   ", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom("\r\n", "/", kTlsPort), "");
    // An IPv6 literal without its closing bracket is not an address.
    TEST_ASSERT_STREQ(targetFrom("[fe80::1", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom("[", "/", kTlsPort), "");
    return 0;
}

/**
 * A name is taken whole or not at all: cleaning it character by character would
 * turn something nobody asked for into a name that answers.
 */
int test_target_takes_the_name_whole()
{
    TEST_ASSERT_STREQ(targetFrom("dns.lo\r\nX-Ignored: 1", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom("d<ns>.lo", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom("dns.lo\"", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom("dns.lo/evil", "/", kTlsPort), "");
    TEST_ASSERT_STREQ(targetFrom("dns.lo evil", "/", kTlsPort), "");
    return 0;
}

/** Space around a header value is the header's; the name inside it is still used. */
int test_target_uses_the_name_inside_the_header()
{
    TEST_ASSERT_STREQ(targetFrom(" dns.lo ", "/", kTlsPort), "https://dns.lo/");
    TEST_ASSERT_STREQ(targetFrom("my-dhcp_server.local", "/", kTlsPort),
                      "https://my-dhcp_server.local/");
    return 0;
}

/** The path comes along — a redirect to the root would lose the page asked for. */
int test_target_keeps_the_path_and_query()
{
    TEST_ASSERT_STREQ(targetFrom("dns.lo", "/search?a=1&b=2", kTlsPort),
                      "https://dns.lo/search?a=1&b=2");
    // A request that named no path gets the root rather than no address at all.
    TEST_ASSERT_STREQ(targetFrom("dns.lo", "", kTlsPort), "https://dns.lo/");
    return 0;
}

/** The message under the redirect links to the address, so it is escaped. */
int test_html_escaping()
{
    TEST_ASSERT_STREQ(htmlEscaped("https://dns.lo/a?b=1&c=2"),
                      "https://dns.lo/a?b=1&amp;c=2");
    TEST_ASSERT_STREQ(htmlEscaped("<a href=\"x\">"), "&lt;a href=&quot;x&quot;&gt;");
    TEST_ASSERT_STREQ(htmlEscaped("plain"), "plain");
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running RedirectPolicy tests...\n");
    int failures = 0;

    failures += test_no_redirect_without_a_listener();
    failures += test_no_redirect_without_a_deadline();
    failures += test_redirect_while_the_pair_is_valid();
    failures += test_redirect_stops_when_the_pair_expires();
    failures += test_status_line_follows_the_method();
    failures += test_target_keeps_the_name_the_client_used();
    failures += test_target_replaces_the_plain_port();
    failures += test_target_keeps_ipv6_brackets();
    failures += test_target_names_a_nonstandard_tls_port();
    failures += test_target_needs_a_usable_name();
    failures += test_target_takes_the_name_whole();
    failures += test_target_uses_the_name_inside_the_header();
    failures += test_target_keeps_the_path_and_query();
    failures += test_html_escaping();

    if (failures == 0) {
        printf("All RedirectPolicy tests PASSED!\n");
    } else {
        printf("Some RedirectPolicy tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#else   // !DHCP_TEST_HOST

#include <cstdio>

extern "C" void app_main()
{
    printf("test_redirectpolicy is a host-only test (build with -DDHCP_TEST_HOST)"
           " - see the file header.\n");
}

#endif  // DHCP_TEST_HOST
