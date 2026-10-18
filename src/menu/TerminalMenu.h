#ifndef DHCP_MENU_TERMINALMENU_H
#define DHCP_MENU_TERMINALMENU_H

#include "ConsoleCommands.h"

#include <string>
#include <functional>

namespace dhcp {
namespace wifi {
class IWiFiManager;
} // namespace wifi

namespace led {
class ILedController;
} // namespace led

namespace web {
class AuthManager;
} // namespace web

namespace menu {

/**
 * @brief Interactive serial terminal menu.
 *
 * Provides commands over UART (USB serial):
 *   lan status                  — Show LAN connection status + IP
 *   ifconfig                    — Addresses, mask, gateway and DNS
 *   passwd reset                — Reset web password to default (admin)
 *   settings reset              — Factory reset settings and reboot
 *   version                     — Show firmware version
 *   reboot                      — Reboot the device
 *   help                        — Show available commands
 *
 * The commands themselves live in @ref ConsoleCommands, which the SSH shell
 * uses too (stage 222): this class only owns the serial side — the prompt, the
 * banner and the `printf` sink.
 */
class TerminalMenu : public IConsoleOutput {
public:
    /**
     * @param wifi   Reference to WiFi manager.
     * @param led    Reference to LED controller.
     * @param auth   Optional pointer to web AuthManager (to reload creds).
     * @param prompt Prompt string (default "dhcp> ").
     */
    TerminalMenu(dhcp::wifi::IWiFiManager& wifi,
                 dhcp::led::ILedController& led,
                 dhcp::web::AuthManager* auth = nullptr,
                 const std::string& prompt = "dhcp> ");

    /**
     * @brief What the two console commands that touch stored state have to do.
     *
     * `reboot` keeps the main-page statistics, a factory reset deletes them —
     * both live in the DNS server, and the menu must not include its header: that
     * include drags `namespace dhcp::dhcp` into this translation unit, after which
     * `dhcp::core::…` in this file silently resolves to `dhcp::dhcp::core` and
     * stops compiling. Two callbacks keep the layering and the compile unit clean.
     */
    using Hook = std::function<void()>;
    void setLifecycleHooks(Hook beforeReboot, Hook factoryReset);

    /**
     * @brief Start the terminal menu task.
     * Reads commands from stdin and processes them.
     */
    void start();

    /**
     * @brief Stop the terminal menu task.
     */
    void stop();

    /**
     * @brief Print a message.
     */
    void print(const std::string& msg);
    void println(const std::string& msg);

    /// @brief The sink the shared commands write into (this console's stdout).
    void write(const std::string& text) override { print(text); }

    /**
     * @brief Process a single command line (public for main loop).
     */
    void processLine(const std::string& line);

private:
    dhcp::wifi::IWiFiManager& wifi_;
    dhcp::led::ILedController& led_;
    dhcp::web::AuthManager* auth_;
    ConsoleCommands commands_;
    std::string prompt_;
    bool running_ = false;
};

} // namespace menu
} // namespace dhcp

#endif // DHCP_MENU_TERMINALMENU_H
