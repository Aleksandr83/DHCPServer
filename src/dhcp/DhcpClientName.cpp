#include "DhcpClientName.h"

#include <cstring>

namespace dhcp {
namespace dhcp {

namespace {
constexpr uint8_t kOptPad   = 0;
constexpr uint8_t kOptEnd   = 255;
constexpr uint8_t kOptHostName = 12;   // RFC 2132
constexpr uint8_t kOptFqdn     = 81;   // RFC 4702
constexpr uint8_t kFqdnFlagE   = 0x04; // domain name is DNS-encoded
} // namespace

const uint8_t* DhcpClientName::findOption(const uint8_t* options, size_t len,
                                          uint8_t code, size_t* outLen)
{
    if (!options) return nullptr;
    size_t i = 0;
    while (i < len) {
        const uint8_t opt = options[i];
        if (opt == kOptPad) { i++; continue; }
        if (opt == kOptEnd) return nullptr;
        if (i + 1 >= len) return nullptr;          // length byte missing
        const size_t optLen = options[i + 1];
        if (i + 2 + optLen > len) return nullptr;  // truncated option
        if (opt == code) {
            if (outLen) *outLen = optLen;
            return options + i + 2;
        }
        i += 2 + optLen;
    }
    return nullptr;
}

std::string DhcpClientName::decodeDnsName(const uint8_t* data, size_t len)
{
    std::string out;
    size_t i = 0;
    while (i < len) {
        const size_t labelLen = data[i];
        if (labelLen == 0) break;                  // root: end of the name
        if (labelLen > 63 || i + 1 + labelLen > len) return std::string();
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(data + i + 1), labelLen);
        i += 1 + labelLen;
    }
    return out;
}

std::string DhcpClientName::hostNameOption(const uint8_t* options, size_t len)
{
    size_t optLen = 0;
    const uint8_t* data = findOption(options, len, kOptHostName, &optLen);
    if (!data || optLen == 0) return std::string();

    // The value is a NUL-terminated string that may be padded with NULs; take
    // everything up to the first NUL.
    size_t n = 0;
    while (n < optLen && data[n] != 0) n++;
    return sanitize(std::string(reinterpret_cast<const char*>(data), n));
}

std::string DhcpClientName::fqdnOption(const uint8_t* options, size_t len)
{
    size_t optLen = 0;
    const uint8_t* data = findOption(options, len, kOptFqdn, &optLen);
    if (!data || optLen < 3) return std::string();   // flags + rcode1 + rcode2

    const uint8_t flags = data[0];
    const uint8_t* name = data + 3;
    const size_t nameLen = optLen - 3;

    std::string value;
    if (flags & kFqdnFlagE) {
        value = decodeDnsName(name, nameLen);
    } else {
        size_t n = 0;
        while (n < nameLen && name[n] != 0) n++;
        value.assign(reinterpret_cast<const char*>(name), n);
    }
    value = sanitize(value);

    // The client may have sent the whole FQDN or only the domain (RFC 4702
    // allows both). A trailing dot is legal in DNS text and means nothing here.
    while (!value.empty() && value.back() == '.') value.pop_back();
    return value;
}

std::string DhcpClientName::shortLabel(const std::string& name)
{
    const size_t dot = name.find('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

std::string DhcpClientName::fromOptions(const uint8_t* options, size_t len)
{
    const std::string host = hostNameOption(options, len);
    if (!host.empty()) return host;      // option 12 is the name itself

    // Option 81 may be "pc1.lan" (use the host part) or just "lan" when the
    // client left the host name to option 12 — which was empty. Both cases end
    // up as whatever the client sent; the operator sees it and can fix it, and
    // the page never claims this is more than a suggestion.
    return shortLabel(fqdnOption(options, len));
}

std::string DhcpClientName::sanitize(const std::string& name)
{
    std::string out;
    size_t i = 0;
    while (i < name.size()) {
        const unsigned char c = static_cast<unsigned char>(name[i]);

        // One character: ASCII, or a validated UTF-8 sequence. Anything else is
        // dropped rather than copied — the name is written into JSON and into
        // the settings export, where an invalid byte breaks the reader.
        size_t charLen = 1;
        bool keep = false;
        if (c >= 0x20 && c < 0x7F) {
            // '|' separates the fields of the allow-list blob and CR/LF separate
            // its lines: a name carrying one would corrupt what it is copied to.
            keep = (c != '|');
        } else if (c >= 0xC2 && c <= 0xDF) {
            charLen = 2;
            keep = (i + 1 < name.size()) &&
                   ((static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80);
        } else if (c >= 0xE0 && c <= 0xEF) {
            charLen = 3;
            keep = (i + 2 < name.size()) &&
                   ((static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80) &&
                   ((static_cast<unsigned char>(name[i + 2]) & 0xC0) == 0x80);
        } else if (c >= 0xF0 && c <= 0xF4) {
            charLen = 4;
            keep = (i + 3 < name.size()) &&
                   ((static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80) &&
                   ((static_cast<unsigned char>(name[i + 2]) & 0xC0) == 0x80) &&
                   ((static_cast<unsigned char>(name[i + 3]) & 0xC0) == 0x80);
        } else {
            keep = false;   // control byte, stray continuation or 5-byte form
        }

        if (keep) {
            if (out.size() + charLen > kMaxLen) break;   // cut on a boundary
            out.append(name, i, charLen);
        }
        i += keep ? charLen : 1;
    }

    // Same trimming the allow-list codec does, so a name pasted from here keeps
    // its shape when it is stored there (tabs are already gone: they are control
    // characters and were dropped above).
    size_t b = 0;
    while (b < out.size() && out[b] == ' ') b++;
    size_t e = out.size();
    while (e > b && out[e - 1] == ' ') e--;
    return out.substr(b, e - b);
}

} // namespace dhcp
} // namespace dhcp
