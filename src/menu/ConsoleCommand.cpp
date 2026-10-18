#include "ConsoleCommand.h"

#include <sstream>

using namespace std;

namespace dhcp {
namespace menu {

ConsoleCommand parseConsoleCommand(const string& line)
{
    istringstream stream(line);
    string cmd;
    if (!(stream >> cmd)) return { ConsoleAction::None };

    auto needSub = [&stream](const char* expected) {
        string sub;
        stream >> sub;
        return sub == expected;
    };

    if (cmd == "help") return { ConsoleAction::Help };
    if (cmd == "version") return { ConsoleAction::Version };
    if (cmd == "ifconfig") return { ConsoleAction::Ifconfig };
    if (cmd == "reboot") return { ConsoleAction::Reboot };
    if (cmd == "exit" || cmd == "quit" || cmd == "logout") {
        return { ConsoleAction::Exit };
    }
    if (cmd == "lan") {
        return { needSub("status") ? ConsoleAction::LanStatus : ConsoleAction::UnknownSub };
    }
    if (cmd == "passwd") {
        return { needSub("reset") ? ConsoleAction::PasswdReset : ConsoleAction::UnknownSub };
    }
    if (cmd == "settings") {
        return { needSub("reset") ? ConsoleAction::SettingsReset : ConsoleAction::UnknownSub };
    }
    return { ConsoleAction::Unknown };
}

} // namespace menu
} // namespace dhcp
