#ifndef DHCP_WEB_JSONWRITER_H
#define DHCP_WEB_JSONWRITER_H

#include <cstdint>
#include <string>

namespace dhcp {
namespace web {

/**
 * @brief Writer for a flat JSON object that owns the comma bookkeeping.
 *
 * The REST layer used to append members through helpers that took an
 * `addComma` flag, and a single wrong flag produced `{,"enabled":false,…}` —
 * a body that answers `200 OK` but makes `JSON.parse()` throw in the browser
 * ("Expected property name or '}' in JSON at position 1"), which is exactly
 * how the file explorer broke. Here the writer decides: it inserts a comma
 * before every member except the first one, so a leading, doubled or trailing
 * comma cannot be emitted at all.
 *
 * Deliberately minimal and free of ESP-IDF dependencies (host-testable):
 * a flat object of string/number/boolean/literal members. Nesting is done by
 * building the nested value into its own writer and passing the result to
 * @ref literal.
 *
 * @code
 * JsonWriter w;
 * w.boolean("enabled", true);
 * w.num("count", 3);
 * w.literal("entries", "[]");
 * httpd_resp_sendstr(req, w.toString().c_str());   // {"enabled":true,"count":3,"entries":[]}
 * @endcode
 */
class JsonWriter {
public:
    /** @brief Append a string member; the value is escaped by the writer. */
    void str(const std::string& key, const std::string& value);

    /** @brief Append an integer member. */
    void num(const std::string& key, int64_t value);

    /** @brief Append a boolean member. */
    void boolean(const std::string& key, bool value);

    /**
     * @brief Append a member whose value is already valid JSON.
     *
     * Used for values the writer does not build itself (arrays, nested
     * objects, pre-escaped text).
     */
    void literal(const std::string& key, const std::string& json);

    /** @brief True while no member has been added. */
    bool empty() const { return body_.empty(); }

    /** @brief The complete `{…}` document. */
    std::string toString() const { return "{" + body_ + "}"; }

    /**
     * @brief Escape a string so it can sit between JSON quotes.
     *
     * Double quote, backslash, newline, carriage return and tab get their
     * two-character escapes; any other control character is dropped, because
     * it would make the document invalid and none of them can appear in a
     * volume error text, a file name or an editable text file.
     */
    static std::string escape(const std::string& in);

private:
    /** @brief Add the separating comma, unless this is the first member. */
    void separate();

    std::string body_;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_JSONWRITER_H
