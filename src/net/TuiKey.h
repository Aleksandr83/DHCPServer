#ifndef DHCP_NET_TUIKEY_H
#define DHCP_NET_TUIKEY_H

#include <string>
#include <vector>

namespace dhcp {
namespace net {

/**
 * @file TuiKey.h
 * @brief What a terminal sends, turned into keys a screen can act on.
 *
 * A terminal speaks in byte sequences: an arrow is `ESC [ A`, F5 is `ESC [ 15 ~`,
 * Alt+F1 (the panel volume switch in this browser) is `ESC [ 1 ; 3 P`, and a plain
 * letter is a letter. A browser that decodes them ad hoc grows a different bug for
 * every client, so the decoding is one pure function and a host test holds it.
 *
 * Deliberately small: the keys this browser uses, and nothing else. A sequence it
 * does not know decodes into nothing at all — it must not swallow the bytes that
 * follow it, because the next byte is usually a key the operator meant to press.
 */
enum class TuiKeyKind {
    None,       ///< never produced; an empty answer is an empty vector
    Char,       ///< a printable character, in @ref TuiKey::ch
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Enter,
    Backspace,
    Tab,
    Escape,     ///< a lone ESC
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    AltF1,      ///< switch the left panel's volume
    AltF2,      ///< switch the right panel's volume
    CtrlC       ///< 0x03, the terminal's interrupt
};

struct TuiKey {
    TuiKeyKind kind = TuiKeyKind::None;
    char ch = '\0';
};

/// @brief Decode one chunk of bytes from the terminal, in order.
std::vector<TuiKey> decodeTuiKeys(const std::string& bytes);

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_TUIKEY_H
