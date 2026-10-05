#include "JsonWriter.h"

using namespace std;

namespace {

// Rule 39: the same control-character limit the file name rules use.
constexpr unsigned char kAsciiPrintableMin = 0x20;

} // namespace

namespace dhcp {
namespace web {

void JsonWriter::separate()
{
    if (!body_.empty()) body_ += ',';
}

void JsonWriter::str(const string& key, const string& value)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":\"";
    body_ += escape(value);
    body_ += '"';
}

void JsonWriter::num(const string& key, int64_t value)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":";
    body_ += to_string(value);
}

void JsonWriter::boolean(const string& key, bool value)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":";
    body_ += value ? "true" : "false";
}

void JsonWriter::literal(const string& key, const string& json)
{
    separate();
    body_ += '"';
    body_ += escape(key);
    body_ += "\":";
    body_ += json;
}

string JsonWriter::escape(const string& in)
{
    string out;
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
                if (static_cast<unsigned char>(c) < kAsciiPrintableMin) break;
                out += c;
        }
    }
    return out;
}

string JsonWriter::unescape(const string& in)
{
    string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '\\' || i + 1 >= in.size()) {
            out += in[i];
            continue;
        }
        const char next = in[++i];
        switch (next) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            // '"', '\\' and '/' stand for themselves. So does the name of any
            // escape this reader does not know — including `\uXXXX`, which this
            // writer never emits and which is therefore kept as its four
            // characters rather than decoded into a guessed byte.
            default: out += next; break;
        }
    }
    return out;
}

} // namespace web
} // namespace dhcp
