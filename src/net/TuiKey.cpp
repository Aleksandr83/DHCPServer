#include "TuiKey.h"

using namespace std;

namespace dhcp {
namespace net {

namespace {

/// @brief The key a CSI sequence's final byte means (no parameters needed).
bool arrowKey(char final_byte, TuiKeyKind& out)
{
    switch (final_byte) {
        case 'A': out = TuiKeyKind::Up; return true;
        case 'B': out = TuiKeyKind::Down; return true;
        case 'C': out = TuiKeyKind::Right; return true;
        case 'D': out = TuiKeyKind::Left; return true;
        case 'H': out = TuiKeyKind::Home; return true;
        case 'F': out = TuiKeyKind::End; return true;
        default: return false;
    }
}

/// @brief The key an `ESC [ <number> ~` sequence means.
bool tildeKey(int number, TuiKeyKind& out)
{
    switch (number) {
        case 1: case 7:   out = TuiKeyKind::Home; return true;
        case 4: case 8:   out = TuiKeyKind::End; return true;
        case 5:           out = TuiKeyKind::PageUp; return true;
        case 6:           out = TuiKeyKind::PageDown; return true;
        case 11:          out = TuiKeyKind::F1; return true;
        case 12:          out = TuiKeyKind::F2; return true;
        case 13:          out = TuiKeyKind::F3; return true;
        case 14:          out = TuiKeyKind::F4; return true;
        case 15:          out = TuiKeyKind::F5; return true;
        case 17:          out = TuiKeyKind::F6; return true;
        case 18:          out = TuiKeyKind::F7; return true;
        case 19:          out = TuiKeyKind::F8; return true;
        case 20:          out = TuiKeyKind::F9; return true;
        case 21:          out = TuiKeyKind::F10; return true;
        case 23:          out = TuiKeyKind::F11; return true;
        case 24:          out = TuiKeyKind::F12; return true;
        default:          return false;   // 2 (insert), 3 (delete) and the rest
    }
}

} // namespace

vector<TuiKey> decodeTuiKeys(const string& bytes)
{
    vector<TuiKey> keys;
    size_t i = 0;

    while (i < bytes.size()) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);

        if (c != 0x1b) {
            ++i;
            if (c == '\r' || c == '\n') {
                keys.push_back({ TuiKeyKind::Enter, 0 });
            } else if (c == '\t') {
                keys.push_back({ TuiKeyKind::Tab, 0 });
            } else if (c == 0x7f || c == 0x08) {
                keys.push_back({ TuiKeyKind::Backspace, 0 });
            } else if (c == 0x03) {
                keys.push_back({ TuiKeyKind::CtrlC, 0 });
            } else if (c >= 0x20 && c < 0x7f) {
                keys.push_back({ TuiKeyKind::Char, static_cast<char>(c) });
            }
            // Everything else (other control bytes) is not a key this screen has.
            continue;
        }

        // ESC: either the start of a sequence, or the Escape key by itself.
        if (i + 1 >= bytes.size()) {
            keys.push_back({ TuiKeyKind::Escape, 0 });
            break;
        }

        const char kind = bytes[i + 1];
        if (kind != '[' && kind != 'O') {
            // ESC followed by an ordinary byte: Escape, then that byte is read as
            // itself on the next turn — nothing is swallowed.
            keys.push_back({ TuiKeyKind::Escape, 0 });
            i += 1;
            continue;
        }

        // A CSI or SS3 sequence: the parameters run to a final byte in 0x40..0x7E.
        size_t j = i + 2;
        string params;
        while (j < bytes.size()) {
            const unsigned char b = static_cast<unsigned char>(bytes[j]);
            if (b >= 0x40 && b <= 0x7e) break;
            params += static_cast<char>(b);
            ++j;
        }
        if (j >= bytes.size()) {
            // The sequence was cut in half by the read; treat the ESC as Escape and
            // let the rest of the bytes be read as keys.
            keys.push_back({ TuiKeyKind::Escape, 0 });
            i += 1;
            continue;
        }

        const char final_byte = bytes[j];
        i = j + 1;

        TuiKeyKind decoded = TuiKeyKind::None;
        if (kind == 'O') {
            // SS3: F1..F4 and the arrows, with no parameters at all.
            switch (final_byte) {
                case 'P': decoded = TuiKeyKind::F1; break;
                case 'Q': decoded = TuiKeyKind::F2; break;
                case 'R': decoded = TuiKeyKind::F3; break;
                case 'S': decoded = TuiKeyKind::F4; break;
                default:
                    arrowKey(final_byte, decoded);
                    break;
            }
            if (decoded != TuiKeyKind::None) keys.push_back({ decoded, 0 });
            continue;
        }

        // CSI. The only parameterised forms this screen needs are `N ~` and the
        // modifier form `1 ; 3 P` / `1 ; 3 Q` (Alt+F1 / Alt+F2).
        if (!params.empty() && final_byte == '~') {
            int number = 0;
            for (char p : params) {
                if (p < '0' || p > '9') { number = -1; break; }
                number = number * 10 + (p - '0');
            }
            if (number > 0 && tildeKey(number, decoded)) {
                keys.push_back({ decoded, 0 });
            }
            continue;
        }
        if (params == "1;3" && final_byte == 'P') {
            keys.push_back({ TuiKeyKind::AltF1, 0 });
            continue;
        }
        if (params == "1;3" && final_byte == 'Q') {
            keys.push_back({ TuiKeyKind::AltF2, 0 });
            continue;
        }
        if (params.empty() && arrowKey(final_byte, decoded)) {
            keys.push_back({ decoded, 0 });
        }
        // Anything else (delete, insert, a mouse report) is not a key here.
    }

    return keys;
}

} // namespace net
} // namespace dhcp
