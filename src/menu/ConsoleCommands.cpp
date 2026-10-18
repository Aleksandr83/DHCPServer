#include "ConsoleCommands.h"

#include "../wifi/IWiFiManager.h"
#include "../core/Config.h"
#include "../core/Version.h"
#include "../core/NetworkDefaults.h"
#include "../web/AuthManager.h"

#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

using namespace std;

static const char* TAG = "ConsoleCommands";

namespace dhcp {
namespace menu {

namespace {

const char* HELP_TEXT =
    "Available commands:\r\n"
    "  lan status                  — Show LAN connection status and IP\r\n"
    "  ifconfig                    — Addresses, mask, gateway and DNS\r\n"
    "  passwd reset                — Reset web password to default (admin)\r\n"
    "  settings reset              — Factory reset ALL settings to defaults and reboot\r\n"
    "  version                     — Show firmware version\r\n"
    "  help                        — Show this help\r\n"
    "  reboot                      — Reboot the device\r\n"
    "  exit                        — Close this session (SSH only)\r\n";

} // namespace

ConsoleCommands::ConsoleCommands(IConsoleOutput& out,
                                 ::dhcp::wifi::IWiFiManager& wifi,
                                 ::dhcp::web::AuthManager* auth)
    : out_(out)
    , wifi_(wifi)
    , auth_(auth)
{
}

void ConsoleCommands::setLifecycleHooks(Hook beforeReboot, Hook factoryReset)
{
    beforeReboot_ = move(beforeReboot);
    factoryReset_ = move(factoryReset);
}

void ConsoleCommands::printHelp()
{
    out_.write(HELP_TEXT);
}

bool ConsoleCommands::processLine(const string& line)
{
    const ConsoleCommand command = parseConsoleCommand(line);
    switch (command.action) {
        case ConsoleAction::None:
            return true;
        case ConsoleAction::Help:
            printHelp();
            return true;
        case ConsoleAction::LanStatus:
            cmdLanStatus();
            return true;
        case ConsoleAction::Ifconfig:
            cmdIfconfig();
            return true;
        case ConsoleAction::Version:
            cmdVersion();
            return true;
        case ConsoleAction::PasswdReset:
            cmdPasswdReset();
            return true;
        case ConsoleAction::SettingsReset:
            cmdSettingsReset();
            return true;
        case ConsoleAction::Reboot:
            cmdReboot();
            return true;
        case ConsoleAction::Exit:
            return false;
        case ConsoleAction::UnknownSub:
            out_.write("Unknown sub-command. Type 'help' for available commands.\r\n");
            return true;
        case ConsoleAction::Unknown:
        default:
            out_.write("Unknown command. Type 'help' for available commands.\r\n");
            return true;
    }
}

void ConsoleCommands::cmdLanStatus()
{
    if (!wifi_.isConnected()) {
        out_.write("Status: Disconnected\r\n");
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Status: Connected\r\n"
             "  Link: %s\r\n"
             "  IPv4: %s\r\n"
             "  IPv6: %s\r\n",
             wifi_.ssid().c_str(), wifi_.ip4().c_str(), wifi_.ip6().c_str());
    out_.write(buf);
}

void ConsoleCommands::cmdIfconfig()
{
    const auto dhcp = ::dhcp::core::Config::instance().getDhcp();
    const auto dns = ::dhcp::core::Config::instance().getDns();

    char buf[512];
    snprintf(buf, sizeof(buf),
             "%-10s %s\r\n"
             "%-10s %s\r\n"
             "%-10s %s\r\n"
             "%-10s %s\r\n"
             "%-10s %s\r\n",
             "link:", wifi_.ssid().c_str(),
             "inet:", wifi_.ip4().empty() ? "—" : wifi_.ip4().c_str(),
             "inet6:", wifi_.ip6().empty() ? "—" : wifi_.ip6().c_str(),
             "netmask:", dhcp.subnet.c_str(),
             "gateway:", dhcp.gateway.c_str());
    out_.write(buf);

    const string upstream = dns.enabled ? dns.externalDns
                                        : string("— (DNS server is off)");
    snprintf(buf, sizeof(buf),
             "%-10s %s\r\n"
             "%-10s %s\r\n",
             "dns:", upstream.c_str(),
             "dns-local:", wifi_.ip4().empty() ? "—" : wifi_.ip4().c_str());
    out_.write(buf);
}

void ConsoleCommands::cmdVersion()
{
    char buf[96];
    snprintf(buf, sizeof(buf), "Firmware version: %s\r\n",
             ::dhcp::core::Version::instance().toString().c_str());
    out_.write(buf);
}

void ConsoleCommands::cmdPasswdReset()
{
    // The command owns the credentials and the lockout limits only, so the rest
    // of the security settings is read first and carried over: a default-built
    // struct here would switch HTTPS off and send the certificate back to the
    // internal volume in NVS (stage 160).
    ::dhcp::core::SecurityConfig sec = ::dhcp::core::Config::instance().getSecurity();
    sec.username = "admin";
    sec.password = "admin";
    sec.maxAttempts = ::dhcp::core::SecurityConfig::kDefaultMaxAttempts;
    sec.lockoutPeriodSec = ::dhcp::core::SecurityConfig::kDefaultLockoutSec;
    ::dhcp::core::Config::instance().setSecurity(sec);

    if (auth_) {
        auth_->reloadConfig();
        out_.write("Web password reset to default (admin/admin) and applied.\r\n");
    } else {
        out_.write("Web password reset to default (admin/admin). Reboot to apply.\r\n");
    }
    ESP_LOGW(TAG, "Web credentials reset to default by a console command");
}

void ConsoleCommands::cmdSettingsReset()
{
    if (!::dhcp::core::Config::instance().resetAll()) {
        out_.write("Settings reset FAILED (NVS erase error).\r\n");
        return;
    }
    if (factoryReset_) factoryReset_();
    out_.write("All settings erased. Rebooting to factory defaults...\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void ConsoleCommands::cmdReboot()
{
    out_.write("Rebooting...\r\n");
    // Same policy as the web paths: a planned restart keeps the counters of the
    // main page (Statistica.dat on the internal volume).
    if (beforeReboot_) beforeReboot_();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

} // namespace menu
} // namespace dhcp
