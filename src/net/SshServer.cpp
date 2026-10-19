#include "SshServer.h"

#include "../core/Config.h"
#include "../menu/ConsoleCommands.h"

#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32P4

#include "libssh/libssh.h"
#include "libssh/server.h"
#include "libssh/callbacks.h"

#include "SshStage.h"

#include "../core/Credentials.h"
#include "../core/DeviceLog.h"
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
//
// The stacks are not guesses. This port already proved how deep libssh goes: the
// first start of the listener needed ~3.9 KB of frames in one task (`app_main` →
// `ssh_bind_listen` → `bind_socket` → lwIP `getaddrinfo`) and overflowed a 3584-byte
// stack. Accepting a connection and running the key exchange are deeper still —
// mbedTLS does the curve arithmetic and the Ed25519 signature on that stack — so
// the accept task gets 12 KB and a session 32 KB; both live in PSRAM, which is
// what makes the size affordable (19.10.2026: a client connecting at all panicked
// the device, and the journal said so on the next boot).
constexpr uint32_t kAcceptStackBytes = 12288;
// Internal RAM, like the accept task: this is the only place in the firmware that
// used an external (PSRAM) stack, and the rule for those is unforgiving — a task
// whose stack lives outside the chip cannot run while the flash cache is disabled,
// which is exactly what every log write to the FAT does. That is why the crash
// moved from call to call as more logging was added and never named a culprit.
constexpr uint32_t kSessionStackBytes = 16384;
/** How long the accept loop waits for a connection before it re-reads `running_`. */
constexpr uint32_t kAcceptPollMs = 200;
// The session task runs *below* the accept loop on purpose: at equal priority it
// could preempt the loop the instant FreeRTOS starts it, and then a crash inside
// the key exchange would be reported as a crash in the peer checks, because the
// loop had not written its own breadcrumb yet. One step lower makes the order
// unambiguous: the loop always finishes its bookkeeping first.
constexpr UBaseType_t kAcceptPriority = 4;
constexpr UBaseType_t kSessionPriority = 3;
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

/**
 * @brief libssh's own words, into the device journal.
 *
 * The key exchange of 19.10.2026 panicked the device inside libssh, where nothing
 * of ours runs. This callback is the library's account of what it was doing, and
 * it is written to `Device.log` — the one file that survives a reboot.
 */
void libsshToJournal(int priority, const char* function, const char* buffer, void*)
{
    (void)priority;
    ::dhcp::core::DeviceLog::instance().noteLibssh(function ? function : "",
                                                   buffer ? buffer : "");
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

    auto& errors = ::dhcp::core::ErrorLog::instance();

    // A stop that could not take the accept task down leaves a bind handle owned by
    // that task: starting again would create a second one over it and the stuck task
    // would free the wrong object. Refusing loudly is the honest answer.
    if (acceptTask_ != nullptr) {
        errors.errorf(TAG, "the previous SSH accept task never stopped: the server "
                           "stays off until the device is restarted");
        return false;
    }

    if (ssh_init() != SSH_OK) {
        ESP_LOGE(TAG, "ssh_init failed");
        errors.errorf(TAG, "ssh_init failed: the SSH server did not start");
        return false;
    }

    const string key = hostKey();
    if (key.empty()) {
        ESP_LOGE(TAG, "no host key: refusing to listen");
        errors.errorf(TAG, "no host key: the SSH server did not start");
        return false;
    }

    ssh_bind bind = ssh_bind_new();
    if (!bind) {
        ESP_LOGE(TAG, "ssh_bind_new failed");
        errors.errorf(TAG, "ssh_bind_new failed: the SSH server did not start");
        return false;
    }

    const string portText = to_string(port);
    const char* bindAddress = "0.0.0.0";
    // 3 = protocol level: every key-exchange step libssh takes, which is the only
    // account of the crash inside `ssh_handle_key_exchange()` that reaches a file.
    const char* verbosity = "3";
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, bindAddress);
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT_STR, portText.c_str());
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY_STR, verbosity);
    ssh_set_log_callback(libsshToJournal);
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

    // The filter's settings are read here, once, on the caller's task: the accept
    // loop must not touch NVS while a client waits on it.
    refreshFilter();

    TaskHandle_t handle = nullptr;
    // Internal RAM on purpose: this task reads no flash now, but it is one
    // `malloc` away from a task that does, and the external-stack rule ("no flash
    // operation while the cache is off") is not worth a reboot to discover. 12 KB
    // of internal RAM for the lifetime of the server is affordable.
    BaseType_t created = xTaskCreate(&SshServer::acceptTask, "ssh_accept",
                                     kAcceptStackBytes, this, kAcceptPriority, &handle);
    if (created != pdPASS) {
        running_.store(false);
        ssh_bind_free(bind);
        bind_ = nullptr;
        ESP_LOGE(TAG, "could not create the accept task");
        errors.errorf(TAG, "could not create the accept task on port %u: the "
                           "SSH server did not start",
                      static_cast<unsigned>(port));
        return false;
    }
    acceptTask_ = handle;

    // The first line an operator looks for in the journal: a server that never
    // wrote it, and never wrote a session either, did not run at all.
    ::dhcp::core::SshLog::instance().noteServerStart(port);
    ESP_LOGI(TAG, "listening on 0.0.0.0:%u", static_cast<unsigned>(port));
    return true;
}

void SshServer::stop()
{
    if (!running_.exchange(false)) return;

    // No `shutdown()` and no freeing from here. The accept loop polls its socket
    // with a short timeout (`kAcceptPollMs`), so it notices `running_ == false` by
    // itself and takes its own `ssh_bind` down on its own task. The old code shut
    // the listening socket down from *this* task and then freed the handle after a
    // timeout if the loop had not woken — a use-after-free waiting for a client to
    // connect, which is what turned the switch off into a panic on 19.10.2026.
    sshStageSet(SshStage::Stopping);
    for (int waited = 0; acceptTask_ != nullptr && waited < kStopWaitMs; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (acceptTask_ != nullptr) {
        // The loop is stuck inside libssh. Freeing the handle under it would be
        // exactly the bug this rewrite removes, so it is left alone: `start()`
        // refuses to run until the task is gone, and the operator is told why.
        const string detail = "the SSH accept task did not stop; the server cannot be "
                             "started again until the device is restarted";
        ESP_LOGE(TAG, "%s", detail.c_str());
        ::dhcp::core::ErrorLog::instance().errorf(TAG, "%s", detail.c_str());
        return;
    }
    sshStageSet(SshStage::StopListening);

    // Sessions notice `running_` on their next poll and close themselves.
    sshStageSet(SshStage::StopSessions);
    for (int waited = 0; activeSessions_.load() > 0 && waited < kStopWaitMs; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    sshStageSet(SshStage::StopDone);
    ::dhcp::core::SshLog::instance().noteServerStop();
    ESP_LOGI(TAG, "SSH server stopped");
    // Nothing of this server is running any more: a stage left behind would be
    // reported by the next boot as a crash that never happened.
    sshStageSet(SshStage::None);
}

void SshServer::refreshFilter()
{
    // Both settings live in NVS, and NVS is a flash access: it happens on the task
    // that saved them (or on the one starting the server), never on the accept
    // loop — that was the 19.10.2026 panic: the device died in this read, between
    // `accepted` and the session task, with the client's socket open.
    const auto& cfgMgr = ::dhcp::core::Config::instance();
    const auto sshCfg = cfgMgr.getSsh();
    const auto dhcpCfg = cfgMgr.getDhcp();
    filter_.apply({ sshCfg.allowOwnSubnet, dhcpCfg.serverIp, dhcpCfg.subnet });
}

void SshServer::acceptTask(void* arg)
{
    static_cast<SshServer*>(arg)->acceptLoop();
}

void SshServer::acceptLoop()
{
    while (running_.load()) {
        // Poll the listener instead of blocking inside `accept()`: this loop has to
        // notice a stop on its own, and it must own its `ssh_bind` (freeing it from
        // `stop()` while this task is inside libssh was the 19.10.2026 panic).
        const socket_t listenFd = bind_ ? ssh_bind_get_fd(static_cast<ssh_bind>(bind_))
                                        : -1;
        if (listenFd < 0) break;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenFd, &readable);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = kAcceptPollMs * 1000;
        const int ready = select(listenFd + 1, &readable, nullptr, nullptr, &timeout);
        if (ready < 0) {                 // interrupted or the socket is gone
            vTaskDelay(pdMS_TO_TICKS(kAcceptPollMs));
            continue;
        }
        if (ready == 0) continue;        // nothing to accept: back to `running_`

        if (activeSessions_.load() == 0) sshStageSet(SshStage::Accepting);
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
        sshStageSet(SshStage::Accepted);
        if (!running_.load()) {
            ssh_disconnect(session);
            ssh_free(session);
            break;
        }

        // The peer, from the socket itself, before anything else looks at it.
        // The line written next is the journal's first breadcrumb: a device that
        // dies inside libssh's accept leaves nothing, one that dies in the filter
        // or in the task creation leaves this line, and one that dies in the key
        // exchange leaves both it and the one after the task exists.
        string clientIp;
        {
            sockaddr_in peer;
            memset(&peer, 0, sizeof(peer));
            socklen_t peerLen = sizeof(peer);
            const socket_t fd = ssh_get_fd(session);
            if (fd >= 0 &&
                getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0) {
                char text[INET_ADDRSTRLEN] = { 0 };
                inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
                clientIp = text;
                sshStageSet(SshStage::PeerKnown);

                // The LAN filter judges the socket, not anything the client says.
                // Its settings were read when the server started or when they were
                // saved (refreshFilter): no NVS here, on the task a client is
                // waiting on.
                if (!filter_.contains(ntohl(peer.sin_addr.s_addr))) {
                    ESP_LOGW(TAG, "refused a session from outside the own subnet");
                    ::dhcp::core::SshLog::instance().noteSubnetRefused(clientIp);
                    ssh_disconnect(session);
                    ssh_free(session);
                    continue;
                }
                sshStageSet(SshStage::FilterChecked);
            }
        }
        ::dhcp::core::SshLog::instance().noteSessionAccepted(clientIp);

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
        sshStageSet(SshStage::TaskCreating);
        const BaseType_t created = xTaskCreate(
            &SshServer::sessionTask, "ssh_session", kSessionStackBytes, sessionArg,
            kSessionPriority, &handle);
        if (created != pdPASS) {
            ESP_LOGW(TAG, "could not start a session task");
            // A connection that dies here never reaches a journal line of its own:
            // the client only sees "connection reset", so the journal has to say
            // what happened, and Errors.log has to carry the resource side.
            ::dhcp::core::SshLog::instance().noteSessionFault(
                clientIp, "no memory for a session task");
            ::dhcp::core::ErrorLog::instance().errorf(
                TAG, "no memory for a session task: the connection from %s was "
                     "dropped",
                clientIp.empty() ? "(unknown address)" : clientIp.c_str());
            activeSessions_--;
            delete sessionArg;
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        // The second breadcrumb: a session task exists and the client is served by
        // it from here on — and it cannot have run yet, because it sits one
        // priority step below this loop.
        ::dhcp::core::SshLog::instance().noteSessionStarted(clientIp);
        sshStageSet(SshStage::TaskCreated);
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
    // The session task owns the breadcrumb from its first instruction: from here
    // until the console opens, a crash is reported as "the key exchange".
    sshStageSet(SshStage::KeyExchange);

    ssh_session session = static_cast<ssh_session>(sessionRaw);
    SessionState state;
    state.session = session;
    state.clientIp = clientIp;
    state.acceptedAt = xTaskGetTickCount();
    bool loggedOpen = false;

    do {
        if (ssh_handle_key_exchange(session) != SSH_OK) {
            const string reason = ssh_get_error(session);
            ESP_LOGW(TAG, "key exchange failed: %s", reason.c_str());
            // The line this report was missing: the client said "Connection reset",
            // the journal was empty, and the reason existed only on the UART.
            ::dhcp::core::SshLog::instance().noteSessionFault(clientIp, reason);
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
        sshStageSet(SshStage::Shell);
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
    // The last session out clears the breadcrumb: a stage left behind would be
    // reported by the next boot as a crash that never happened.
    if (activeSessions_.load() == 0) sshStageSet(SshStage::None);
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

void SshServer::refreshFilter() {}

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
