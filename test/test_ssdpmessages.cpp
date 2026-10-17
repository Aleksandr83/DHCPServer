/**
 * @file test_ssdpmessages.cpp
 * @brief Unit tests for the SSDP/UPnP messages the device sends (stage 220).
 *
 * SSDP is a text protocol: what a Windows machine sees is the bytes below. The
 * socket and the XML route need ESP-IDF, but the messages, the search-target
 * parsing, the UUID derivation and the device description do not — and a wrong
 * `USN` or a missing `ST` is exactly the kind of defect that only shows up as
 * "the device does not appear".
 *
 * There is nothing to compare against in the past: before this stage the header
 * did not exist, so this test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_ssdpmessages.cpp host_main.cpp -o test_ssdpmessages
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>
#include <vector>

#include "../src/net/SsdpMessages.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)
#define TEST_ASSERT_CONTAINS(hay, needle) do { \
        const string _h = (hay); const string _n = (needle); \
        if (_h.find(_n) == string::npos) { printf("FAIL: %s:%d: \"%s\" not in\n%s\n", __FILE__, __LINE__, \
                                                 _n.c_str(), _h.c_str()); return 1; } \
    } while(0)

using namespace dhcp::net::ssdp;

namespace {

const uint8_t kMacA[6] = { 0x24, 0x0A, 0xC4, 0x01, 0x23, 0x45 };
const uint8_t kMacB[6] = { 0x24, 0x0A, 0xC4, 0x01, 0x23, 0x46 };

/** A UUID is stable for one device and different for another, with no storage. */
int test_the_uuid_comes_from_the_mac()
{
    const string a = uuidFromMac(kMacA);
    const string b = uuidFromMac(kMacA);
    const string c = uuidFromMac(kMacB);
    TEST_ASSERT_STREQ(a, b);
    TEST_ASSERT_TRUE(a != c);
    TEST_ASSERT_TRUE(a.rfind("uuid:", 0) == 0);
    TEST_ASSERT_TRUE(a.size() == 5 + 36);
    // RFC 4122 shape: 8-4-4-4-12, version 5 in the third group, variant in the
    // fourth. A parser that ignores this still works, but Windows is not the only
    // reader of the UDN.
    TEST_ASSERT_TRUE(a[13] == '-' && a[18] == '-' && a[23] == '-' && a[28] == '-');
    TEST_ASSERT_TRUE(a[19] == '5');
    TEST_ASSERT_TRUE(a[24] == '8' || a[24] == '9' || a[24] == 'a' || a[24] == 'b');
    return 0;
}

/** The description page is what Windows fetches; the address has to be right. */
int test_the_location()
{
    TEST_ASSERT_STREQ(locationFromIp("192.168.1.201"),
                      "http://192.168.1.201:80/upnp/device.xml");
    return 0;
}

/** The three advertisements one device sends (root device, itself, its type). */
int test_the_targets()
{
    const vector<string> t = targets("uuid:abc");
    TEST_ASSERT_TRUE(t.size() == 3);
    TEST_ASSERT_STREQ(t[0], "upnp:rootdevice");
    TEST_ASSERT_STREQ(t[1], "uuid:abc");
    TEST_ASSERT_STREQ(t[2], kDeviceType);
    return 0;
}

/** An M-SEARCH answer carries the target asked for and one USN per target. */
int test_the_search_response()
{
    const string r = searchResponse(kDeviceType, "uuid:abc",
                                    "http://192.168.1.201:80/upnp/device.xml",
                                    "ESP32-P4/1.0 UPnP/1.1 DHCPServer/1.0");
    TEST_ASSERT_CONTAINS(r, "HTTP/1.1 200 OK\r\n");
    TEST_ASSERT_CONTAINS(r, "CACHE-CONTROL: max-age=1800\r\n");
    TEST_ASSERT_CONTAINS(r, "EXT:\r\n");
    TEST_ASSERT_CONTAINS(r, "LOCATION: http://192.168.1.201:80/upnp/device.xml\r\n");
    TEST_ASSERT_CONTAINS(r, "ST: " + string(kDeviceType) + "\r\n");
    TEST_ASSERT_CONTAINS(r, "USN: uuid:abc::" + string(kDeviceType) + "\r\n");
    // Headers end with an empty line and nothing hangs after it.
    TEST_ASSERT_TRUE(r.size() >= 4 && r.compare(r.size() - 4, 4, "\r\n\r\n") == 0);
    return 0;
}

/** `alive` describes where to fetch; `byebye` only says the device is leaving. */
int test_the_notifications()
{
    const string alive = notifyMessage("ssdp:alive", kDeviceType, "uuid:abc",
                                       "http://192.168.1.201:80/upnp/device.xml",
                                       "ESP32-P4/1.0 UPnP/1.1 DHCPServer/1.0");
    TEST_ASSERT_CONTAINS(alive, "NOTIFY * HTTP/1.1\r\n");
    TEST_ASSERT_CONTAINS(alive, "HOST: 239.255.255.250:1900\r\n");
    TEST_ASSERT_CONTAINS(alive, "NTS: ssdp:alive\r\n");
    TEST_ASSERT_CONTAINS(alive, "NT: " + string(kDeviceType) + "\r\n");
    TEST_ASSERT_CONTAINS(alive, "USN: uuid:abc::" + string(kDeviceType) + "\r\n");
    TEST_ASSERT_CONTAINS(alive, "LOCATION: http://192.168.1.201:80/upnp/device.xml\r\n");

    const string bye = notifyMessage("ssdp:byebye", kDeviceType, "uuid:abc", "", "");
    TEST_ASSERT_CONTAINS(bye, "NTS: ssdp:byebye\r\n");
    TEST_ASSERT_TRUE(bye.find("LOCATION:") == string::npos);
    return 0;
}

/** Only a real M-SEARCH is answered; the target is read case-insensitively. */
int test_the_search_parsing()
{
    const string search =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "ST: urn:schemas-upnp-org:device:Basic:1\r\n"
        "\r\n";
    TEST_ASSERT_TRUE(ssdpIsSearchRequest(search));
    TEST_ASSERT_STREQ(ssdpSearchTarget(search), "urn:schemas-upnp-org:device:Basic:1");

    const string lower = "m-search * http/1.1\r\nst: ssdp:all\r\n\r\n";
    TEST_ASSERT_TRUE(ssdpIsSearchRequest(lower));
    TEST_ASSERT_STREQ(ssdpSearchTarget(lower), "ssdp:all");

    TEST_ASSERT_FALSE(ssdpIsSearchRequest("NOTIFY * HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_FALSE(ssdpIsSearchRequest("GET / HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_STREQ(ssdpSearchTarget("M-SEARCH * HTTP/1.1\r\n\r\n"), "");
    return 0;
}

/** `ssdp:all` asks for everything; any other target is answered only when it is
 *  one of ours. */
int test_which_targets_a_search_asks_for()
{
    const vector<string> all = targetsForSearch("ssdp:all", "uuid:abc");
    TEST_ASSERT_TRUE(all.size() == 3);

    const vector<string> one = targetsForSearch(kDeviceType, "uuid:abc");
    TEST_ASSERT_TRUE(one.size() == 1);
    TEST_ASSERT_STREQ(one[0], kDeviceType);

    const vector<string> none = targetsForSearch("urn:schemas-upnp-org:device:MediaServer:1", "uuid:abc");
    TEST_ASSERT_TRUE(none.empty());
    return 0;
}

/** The description Windows fetches: the device's own name, its type and its UDN. */
int test_the_device_description()
{
    const string xml = deviceXml("DhcpServer", "DHCPServer",
                                 "01.02.057.00.26.10.RU", "uuid:abc", "/");
    TEST_ASSERT_CONTAINS(xml, "<?xml version=\"1.0\"?>");
    TEST_ASSERT_CONTAINS(xml, "<deviceType>" + string(kDeviceType) + "</deviceType>");
    TEST_ASSERT_CONTAINS(xml, "<friendlyName>DhcpServer</friendlyName>");
    TEST_ASSERT_CONTAINS(xml, "<modelName>DHCPServer</modelName>");
    TEST_ASSERT_CONTAINS(xml, "<modelNumber>01.02.057.00.26.10.RU</modelNumber>");
    TEST_ASSERT_CONTAINS(xml, "<UDN>uuid:abc</UDN>");
    TEST_ASSERT_CONTAINS(xml, "<presentationURL>/</presentationURL>");
    // A name with markup must not break the document (the host-name rule forbids
    // these today, but the builder is the last line of defence).
    const string escaped = deviceXml("a&b<c>\"", "DHCPServer", "1", "uuid:abc", "/");
    TEST_ASSERT_CONTAINS(escaped, "<friendlyName>a&amp;b&lt;c&gt;&quot;</friendlyName>");
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running SsdpMessages tests...\n");
    int failures = 0;

    failures += test_the_uuid_comes_from_the_mac();
    failures += test_the_location();
    failures += test_the_targets();
    failures += test_the_search_response();
    failures += test_the_notifications();
    failures += test_the_search_parsing();
    failures += test_which_targets_a_search_asks_for();
    failures += test_the_device_description();

    if (failures == 0) {
        printf("All SsdpMessages tests PASSED!\n");
    } else {
        printf("Some SsdpMessages tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
