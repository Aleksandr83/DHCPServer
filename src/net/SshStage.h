#ifndef DHCP_NET_SSHSTAGE_H
#define DHCP_NET_SSHSTAGE_H

#include <cstdint>

namespace dhcp {
namespace net {

/**
 * @brief How far the SSH server got before the device died.
 *
 * The panics of 19.10.2026 killed the device every time a client connected, and
 * nothing that is written to a file could say where: the journal's line is queued
 * and a FAT write is not synchronous with the crash, and there is no UART cable
 * on the bench. So the stage is kept in **RTC memory**, which survives a panic
 * (the reset that follows keeps it) and needs no file system at all: the next boot
 * reads it, writes one sentence into `Device.log`, and clears it.
 *
 * A stage is only ever a *breadcrumb*: it says what the code was doing, not what
 * went wrong. The value is meaningful only after a restart that was not a power-on
 * — a clean power cycle clears RTC memory by itself, and a planned reboot is
 * reported as such.
 */
enum class SshStage : uint32_t {
    None = 0,       ///< no session in flight (cleared after a clean end)
    Accepting,      ///< inside libssh's `ssh_bind_accept()`
    Accepted,       ///< the socket has a peer; the peer checks come next
    PeerKnown,      ///< the client address is out of `getpeername()`
    FilterChecked,  ///< the own-subnet filter has answered
    TaskCreating,   ///< between the decision to serve and FreeRTOS creating the task
    TaskCreated,    ///< the session task exists and has not run yet
    KeyExchange,    ///< inside `ssh_handle_key_exchange()`
    Shell,          ///< authenticated, running the console
};

/// @brief Store the stage (RTC memory: survives a panic, not a power cycle).
void sshStageSet(SshStage stage);

/// @brief The stage left by the previous run (None when it ended cleanly).
SshStage sshStageLast();
/// @brief The words a report uses for a stage.
inline const char* sshStageText(SshStage stage)
{
    switch (stage) {
    case SshStage::Accepting:     return "waiting inside accept";
    case SshStage::Accepted:      return "a connection accepted, reading the peer address";
    case SshStage::PeerKnown:     return "the peer address known, in the own-subnet filter";
    case SshStage::FilterChecked: return "the subnet filter passed, before the session task";
    case SshStage::TaskCreating:  return "creating the session task";
    case SshStage::TaskCreated:   return "a session task created, before the key exchange";
    case SshStage::KeyExchange:   return "the key exchange";
    case SshStage::Shell:         return "the console";
    case SshStage::None:          break;
    }
    return "";
}

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_SSHSTAGE_H
