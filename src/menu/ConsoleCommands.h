#ifndef DHCP_MENU_CONSOLECOMMANDS_H
#define DHCP_MENU_CONSOLECOMMANDS_H

#include "ConsoleCommand.h"

#include <functional>
#include <string>

namespace dhcp {
namespace wifi { class IWiFiManager; }
namespace web { class AuthManager; }
} // namespace dhcp

namespace dhcp {
namespace menu {

/**
 * @brief Where a console writes (stage 222).
 *
 * The serial menu writes to `printf`, the SSH shell writes into an SSH channel.
 * Both print the same text from the same code, so the sink is the only thing
 * that differs — that is the whole reason this interface exists.
 */
class IConsoleOutput {
public:
    virtual ~IConsoleOutput() = default;
    virtual void write(const std::string& text) = 0;
};

/**
 * @brief The commands the serial console and the SSH shell share.
 *
 * One implementation, two callers: a second copy of `passwd reset` or of the
 * factory-reset sequence would drift from this one the first time either is
 * touched.
 *
 * `processLine()` returns false when the line asked the session to end — the
 * SSH shell then closes the channel; the serial menu has no session and keeps
 * running.
 */
class ConsoleCommands {
public:
    using Hook = std::function<void()>;

    ConsoleCommands(IConsoleOutput& out,
                    ::dhcp::wifi::IWiFiManager& wifi,
                    ::dhcp::web::AuthManager* auth = nullptr);

    /**
     * @brief What the two commands that touch stored state have to do.
     *
     * Same hooks the serial menu already uses: `reboot` keeps the main-page
     * statistics, a factory reset deletes them, and both live outside this class.
     */
    void setLifecycleHooks(Hook beforeReboot, Hook factoryReset);

    /// @brief Execute one typed line; false means "end this session".
    bool processLine(const std::string& line);

    /// @brief The help text, also used as the SSH banner.
    void printHelp();

private:
    void cmdLanStatus();
    void cmdIfconfig();
    void cmdVersion();
    void cmdPasswdReset();
    void cmdSettingsReset();
    void cmdReboot();

    IConsoleOutput& out_;
    ::dhcp::wifi::IWiFiManager& wifi_;
    ::dhcp::web::AuthManager* auth_;
    Hook beforeReboot_;
    Hook factoryReset_;
};

} // namespace menu
} // namespace dhcp

#endif // DHCP_MENU_CONSOLECOMMANDS_H
