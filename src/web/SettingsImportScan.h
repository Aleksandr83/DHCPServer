#ifndef DHCP_WEB_SETTINGSIMPORTSCAN_H
#define DHCP_WEB_SETTINGSIMPORTSCAN_H

#include <string>
#include <vector>

namespace dhcp {
namespace web {

/**
 * @brief Whitespace inside a JSON document.
 *
 * Not `isspace`: the body is bytes, and a byte above 0x7F must not be read as a
 * character (that would need the C locale, which this code does not touch).
 */
inline bool isJsonSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/**
 * @file SettingsImportScan.h
 * @brief Which keys of an imported settings document this firmware does not know.
 *
 * `POST /api/settings/import` applies the sections it recognizes and reports the
 * rest to the operator as "not imported (unknown to this firmware)". That report
 * used to be produced by looking at every quoted token followed by ':' — and a
 * *value* can look exactly like that: a name holding a quote and a colon ends a
 * token right before one, so an ordinary setting was reported as unknown. A key
 * in JSON is preceded by '{' or ',' instead, and that is the difference this scan
 * uses: a token that follows a value is not a key, whatever it contains.
 *
 * Escapes are skipped rather than decoded (a `\"` inside a value is not the end
 * of it), so a value written by the export cannot fake a key.
 *
 * Free of ESP-IDF on purpose — host-tested in `test/test_settingsimportscan.cpp`.
 *
 * @param body  The document as it arrived.
 * @param known Keys this firmware recognizes, at any depth.
 * @return The unknown keys, in the order they appear, each once.
 */
inline std::vector<std::string> unknownSettingsKeys(const std::string& body,
                                                    const std::vector<std::string>& known)
{
    std::vector<std::string> out;
    const size_t n = body.size();

    for (size_t i = 0; i < n; i++) {
        if (body[i] != '"') continue;

        const size_t open = i;              // the opening quote of this token
        const size_t start = i + 1;
        size_t end = start;
        while (end < n && body[end] != '"') {
            if (body[end] == '\\') end++;   // an escape: its quote is not the end
            end++;
        }
        if (end >= n) break;
        i = end;                            // carry on after this string

        // A key follows '{' or ','; a value follows ':'. The character to look at
        // is the one before the *opening* quote of the token — the character
        // before the text is always that quote itself.
        size_t before = open;
        while (before > 0 && isJsonSpace(body[before - 1])) before--;
        if (before == 0) continue;
        if (body[before - 1] != '{' && body[before - 1] != ',') continue;

        size_t after = end + 1;
        while (after < n && isJsonSpace(body[after])) after++;
        if (after >= n || body[after] != ':') continue;

        const std::string key = body.substr(start, end - start);
        bool isKnown = false;
        for (const auto& k : known) {
            if (k == key) { isKnown = true; break; }
        }
        if (isKnown) continue;

        bool seen = false;
        for (const auto& k : out) {
            if (k == key) { seen = true; break; }
        }
        if (!seen) out.push_back(key);
    }
    return out;
}

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_SETTINGSIMPORTSCAN_H
