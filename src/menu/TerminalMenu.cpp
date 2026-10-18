#include "TerminalMenu.h"
#include "../wifi/IWiFiManager.h"
#include "../led/ILedController.h"

#include <cstdio>

using namespace std;

namespace dhcp {
namespace menu {

TerminalMenu::TerminalMenu(dhcp::wifi::IWiFiManager& wifi,
                           dhcp::led::ILedController& led,
                           dhcp::web::AuthManager* auth,
                           const string& prompt)
    : wifi_(wifi)
    , led_(led)
    , auth_(auth)
    , commands_(*this, wifi, auth)
    , prompt_(prompt)
{
}

void TerminalMenu::setLifecycleHooks(Hook beforeReboot, Hook factoryReset)
{
    commands_.setLifecycleHooks(move(beforeReboot), move(factoryReset));
}

void TerminalMenu::start()
{
    running_ = true;
    println("DHCPServer Terminal Menu");
    println("Type 'help' for available commands.");
    // Prompt is printed by main loop
}

void TerminalMenu::stop()
{
    running_ = false;
    println("\r\nTerminal menu stopped.");
}

void TerminalMenu::print(const string& msg)
{
    printf("%s", msg.c_str());
    fflush(stdout);
}

void TerminalMenu::println(const string& msg)
{
    printf("%s\r\n", msg.c_str());
    fflush(stdout);
}

void TerminalMenu::processLine(const string& line)
{
    // `exit` has no meaning on the serial console (there is no session to leave),
    // so the "end this session" answer is deliberately ignored here: the serial
    // menu keeps running, exactly as it did before the SSH shell existed.
    if (!commands_.processLine(line)) {
        println("Nothing to exit on the serial console.");
    }
}

} // namespace menu
} // namespace dhcp
