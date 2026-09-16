#include "JsonWriter.h"

namespace dhcp {
namespace web {

void JsonWriter::separate()
{
    if (!body_.empty()) body_ += ',';
}

void JsonWriter::str(const std::string& key, const std::string& value)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":\"";
    body_ += escape(value);
    body_ += '"';
}

void JsonWriter::num(const std::string& key, int64_t value)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":";
    body_ += std::to_string(value);
}

void JsonWriter::boolean(const std::string& key, bool value)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":";
    body_ += value ? "true" : "false";
}

void JsonWriter::literal(const std::string& key, const std::string& json)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":";
    body_ += json;
}

std::string JsonWriter::escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // Control characters would make the body invalid JSON.
                if (static_cast<unsigned char>(c) < 0x20) break;
                out += c;
        }
    }
    return out;
}

} // namespace web
} // namespace dhcp
