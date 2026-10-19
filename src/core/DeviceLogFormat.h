#ifndef DHCP_CORE_DEVICELOGFORMAT_H
#define DHCP_CORE_DEVICELOGFORMAT_H

#include <string>

namespace dhcp {
namespace core {

/**
 * @file DeviceLogFormat.h
 * @brief How a device event reads in `Device.log`.
 *
 * The journal answers the question an operator asks after every surprise: what
 * did this box do, and why did it restart? The sentences are therefore about
 * **the device itself**, not about a service and not about a fault:
 *
 *   * a run started, and with which version;
 *   * the previous run ended with something other than a power-on — the reason
 *     is named in the same line, because a panic that leaves no trace is the
 *     case this journal exists for;
 *   * somebody asked for a restart (the web interface, the console, an SSH
 *     session), which a plain boot line cannot tell from a planned restart;
 *   * the settings were restored from a backup file, and a factory reset erased
 *     them — the second one especially, since nothing else survives it.
 *
 * The text is built without its timestamp — `ErrorLogCore` adds that — which is
 * what lets a host test check the sentences.
 */

/// @brief `boot: DHCPServer 01.03.058.00.26.10.RU started`.
inline std::string deviceBootText(const std::string& version)
{
    return "boot: DHCPServer " + version + " started";
}

/**
 * @brief `boot: DHCPServer <version> started; the previous run ended with <reason>`.
 *
 * The reason is the firmware's own wording for the reset cause (`a panic (the
 * firmware aborted)`, `the task watchdog`, `a software restart`), so the file
 * says what the terminal would have said on the previous run.
 */
inline std::string deviceBootAfterText(const std::string& version,
                                       const std::string& reason)
{
    return deviceBootText(version) + "; the previous run ended with " + reason;
}

/// @brief `restart requested from the web interface`.
inline std::string deviceRestartText(const std::string& source)
{
    return "restart requested from " + source;
}

/// @brief `factory reset requested from the console: every setting is erased and
///        the device restarts`.
inline std::string deviceFactoryResetText(const std::string& source)
{
    return "factory reset requested from " + source +
           ": every setting is erased and the device restarts";
}

/// @brief `settings imported from a backup file`.
inline std::string deviceSettingsImportText()
{
    return "settings imported from a backup file";
}

/**
 * @brief `libssh ssh_kex2: ...` — the library's own account of a key exchange.
 *
 * The 19.10.2026 panic happened inside `ssh_handle_key_exchange()`, where nothing
 * of ours runs: the only witness is libssh itself. Its log is routed into this
 * journal, so the last line before a reboot says what it was doing.
 */
inline std::string deviceLibsshText(const std::string& function,
                                    const std::string& message)
{
    std::string text = message;
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return "libssh " + (function.empty() ? std::string("?") : function) + ": " + text;
}

/**
 * @brief `the previous run ended inside the SSH server: the key exchange`.
 *
 * The reason a restart happened is one line; **where** it happened is this one.
 * The stage comes from RTC memory (`net/SshStage.h`), so it survives the panic
 * that no file write could survive.
 */
inline std::string deviceSshCrashText(const std::string& stage)
{
    return "the previous run ended inside the SSH server: " + stage;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_DEVICELOGFORMAT_H
