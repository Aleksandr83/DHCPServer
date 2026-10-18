#include "SshServer.h"

#include "../core/Config.h"
#include "../menu/ConsoleCommands.h"

#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32P4

#include "libssh/libssh.h"
#include "libssh/server.h"
#include "libssh/callbacks.h"

#include "../core/Credentials.h"
#include "../core/ErrorLog.h"
#include "../core/SubnetFilter.h"
#include "../core/SshLog.h"
#include "../wifi/IWiFiManager.h"
#include "../web/AuthManager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cerrno>
#include <cstring>
#include <new>

using namespace std;

static const char* TAG = "SshServer";

// Rule 39: the numbers below decide how the server behaves, so they are named.
constexpr uint32_t kAcceptStackBytes = 6144;
constexpr uint32_t kSessionStackBytes = 14336;
constexpr UBaseType_t kAcceptPriority = 4;
constexpr UBaseType_t kSessionPriority = 4;
constexpr int kAuthAttemptsMax = 3;
constexpr long kSessionPollTimeoutMs = 1000;
constexpr int kStopWaitMs = 3000;
constexpr size_t kReadBufBytes = 128;
constexpr size_t kLineMaxChars = 200;
constexpr char kBackspaceDel = 0x7f;
constexpr char kBackspaceBs = 0x08;
constexpr const char* kPrompt = "dhcp> ";
constexpr const char* kBanner =
    "\r\nDHCPServer console over SSH. Type 'help' for available commands.\r\n";

namespace dhcp {
namespace net {

namespace {

/// @brief What one connection's callbacks share.
///
/// The callback structures live here, not in a function-local `static`: libssh
/// keeps the **pointer** (`session->server_callbacks = cb`), so two sessions
/// sharing one structure would both be served with the last session's
/// `userdata`.
struct SessionState {
    ssh_server_callbacks_struct serverCallbacks{};
    ssh_channel_callbacks_struct channelCallbacks{};
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    /// The peer as text: what the journal names, and what the page would show.
    std::string clientIp;
    /// The name the client asked to log in as; empty until it asks.
    std::string user;
    bool authenticated = false;
    bool shellRequested = false;
    int attempts = 0;
    bool givenUp = false;
    /// When the session was accepted, in ticks — the duration is measured from
    /// here, not from the first command.
    TickType_t acceptedAt = 0;
};

/// @brief The SSH channel as the console's output sink.
class ChannelOutput : public ::dhcp::menu::IConsoleOutput {
public:
    explicit ChannelOutput(ssh_channel channel) : channel_(channel) {}

    void write(const string& text) override
    {
        size_t offset = 0;
        while (offset < text.size()) {
            const int sent = ssh_channel_write(channel_, text.data() + offset,
                                               static_cast<uint32_t>(text.size() - offset));
            if (sent <= 0) return;
            offset += static_cast<size_t>(sent);
        }
    }

private:
    ssh_channel channel_;
};

int onAuthNone(ssh_session session, const char* user, void* userdata)
{
    (void)user;
    (void)userdata;
    // The first question almost every client asks. Answering it with "denied"
    // and naming the method is what makes the client send a password.
    ssh_set_auth_methods(session, SSH_AUTH_METHOD_PASSWORD);
    return SSH_AUTH_DENIED;
}

int onAuthPassword(ssh_session session, const char* user, const char* password,
                   void* userdata)
{
    (void)session;
    SessionState* state = static_cast<SessionState*>(userdata);
    if (state == nullptr || state->givenUp) return SSH_AUTH_DENIED;

    // The name is remembered even when the password is wrong: a journal line
    // about a refused attempt without the name it was made for is half a line.
    if (user) state->user = user;

    const auto sec = ::dhcp::core::Config::instance().getSecurity();
    const bool ok = ::dhcp::core::credentialsMatch(
        user ? user : "", password ? password : "", sec.username, sec.password);
    if (ok) {
        state->authenticated = true;
        ESP_LOGI(TAG, "session authenticated");
        return SSH_AUTH_SUCCESS;
    }

    state->attempts++;
    ESP_LOGW(TAG, "refused password attempt %d of %d", state->attempts, kAuthAttemptsMax);
    ::dhcp::core::SshLog::instance().noteAuthFailed(state->clientIp, state->attempts,
                                                    kAuthAttemptsMax);
    if (state->attempts >= kAuthAttemptsMax) state->givenUp = true;
    return SSH_AUTH_DENIED;
}

int onPtyRequest(ssh_session session, ssh_channel channel, const char* term,
                 int cols, int rows, int py, int px, void* userdata)
{
    (void)session;
    (void)channel;
    (void)term;
    (void)cols;
    (void)rows;
    (void)py;
    (void)px;
    (void)userdata;
    // The console does its own echoing and line editing, so the PTY is only a
    // promise that a terminal is on the other side.
    return SSH_OK;
}

int onShellRequest(ssh_session session, ssh_channel channel, void* userdata)
{
    (void)session;
    (void)channel;
    SessionState* state = static_cast<SessionState*>(userdata);
    if (state) state->shellRequested = true;
    return SSH_OK;
}

ssh_channel onChannelOpen(ssh_session session, void* userdata)
{
    SessionState* state = static_cast<SessionState*>(userdata);
    if (state == nullptr || state->channel != nullptr) return nullptr;

    ssh_channel channel = ssh_channel_new(session);
    if (channel == nullptr) return nullptr;

    ssh_callbacks_init(&state->channelCallbacks);
    state->channelCallbacks.userdata = state;
    state->channelCallbacks.channel_pty_request_function = onPtyRequest;
    state->channelCallbacks.channel_shell_request_function = onShellRequest;
    ssh_set_channel_callbacks(channel, &state->channelCallbacks);

    state->channel = channel;
    return channel;
}

} // namespace

bool SshServer::available()
{
    return true;
}

SshServer::SshServer(::dhcp::wifi::IWiFiManager& wifi,
                     ::dhcp::web::AuthManager* auth)
    : wifi_(wifi)
    , auth_(auth)
{
}

SshServer::~SshServer()
{
    stop();
}

void SshServer::setLifecycleHooks(function<void()> beforeReboot,
                                  function<void()> factoryReset)
{
    beforeReboot_ = move(beforeReboot);
    factoryReset_ = move(factoryReset);
}

string SshServer::hostKey()
{
    if (!hostKey_.empty()) return hostKey_;

    hostKey_ = ::dhcp::core::Config::instance().getSshHostKey();
    if (!hostKey_.empty()) return hostKey_;

    ssh_pki_ctx context = ssh_pki_ctx_new();
    if (!context) {
        ESP_LOGE(TAG, "could not create a key context");
        return "";
    }
    ssh_key key = nullptr;
    int rc = ssh_pki_generate_key(SSH_KEYTYPE_ED25519, context, &key);
    ssh_pki_ctx_free(context);
    if (rc != SSH_OK || key == nullptr) {
        ESP_LOGE(TAG, "could not generate a host key");
        return "";
    }

    char* base64 = nullptr;
    rc = ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &base64);
    ssh_key_free(key);
    if (rc != SSH_OK || base64 == nullptr) {
        ESP_LOGE(TAG, "could not export the host key");
        return "";
    }
    hostKey_ = base64;
    ssh_string_free_char(base64);
    ::dhcp::core::Config::instance().setSshHostKey(hostKey_);
    ESP_LOGI(TAG, "generated an Ed25519 host key (%u bytes), kept in NVS",
             static_cast<unsigned>(hostKey_.size()));
    return hostKey_;
}

bool SshServer::start(uint16_t port)
{
    if (running_.load()) return true;

    if (ssh_init() != SSH_OK) {
        ESP_LOGE(TAG, "ssh_init failed");
        return false;
    }

    const string key = hostKey();
    if (key.empty()) {
        ESP_LOGE(TAG, "no host key: refusing to listen");
        return false;
    }

    ssh_bind bind = ssh_bind_new();
    if (!bind) {
        ESP_LOGE(TAG, "ssh_bind_new failed");
        return false;
    }

    const string portText = to_string(port);
    const char* bindAddress = "0.0.0.0";
    const char* verbosity = "0";
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, bindAddress);
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT_STR, portText.c_str());
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY_STR, verbosity);
    if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY_STR, key.c_str()) != SSH_OK) {
        const string detail = string("the host key was refused: ") + ssh_get_error(bind);
        ESP_LOGE(TAG, "%s", detail.c_str());
        ::dhcp::core::ErrorLog::instance().errorf(TAG, "%s", detail.c_str());
        ssh_bind_free(bind);
        return false;
    }
    if (ssh_bind_listen(bind) != SSH_OK) {
        // A fault of the server belongs to the error log, not to the journal of
        // sessions: the operator looks for it there when the switch does nothing.
        const string detail = string("listen on port ") + portText + " failed: " +
                              ssh_get_error(bind);
        ESP_LOGE(TAG, "%s", detail.c_str());
        ::dhcp::core::ErrorLog::instance().errorf(TAG, "%s", detail.c_str());
        ssh_bind_free(bind);
        return false;
    }

    bind_ = bind;
    running_.store(true);

    TaskHandle_t handle = nullptr;
    BaseType_t created = xTaskCreateWithCaps(
        &SshServer::acceptTask, "ssh_accept", kAcceptStackBytes, this,
        kAcceptPriority, &handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        // PSRAM is a preference, not a requirement: a stack in internal RAM is
        // better than a server that does not run.
        ESP_LOGW(TAG, "no PSRAM task stack; falling back to internal RAM");
        created = xTaskCreate(&SshServer::acceptTask, "ssh_accept", kAcceptStackBytes,
                              this, kAcceptPriority, &handle);
    }
    if (created != pdPASS) {
        running_.store(false);
        ssh_bind_free(bind);
        bind_ = nullptr;
        ESP_LOGE(TAG, "could not create the accept task");
        return false;
    }
    acceptTask_ = handle;

    ESP_LOGI(TAG, "listening on 0.0.0.0:%u", static_cast<unsigned>(port));
    return true;
}

void SshServer::stop()
{
    if (!running_.exchange(false)) return;

    // `ssh_bind_accept()` blocks; shutting the listening socket down wakes it.
    if (bind_) {
        const socket_t fd = ssh_bind_get_fd(static_cast<ssh_bind>(bind_));
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }

    // The accept task owns the bind handle and frees it on its way out.
    for (int waited = 0; acceptTask_ != nullptr && waited < kStopWaitMs; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (acceptTask_ != nullptr) {
        ESP_LOGW(TAG, "the accept task did not stop in time");
        if (bind_) {
            ssh_bind_free(static_cast<ssh_bind>(bind_));
            bind_ = nullptr;
        }
        acceptTask_ = nullptr;
    }

    // Sessions notice `running_` on their next poll and close themselves.
    for (int waited = 0; activeSessions_.load() > 0 && waited < kStopWaitMs; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "SSH server stopped");
}

void SshServer::acceptTask(void* arg)
{
    static_cast<SshServer*>(arg)->acceptLoop();
}

void SshServer::acceptLoop()
{
    const auto& cfgMgr = ::dhcp::core::Config::instance();

    while (running_.load()) {
        ssh_session session = ssh_new();
        if (!session) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (ssh_bind_accept(static_cast<ssh_bind>(bind_), session) != SSH_OK) {
            ssh_free(session);
            if (!running_.load()) break;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!running_.load()) {
            ssh_disconnect(session);
            ssh_free(session);
            break;
        }

        // The LAN filter judges the socket, not anything the client says.
        string clientIp;
        {
            const auto sshCfg = cfgMgr.getSsh();
            const auto dhcpCfg = cfgMgr.getDhcp();
            ::dhcp::core::SubnetFilter filter;
            filter.apply({ sshCfg.allowOwnSubnet, dhcpCfg.serverIp, dhcpCfg.subnet });

            sockaddr_in peer;
            memset(&peer, 0, sizeof(peer));
            socklen_t peerLen = sizeof(peer);
            const socket_t fd = ssh_get_fd(session);
            const bool known = fd >= 0 &&
                getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0;
            if (known) {
                char text[INET_ADDRSTRLEN] = { 0 };
                inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
                clientIp = text;
            }
            if (known && !filter.contains(ntohl(peer.sin_addr.s_addr))) {
                ESP_LOGW(TAG, "refused a session from outside the own subnet");
                ::dhcp::core::SshLog::instance().noteSubnetRefused(clientIp);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }
        }

        if (activeSessions_.load() >= kMaxSessions) {
            ESP_LOGW(TAG, "refused a session: %d of %d already running",
                     activeSessions_.load(), kMaxSessions);
            ::dhcp::core::SshLog::instance().noteSessionLimit(
                clientIp, static_cast<uint32_t>(kMaxSessions));
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        auto* sessionArg = new (std::nothrow) SessionArg{ this, session, clientIp };
        if (!sessionArg) {
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        activeSessions_++;
        TaskHandle_t handle = nullptr;
        BaseType_t created = xTaskCreateWithCaps(
            &SshServer::sessionTask, "ssh_session", kSessionStackBytes, sessionArg,
            kSessionPriority, &handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            ESP_LOGW(TAG, "no PSRAM for a session stack; trying internal RAM");
            created = xTaskCreate(&SshServer::sessionTask, "ssh_session",
                                  kSessionStackBytes, sessionArg,
                                  kSessionPriority, &handle);
        }
        if (created != pdPASS) {
            ESP_LOGW(TAG, "could not start a session task");
            activeSessions_--;
            delete sessionArg;
            ssh_disconnect(session);
            ssh_free(session);
        }
    }

    if (bind_) {
        ssh_bind_free(static_cast<ssh_bind>(bind_));
        bind_ = nullptr;
    }
    acceptTask_ = nullptr;
    vTaskDelete(nullptr);
}

void SshServer::sessionTask(void* arg)
{
    SessionArg* sessionArg = static_cast<SessionArg*>(arg);
    sessionArg->server->runSession(sessionArg->session, sessionArg->clientIp);
    delete sessionArg;
    vTaskDelete(nullptr);
}

void SshServer::runSession(void* sessionRaw, const string& clientIp)
{
    ssh_session session = static_cast<ssh_session>(sessionRaw);
    SessionState state;
    state.session = session;
    state.clientIp = clientIp;
    state.acceptedAt = xTaskGetTickCount();
    bool loggedOpen = false;

    do {
        if (ssh_handle_key_exchange(session) != SSH_OK) {
            ESP_LOGW(TAG, "key exchange failed: %s", ssh_get_error(session));
            break;
        }

        ssh_callbacks_init(&state.serverCallbacks);
        state.serverCallbacks.userdata = &state;
        state.serverCallbacks.auth_none_function = onAuthNone;
        state.serverCallbacks.auth_password_function = onAuthPassword;
        state.serverCallbacks.channel_open_request_session_function = onChannelOpen;
        ssh_set_server_callbacks(session, &state.serverCallbacks);

        // Authentication and the channel request arrive through `ssh_event`;
        // polling it until the client has asked for a shell is the whole of this
        // loop. A give-up closes the connection instead of counting forever.
        ssh_event event = ssh_event_new();
        if (event == nullptr || ssh_event_add_session(event, session) != SSH_OK) {
            if (event) ssh_event_free(event);
            break;
        }
        while (running_.load() && !state.shellRequested && !state.givenUp) {
            if (ssh_event_dopoll(event, kSessionPollTimeoutMs) == SSH_ERROR) break;
        }
        ssh_event_free(event);

        if (!state.authenticated || !state.shellRequested || state.channel == nullptr) {
            break;
        }

        // ─── The console itself: the same commands as the serial menu ───
        ssh_channel channel = state.channel;
        // A session is a session once somebody is in it: the line is written after
        // the password, not when the socket was accepted, so a port scan does not
        // fill the journal with openings nobody made.
        ::dhcp::core::SshLog::instance().noteSessionOpen(clientIp, state.user);
        loggedOpen = true;
        ChannelOutput out(channel);
        ::dhcp::menu::ConsoleCommands commands(out, wifi_, auth_);
        commands.setLifecycleHooks(beforeReboot_, factoryReset_);
        out.write(kBanner);
        out.write(kPrompt);

        string line;
        char buffer[kReadBufBytes];
        while (running_.load() && ssh_channel_is_open(channel) &&
               !ssh_channel_is_eof(channel)) {
            const int got = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
            if (got == SSH_AGAIN || got == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (got < 0) break;

            bool sessionEnded = false;
            for (int i = 0; i < got && !sessionEnded; i++) {
                const char c = buffer[i];
                if (c == '\r' || c == '\n') {
                    out.write("\r\n");
                    if (line.empty()) {
                        out.write(kPrompt);
                        continue;
                    }
                    const bool keepGoing = commands.processLine(line);
                    line.clear();
                    if (!keepGoing) {
                        out.write("Goodbye.\r\n");
                        ssh_channel_send_eof(channel);
                        ssh_channel_close(channel);
                        sessionEnded = true;
                        break;
                    }
                    out.write(kPrompt);
                } else if (c == kBackspaceDel || c == kBackspaceBs) {
                    if (!line.empty()) {
                        line.pop_back();
                        out.write("\b \b");
                    }
                } else if (c >= 0x20 && line.size() < kLineMaxChars) {
                    line += c;
                    out.write(string(1, c));
                }
            }
            if (sessionEnded) break;
        }
    } while (false);

    if (state.channel) {
        if (ssh_channel_is_open(state.channel)) ssh_channel_close(state.channel);
        ssh_channel_free(state.channel);
    }
    if (loggedOpen) {
        const uint32_t seconds = static_cast<uint32_t>(
            (xTaskGetTickCount() - state.acceptedAt) * portTICK_PERIOD_MS / 1000);
        ::dhcp::core::SshLog::instance().noteSessionClose(clientIp, seconds);
    }
    ssh_disconnect(session);
    ssh_free(session);
    activeSessions_--;
    ESP_LOGI(TAG, "session closed (%d still active)", activeSessions_.load());
}

} // namespace net
} // namespace dhcp

#else // !CONFIG_IDF_TARGET_ESP32P4

namespace dhcp {
namespace net {

bool SshServer::available()
{
    return false;
}

SshServer::SshServer(::dhcp::wifi::IWiFiManager& wifi,
                     ::dhcp::web::AuthManager* auth)
    : wifi_(wifi)
    , auth_(auth)
{
}

SshServer::~SshServer() = default;

void SshServer::setLifecycleHooks(std::function<void()>, std::function<void()>) {}

bool SshServer::start(uint16_t)
{
    ESP_LOGW("SshServer", "SSH is not part of this build (ESP32-P4 only)");
    return false;
}

void SshServer::stop() {}

std::string SshServer::hostKey()
{
    return "";
}

void SshServer::acceptTask(void*) {}
void SshServer::sessionTask(void*) {}
void SshServer::acceptLoop() {}
void SshServer::runSession(void*, const std::string&) {}

} // namespace net
} // namespace dhcp

#endif // CONFIG_IDF_TARGET_ESP32P4
