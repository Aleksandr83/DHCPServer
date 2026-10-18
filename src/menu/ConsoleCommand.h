#ifndef DHCP_MENU_CONSOLECOMMAND_H
#define DHCP_MENU_CONSOLECOMMAND_H

#include <string>

namespace dhcp {
namespace menu {

/**
 * @brief What one typed console line asks for (stage 222).
 *
 * The serial menu and the SSH shell run the **same** commands, so the meaning of
 * a line is decided once, here, and both consoles act on the answer. The parser
 * is pure — no ESP-IDF, no output — which is what lets a host test hold it.
 */
enum class ConsoleAction {
    None,            ///< empty line: nothing to do
    Help,
    LanStatus,
    Ifconfig,        ///< addresses, mask, gateway, DNS
    Version,
    PasswdReset,
    SettingsReset,
    Reboot,
    Exit,            ///< leave the session (the serial console has nothing to leave)
    Unknown,         ///< not a command
    UnknownSub       ///< a command without the expected sub-command
};

struct ConsoleCommand {
    ConsoleAction action = ConsoleAction::None;
};

/// @brief Read a line the way both consoles do: exact words, no case folding.
ConsoleCommand parseConsoleCommand(const std::string& line);

/// @brief Does this action end the session (only `exit`, `quit`, `logout`)?
constexpr bool consoleActionEndsSession(ConsoleAction action)
{
    return action == ConsoleAction::Exit;
}

} // namespace menu
} // namespace dhcp

#endif // DHCP_MENU_CONSOLECOMMAND_H
