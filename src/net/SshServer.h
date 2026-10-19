#ifndef DHCP_NET_SSHSERVER_H
#define DHCP_NET_SSHSERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "../core/SubnetFilter.h"
namespace dhcp {
namespace wifi { class IWiFiManager; }
namespace web { class AuthManager; }
} // namespace dhcp

namespace dhcp {
namespace net {

/**
 * @brief The device's SSH2 server (stage 222).
 *
 * One listener, at most @ref kMaxSessions sessions, and the **same** commands the
 * serial console runs — `menu::ConsoleCommands` is shared, so a command cannot
 * behave differently over SSH. Authentication is the web interface's login pair
 * (`Settings ▸ Security`), compared in constant time (`core::credentialsMatch`).
 *
 * The host key is generated on the device the first time the server starts and
 * kept in NVS: a restored device makes its own, and the key never travels in a
 * settings backup.
 *
 * Only the ESP32-P4 build links libssh (see `src/CMakeLists.txt`); on any other
 * target @ref start reports that the server is unavailable instead of pretending.
 */
class SshServer {
public:
    /// @brief Sessions that may run at once. The second is refused when the
    /// device is short of memory — a session that cannot be served honestly is
    /// better refused than half-served.
    static constexpr int kMaxSessions = 2;

    SshServer(::dhcp::wifi::IWiFiManager& wifi, ::dhcp::web::AuthManager* auth);
    ~SshServer();
    SshServer(const SshServer&) = delete;
    SshServer& operator=(const SshServer&) = delete;

    /// @brief Is this build able to serve SSH at all?
    static bool available();

    /// @brief Start listening on @p port. Does nothing when already listening.
    bool start(uint16_t port);

    /// @brief Close the listener and the open sessions.
    void stop();

    bool isRunning() const { return running_.load(); }
    int activeSessions() const { return activeSessions_.load(); }

    /// @brief Hooks the shared commands need (`reboot`, factory reset).
    void setLifecycleHooks(std::function<void()> beforeReboot,
                           std::function<void()> factoryReset);

    /**
     * @brief Re-read the own-subnet filter's settings.
     *
     * Called when the server starts and whenever the SSH or DHCP settings are
     * saved. The accept loop deliberately does **not** read them per connection:
     * that put an NVS read (a flash access) inside the task serving a client, and
     * the 19.10.2026 panic happened exactly there.
     */
    void refreshFilter();

private:
    /// @brief One accepted session: the socket plus what the task needs.
    struct SessionArg {
        SshServer* server = nullptr;
        void* session = nullptr;   // ssh_session
        std::string clientIp;      // as text, for the journal
    };

    static void acceptTask(void* arg);
    static void sessionTask(void* arg);

    void acceptLoop();
    void runSession(void* sessionRaw, const std::string& clientIp);

    /// @brief The stored host key, generated and saved on first use.
    std::string hostKey();

    ::dhcp::wifi::IWiFiManager& wifi_;
    ::dhcp::web::AuthManager* auth_;
    std::atomic<bool> running_{false};
    std::atomic<int> activeSessions_{0};
    void* bind_ = nullptr;        // ssh_bind
    void* acceptTask_ = nullptr;  // TaskHandle_t
    std::string hostKey_;
    ::dhcp::core::SubnetFilter filter_;
    std::function<void()> beforeReboot_;
    std::function<void()> factoryReset_;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_SSHSERVER_H
