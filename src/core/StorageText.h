#ifndef DHCP_CORE_STORAGETEXT_H
#define DHCP_CORE_STORAGETEXT_H

#include <cctype>
#include <string>

namespace dhcp {
namespace core {

/**
 * @file StorageText.h
 * @brief One text field of a delimited settings blob, ready to be stored.
 *
 * The static bindings, the local hosts and the DHCP allow-list are each stored
 * as ONE text blob: '|' separates the fields of an entry and '\n' separates the
 * entries. A value carrying one of those characters is not escaped by that
 * format — it *becomes* a separator, and every field after it shifts on the next
 * read: a local host named `a|b` with the address `1.2.3.4` came back as the name
 * `a`, the address `b`, and `1.2.3.4` as its IPv6 address. The allow-list cleaned
 * its names from the start (`DhcpAllowedList::sanitizeName`); the rule lives here
 * now, so the three lists cannot drift apart.
 *
 * The spaces around the value are trimmed for the same reason the separators are
 * replaced: the readers trim their lines, so a stored leading space would
 * silently disappear, and a value would not survive its own round trip.
 *
 * Free of ESP-IDF on purpose: the settings layer, the three list codecs and the
 * host tests all need it.
 */
inline std::string storageFieldText(const std::string& value)
{
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) begin++;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;

    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; i++) {
        const char c = value[i];
        // '|' ends a field and CR/LF end an entry in this blob.
        out += (c == '|' || c == '\n' || c == '\r') ? ' ' : c;
    }
    return out;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_STORAGETEXT_H
