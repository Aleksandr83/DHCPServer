#include "TerminalMenu.h"
#include "../wifi/IWiFiManager.h"
#include "../led/ILedController.h"
#include "../core/Version.h"
#include "../core/Config.h"
#include "../web/AuthManager.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>
#include <algorithm>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace std;

static const char* TAG = "TerminalMenu";

namespace dhcp {
namespace menu {

static const char* HELP_TEXT =
    "Available commands:\r\n"
    "  lan status                  — Show LAN connection status and IP\r\n"
    "  passwd reset                — Reset web password to default (admin)\r\n"
    "  settings reset              — Factory reset ALL settings to defaults and reboot\r\n"
    "  version                     — Show firmware version\r\n"
    "  help                        — Show this help\r\n"
    "  reboot                      — Reboot the device\r\n";

TerminalMenu::TerminalMenu(dhcp::wifi::IWiFiManager& wifi,
                           dhcp::led::ILedController& led,
                           dhcp::web::AuthManager* auth,
                           const string& prompt)
    : wifi_(wifi)
    , led_(led)
    , auth_(auth)
    , prompt_(prompt)
{
}

void TerminalMenu::setLifecycleHooks(Hook beforeReboot, Hook factoryReset)
{
    beforeReboot_ = move(beforeReboot);
    factoryReset_ = move(factoryReset);
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
    if (line.empty()) return;

    istringstream stream(line);
    string cmd;
    stream >> cmd;

    if (cmd == "help") {
        cmdHelp();
    } else if (cmd == "lan") {
        string sub;
        stream >> sub;
        if (sub == "status") {
            cmdLanStatus();
        } else {
            println("Unknown lan subcommand. Usage: lan status");
        }
    } else if (cmd == "version") {
        cmdVersion();
    } else if (cmd == "passwd") {
        string sub;
        stream >> sub;
        if (sub == "reset") {
            cmdPasswdReset();
        } else {
            println("Unknown passwd subcommand. Usage: passwd reset");
        }
    } else if (cmd == "settings") {
        string sub;
        stream >> sub;
        if (sub == "reset") {
            cmdSettingsReset();
        } else {
            println("Unknown settings subcommand. Usage: settings reset");
        }
    } else if (cmd == "reboot") {
        println("Rebooting...");
        // Same policy as the web paths: a planned restart keeps the counters of the
        // main page (Statistica.dat on the internal volume). The pause is there
        // because this path had none — the file is 92 bytes, but it goes to flash.
        if (beforeReboot_) beforeReboot_();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        println("Unknown command. Type 'help' for available commands.");
    }
}

void TerminalMenu::cmdHelp()
{
    print(HELP_TEXT);
}

void TerminalMenu::cmdLanStatus()
{
    if (wifi_.isConnected()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                      "Status: Connected\r\n"
                      "  Link: %s\r\n"
                      "  IPv4: %s\r\n"
                      "  IPv6: %s\r\n",
                      wifi_.ssid().c_str(),
                      wifi_.ip4().c_str(),
                      wifi_.ip6().c_str());
        print(buf);
    } else {
        println("Status: Disconnected");
    }
}

void TerminalMenu::cmdVersion()
{
    const auto& ver = dhcp::core::Version::instance();
    char buf[64];
    snprintf(buf, sizeof(buf), "Firmware version: %s\r\n", ver.toString().c_str());
    print(buf);
}

void TerminalMenu::cmdPasswdReset()
{
    // Reset web credentials to defaults and persist to NVS
    // The command owns the credentials and the lockout limits only, so the rest
    // of the security settings is read first and carried over: a default-built
    // struct here would switch HTTPS off and send the certificate back to the
    // internal volume in NVS, and the device would come up serving an interface
    // the operator had configured otherwise (stage 160).
    dhcp::core::SecurityConfig sec = dhcp::core::Config::instance().getSecurity();
    sec.username = "admin";
    sec.password = "admin";
    sec.maxAttempts = core::SecurityConfig::kDefaultMaxAttempts;
    sec.lockoutPeriodSec = core::SecurityConfig::kDefaultLockoutSec;
    dhcp::core::Config::instance().setSecurity(sec);

    // Apply immediately in the running web server
    if (auth_) {
        auth_->reloadConfig();
        println("Web password reset to default (admin/admin) and applied.");
    } else {
        println("Web password reset to default (admin/admin). Reboot to apply.");
    }
    ESP_LOGW(TAG, "Web credentials reset to default by console command");
}

void TerminalMenu::cmdSettingsReset()
{
    // Full factory reset: erase the whole NVS settings namespace, then reboot.
    // On the next boot every getter falls back to its compile-time default.
    if (!dhcp::core::Config::instance().resetAll()) {
        println("Settings reset FAILED (NVS erase error).");
        return;
    }
    // The statistics describe the settings that were just erased.
    if (factoryReset_) factoryReset_();
    println("All settings erased. Rebooting to factory defaults...");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

} // namespace menu
} // namespace dhcp
