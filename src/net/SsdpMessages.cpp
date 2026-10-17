#include "SsdpMessages.h"

#include <cstdio>
#include <cstring>

using namespace std;

namespace dhcp {
namespace net {
namespace ssdp {

namespace {

/// FNV-1a, two different seeds: sixteen bytes from six, without a crypto
/// dependency and without keeping a UUID in NVS.
constexpr uint64_t kFnvOffsetA = 1469598103934665603ULL;
constexpr uint64_t kFnvOffsetB = 1099511628211ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fnv1a(const uint8_t mac[6], uint64_t seed, uint8_t salt)
{
    uint64_t hash = seed;
    for (int i = 0; i < 6; i++) {
        hash ^= static_cast<uint64_t>(mac[i] ^ salt);
        hash *= kFnvPrime;
    }
    return hash;
}

string usnFor(const string& uuid, const string& nt)
{
    // UPnP: the UUID target names itself, every other target is `udn::target`.
    return (nt == uuid) ? uuid : uuid + "::" + nt;
}

bool startsWithNoCase(const string& text, const char* prefix)
{
    const size_t n = strlen(prefix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        char p = prefix[i];
        if (p >= 'a' && p <= 'z') p = static_cast<char>(p - 'a' + 'A');
        if (c != p) return false;
    }
    return true;
}

string xmlEscaped(const string& text)
{
    string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

} // namespace

string uuidFromMac(const uint8_t mac[6])
{
    const uint64_t a = fnv1a(mac, kFnvOffsetA, 0x5A);
    const uint64_t b = fnv1a(mac, kFnvOffsetB, 0xA5);

    uint8_t bytes[16];
    for (int i = 0; i < 8; i++) {
        bytes[i] = static_cast<uint8_t>(a >> (56 - 8 * i));
        bytes[8 + i] = static_cast<uint8_t>(b >> (56 - 8 * i));
    }
    // RFC 4122 shape: version 5 (name-based) and the variant bits.
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x50);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    char buf[48];
    snprintf(buf, sizeof(buf),
             "uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
             bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

string locationFromIp(const string& ip)
{
    return "http://" + ip + ":80" + kDescriptionPath;
}

vector<string> targets(const string& uuid)
{
    return { kRootDevice, uuid, kDeviceType };
}

vector<string> targetsForSearch(const string& searchTarget, const string& uuid)
{
    if (startsWithNoCase(searchTarget, "ssdp:all")) return targets(uuid);
    for (const string& target : targets(uuid)) {
        if (searchTarget == target) return { target };
    }
    return {};
}

string searchResponse(const string& st, const string& uuid,
                      const string& location, const string& server)
{
    string msg = "HTTP/1.1 200 OK\r\n";
    msg += "CACHE-CONTROL: max-age=" + to_string(kMaxAgeSec) + "\r\n";
    msg += "EXT:\r\n";
    msg += "LOCATION: " + location + "\r\n";
    msg += "SERVER: " + server + "\r\n";
    msg += "ST: " + st + "\r\n";
    msg += "USN: " + usnFor(uuid, st) + "\r\n";
    msg += "\r\n";
    return msg;
}

string notifyMessage(const string& nts, const string& nt, const string& uuid,
                     const string& location, const string& server)
{
    const bool alive = (nts == "ssdp:alive");
    string msg = "NOTIFY * HTTP/1.1\r\n";
    msg += string("HOST: ") + kMulticastIp + ":" + to_string(kPort) + "\r\n";
    if (alive) {
        msg += "CACHE-CONTROL: max-age=" + to_string(kMaxAgeSec) + "\r\n";
        msg += "LOCATION: " + location + "\r\n";
        msg += "SERVER: " + server + "\r\n";
    }
    msg += "NT: " + nt + "\r\n";
    msg += "NTS: " + nts + "\r\n";
    msg += "USN: " + usnFor(uuid, nt) + "\r\n";
    msg += "\r\n";
    return msg;
}

bool ssdpIsSearchRequest(const string& datagram)
{
    return startsWithNoCase(datagram, "M-SEARCH ");
}

string ssdpSearchTarget(const string& datagram)
{
    // The header may stand at the start of a line, or at the very beginning of a
    // datagram that has no request line (the caller checks the request line).
    size_t pos = 0;
    while (pos < datagram.size()) {
        const size_t lineEnd = datagram.find('\n', pos);
        const size_t end = (lineEnd == string::npos) ? datagram.size() : lineEnd;
        string line = datagram.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (startsWithNoCase(line, "ST:")) {
            string value = line.substr(3);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            return value;
        }
        if (lineEnd == string::npos) break;
        pos = lineEnd + 1;
    }
    return "";
}

string deviceXml(const string& friendlyName, const string& modelName,
                 const string& modelNumber, const string& uuid,
                 const string& presentationUrl)
{
    string xml = "<?xml version=\"1.0\"?>\r\n";
    xml += "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n";
    xml += "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n";
    xml += "  <device>\r\n";
    xml += string("    <deviceType>") + kDeviceType + "</deviceType>\r\n";
    xml += "    <friendlyName>" + xmlEscaped(friendlyName) + "</friendlyName>\r\n";
    xml += "    <manufacturer>DHCPServer</manufacturer>\r\n";
    xml += "    <modelDescription>DHCP, DNS and NTP server</modelDescription>\r\n";
    xml += "    <modelName>" + xmlEscaped(modelName) + "</modelName>\r\n";
    xml += "    <modelNumber>" + xmlEscaped(modelNumber) + "</modelNumber>\r\n";
    xml += "    <UDN>" + xmlEscaped(uuid) + "</UDN>\r\n";
    xml += "    <presentationURL>" + xmlEscaped(presentationUrl) + "</presentationURL>\r\n";
    xml += "  </device>\r\n";
    xml += "</root>\r\n";
    return xml;
}

} // namespace ssdp
} // namespace net
} // namespace dhcp
