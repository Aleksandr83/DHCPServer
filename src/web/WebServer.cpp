#include "WebServer.h"
#include "RedirectPolicy.h"
#include "RestApi.h"
#include "../core/Config.h"
#include "../core/ErrorLog.h"
#include "../security/CertStore.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace std;

static const char* TAG = "WebServer";

namespace {

/** @brief The name of a route's method, for the log. */
const char* methodName(::dhcp::web::RouteMethod method)
{
    return (method == ::dhcp::web::RouteMethod::Post) ? "POST" : "GET";
}

} // namespace

namespace {

// Rule 39: the numbers the httpd configuration used to spell out.
constexpr uint16_t kHttpPort = 80;           // the plain interface, always on
constexpr uint16_t kHttpsPort = 443;         // the TLS interface, on request
// httpd binds its control socket (127.0.0.1:ctrl_port) without SO_REUSEADDR, so
// two servers cannot share one. The plain server keeps the IDF default and the
// TLS one takes the next port — the one HTTPD_SSL_CONFIG_DEFAULT() asks for, and
// which the httpd block built below would otherwise overwrite with the default.
// On one port the second server dies in cs_create_ctrl_sock() with ESP_FAIL and
// the interface only says "the HTTPS server could not be started" (stage 166).
constexpr uint16_t kHttpCtrlPort = ESP_HTTPD_DEF_CTRL_PORT;
constexpr uint16_t kHttpsCtrlPort = ESP_HTTPD_DEF_CTRL_PORT + 1;
constexpr size_t kHttpdStackBytes = 8192;    // the plain server task's stack
// The TLS handshake runs on the same task as the handlers and costs more than
// the handler work itself, so the TLS server gets half again as much stack —
// with a 4 KB default a handshake alone was enough to overflow it.
constexpr size_t kHttpsStackBytes = 12288;
constexpr size_t kMaxOpenSockets = 16;       // connections served at once
// The TLS server gets a much smaller pool, and not by taste: every open TLS
// connection holds its own content buffers (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
// 16 KB plus 4 KB out) on top of its SSL context, so sixteen of them would ask
// for more heap than the device has. Failing a handshake is worse than making a
// sixth browser wait, so the plain server keeps its 16 and the TLS one takes 6.
constexpr size_t kHttpsMaxOpenSockets = 6;
constexpr int kKeepAliveIdleSec = 5;         // idle keep-alive timeout
constexpr int kKeepAliveIntervalSec = 5;     // ...and its probe interval
constexpr int kLingerTimeoutSec = 1;         // how long a close may linger
constexpr size_t kStaticFileChunkBytes = 512; // one chunk of a served file
// Stage 165: the retry that waits for the device clock. It walks the same path a
// manual switch does, but a switch answers a request that already exists, while
// this one is born from a sync and lives on its own — it gets the plain server's
// stack size, not the switch task's 4096, since it reads the pair from the volume
// and hands the PEM to the TLS server on the way.
constexpr size_t kHttpsRetryStackBytes = kHttpdStackBytes;
// Stage 167: the buffer a `Host` header is read into. A name, its dots and a
// port fit many times over in 128 bytes; a longer header is not an address to
// send a browser to, and the request is then answered as it always was.
constexpr size_t kHostHeaderBytes = 128;
// One step above idle, like the switch task: the real work of both happens
// either on the httpd task or in the short call they make themselves.
constexpr UBaseType_t kHttpsRetryPriority = tskIDLE_PRIORITY + 1;
// The retry is pushed off the notification it was called from: that call arrives
// while the network task is still finishing its sync, and a TLS server start
// allocates and binds. Few hundred milliseconds are nothing next to a listener
// that then serves for days.
constexpr uint32_t kHttpsRetryDelayMs = 500;

} // namespace

namespace dhcp {
namespace web {

// The one server whose routes the plain-port gate guards. Defined here, next to
// the gate that reads it (see WebServer.h).
WebServer* WebServer::s_instance = nullptr;

WebServer::WebServer(::dhcp::wifi::IWiFiManager& wifi,
                     ::dhcp::dhcp::IDhcpServer& dhcpSrv,
                     ::dhcp::dns::DnsServer& dnsSrv,
                     ::dhcp::time::TimeServer& timeSrv,
                     ::dhcp::files::IFileManager& fileMgr)
    : wifi_(wifi)
    , dhcpSrv_(dhcpSrv)
    , dnsSrv_(dnsSrv)
    , timeSrv_(timeSrv)
    , fileMgr_(fileMgr)
{
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::start()
{
    if (server_) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    // Ensure AuthManager has loaded settings from NVS
    auth_.ensureConfigLoaded();

    // Initialize RestApi with subsystem references. The web server itself is
    // handed over as the HTTPS control: the settings handler has to ask whether
    // a certificate can serve before it accepts a switch to HTTPS (stage 158),
    // and a refusal it cannot check would be a promise it cannot keep.
    RestApi::init(&wifi_, &dhcpSrv_, &dnsSrv_, &timeSrv_, &auth_, &fileMgr_, this);

    httpd_config_t config = makeConfig(kHttpPort, kHttpdStackBytes, kMaxOpenSockets, kHttpCtrlPort);
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        server_ = nullptr;
        return false;
    }

    // The gate every plain route is served through reads the server state from
    // here, and it is set only now: the routes that call it do not exist before
    // httpd_start() succeeded.
    s_instance = this;

    // Custom 404 page for missing URIs / files — on the plain port a redirect
    // like every other answer, on the TLS one the page itself (stage 167).
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, err404RedirectHandler);

    registerRoutes(server_, true);
    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);

    // HTTPS is a setting, so the device comes back with the interface it went
    // down with. A stored pair that cannot serve (card removed, certificate
    // expired) is named in the log and the device stays reachable over HTTP:
    // dropping the operator into an interface that does not answer would be the
    // worst outcome this feature could have.
    if (::dhcp::core::Config::instance().getSecurity().httpsEnabled) {
        string status;
        if (httpsAvailable(&status)) {
            startHttps();
        } else {
            // The usual reason a stored pair comes out unusable at boot is the
            // clock: SNTP answers after the servers are up, so the certificate —
            // a statement about time — is judged against the epoch and reads as
            // "not yet valid". The decision is therefore not final: onClockSet()
            // repeats it when a real date exists (stage 165).
            ESP_LOGW(TAG, "HTTPS is enabled in the settings but cannot serve (%s): "
                          "staying on HTTP; the pair is judged again once the "
                          "clock is set", status.c_str());
        }
    }
    return true;
}

void WebServer::stop()
{
    // The TLS server first: it is the one whose clients are waiting on a
    // handshake, and stopping the plain server under them would only add noise.
    stopHttps();
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        // The routes this guard belongs to are gone with the server; a request
        // that is still in flight is answered by the handler it was registered
        // with, which is why the pointer is cleared and not the state.
        s_instance = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

bool WebServer::isRunning() const
{
    return server_ != nullptr;
}

httpd_config_t WebServer::makeConfig(uint16_t port, size_t stackBytes, size_t maxSockets,
                                     uint16_t ctrlPort)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = ctrlPort;
    // Handler slots for the routes this build actually has, taken from the table
    // itself — never a number written by hand (stage 137: the hand-written 81
    // next to 82 routes is how the Version page lost its route).
    size_t routeCount = 0;
    routes(routeCount);
    config.max_uri_handlers = static_cast<int>(RouteTable::slotsFor(routeCount));
    // The settings export/import handlers build large JSON and read big POST
    // bodies on the httpd task — the default 4096-byte stack overflows (panic:
    // LoadProhibited in the FreeRTOS scheduler, stack filled with 0xa5). Raise
    // the httpd task stack; there is plenty of free heap (~150 KB).
    config.stack_size = stackBytes;
    // Bigger socket pool: the web UI polls /api/status periodically, and with
    // the default pool of 7 (3 reserved for internal use -> only 4 clients)
    // connections pile up in TIME_WAIT and httpd stops accepting
    // ("httpd_accept_conn: error in accept (23)").
    // NOTE: httpd caps max_open_sockets at LWIP_MAX_SOCKETS - 3 (3 are used
    // internally). With LWIP_MAX_SOCKETS=24 the cap is 21; 16 leaves headroom.
    config.max_open_sockets = maxSockets;
    // Keep-alive: the browser reuses one connection for the polling instead
    // of opening a new TCP connection every poll.
    config.keep_alive_enable = true;
    config.keep_alive_idle = kKeepAliveIdleSec;
    config.keep_alive_interval = kKeepAliveIntervalSec;
    // Close sockets promptly (avoid TIME_WAIT backlog building up)
    config.enable_so_linger = true;
    config.linger_timeout = kLingerTimeoutSec;
    config.lru_purge_enable = true;
    return config;
}

bool WebServer::httpsAvailable(string* status) const
{
    if (certs_ == nullptr) {
        // A build without a data volume for the pair (the classic ESP32 has
        // neither the internal FAT nor a card slot) says so instead of letting
        // the operator look for a certificate that could never be stored.
        if (status != nullptr) {
            *status = ::dhcp::security::certStatusName(::dhcp::security::CertStatus::NoStore);
        }
        return false;
    }

    const ::dhcp::security::CertInfo info = certs_->info();
    const ::dhcp::security::CertStatus state = ::dhcp::security::certStatus(info);
    if (status != nullptr) *status = ::dhcp::security::certStatusName(state);
    const bool usable = ::dhcp::security::certUsable(state);
    // Remembered here, where the pair is read from its volume anyway, and not by
    // the redirect gate, which may not read a file on every request: the date is
    // all the gate compares with the clock. A pair that cannot serve leaves no
    // date, and no date means no redirect (stage 167).
    rememberPairDeadline(usable ? info.notAfterEpoch : 0);
    return usable;
}

bool WebServer::setHttpsEnabled(bool enabled, string* detail)
{
    if (enabled) {
        string status;
        if (!httpsAvailable(&status)) {
            if (detail != nullptr) {
                *detail = "no usable certificate (" + status + ")";
            }
            return false;
        }
        if (!startHttps()) {
            if (detail != nullptr) *detail = "the HTTPS server could not be started";
            return false;
        }
    } else {
        stopHttps();
    }

    // Written only once the server is actually in the requested state, so a
    // reboot cannot bring back a setting that never worked.
    auto security = ::dhcp::core::Config::instance().getSecurity();
    security.httpsEnabled = enabled;
    ::dhcp::core::Config::instance().setSecurity(security);
    ESP_LOGI(TAG, "HTTPS %s", enabled ? "enabled" : "disabled");
    return true;
}

bool WebServer::startHttps()
{
    if (httpsServer_ != nullptr) return true;
    if (certs_ == nullptr) return false;

    string detail;
    if (!certs_->load(certPem_, keyPem_, &detail)) {
        ESP_LOGE(TAG, "cannot read the certificate pair: %s", detail.c_str());
        return false;
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd = makeConfig(kHttpsPort, kHttpsStackBytes, kHttpsMaxOpenSockets, kHttpsCtrlPort);
    config.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
    config.port_secure = kHttpsPort;
    // The PEM text is read at start time — the HTTPS server copies it into its
    // own buffers (https_server.c) — and the pair itself stays on the volume.
    config.servercert = reinterpret_cast<const uint8_t*>(certPem_.c_str());
    config.servercert_len = certPem_.size() + 1;   // the terminator says "this is PEM"
    config.prvtkey_pem = reinterpret_cast<const uint8_t*>(keyPem_.c_str());
    config.prvtkey_len = keyPem_.size() + 1;

    const esp_err_t err = httpd_ssl_start(&httpsServer_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
        // The toggle answers "could not be started" and no more, so the code
        // itself goes where it can be read without a serial console
        // (Errors.log, stage 123) — stage 166 was diagnosed blind for that
        // very reason.
        if (::dhcp::core::ErrorLog::instance().core()) {
            ::dhcp::core::ErrorLog::instance().errorf("web",
                "httpd_ssl_start failed: %s", esp_err_to_name(err));
        }
        httpsServer_ = nullptr;
        certPem_.clear();
        keyPem_.clear();
        return false;
    }

    httpd_register_err_handler(httpsServer_, HTTPD_404_NOT_FOUND, err404Handler);
    registerRoutes(httpsServer_, false);
    ESP_LOGI(TAG, "HTTPS server started on port %u", static_cast<unsigned>(kHttpsPort));
    return true;
}

void WebServer::stopHttps()
{
    if (httpsServer_ == nullptr) return;
    httpd_ssl_stop(httpsServer_);
    httpsServer_ = nullptr;
    // Nothing is serving the pair any more, so there is nothing to redirect to —
    // and the date is what the plain port's gate looks at.
    rememberPairDeadline(0);
    // The pair itself stays on its volume; these were only the copies the TLS
    // server was reading while it ran.
    certPem_.clear();
    keyPem_.clear();
    ESP_LOGI(TAG, "HTTPS server stopped");
}

void WebServer::onClockSet()
{
    // Called from TimeServer's clock notification, i.e. from the network task
    // while it is still finishing its sync, and it must return there before that
    // task can breathe (see TimeServer::ClockSetCallback): the retry is a task,
    // and this is only the decision whether one is worth starting.
    if (httpsServer_ != nullptr) return;    // the TLS server is already serving
    if (!::dhcp::core::Config::instance().getSecurity().httpsEnabled) return; // nobody asked for TLS
    if (retryPending_) return;              // an earlier retry is still on its way

    retryPending_ = true;
    if (xTaskCreate(httpsRetryTask, "https_retry", kHttpsRetryStackBytes, this,
                    kHttpsRetryPriority, nullptr) != pdPASS) {
        retryPending_ = false;
        ESP_LOGE(TAG, "cannot start the HTTPS retry task");
    }
}

void WebServer::httpsRetryTask(void* arg)
{
    WebServer* self = static_cast<WebServer*>(arg);
    vTaskDelay(pdMS_TO_TICKS(kHttpsRetryDelayMs));
    self->retryPending_ = false;

    // The wait is long enough for a manual switch to have happened: the wish
    // that is retried is the one stored now, not the one read before the delay.
    if (self->httpsServer_ == nullptr &&
        ::dhcp::core::Config::instance().getSecurity().httpsEnabled) {
        string reason;
        if (self->setHttpsEnabled(true, &reason)) {
            ESP_LOGI(TAG, "HTTPS started on the clock it was waiting for");
        } else {
            // The setting is written only once the server is up, so this leaves
            // the wish intact for the next boot instead of hiding a failure.
            ESP_LOGW(TAG, "HTTPS is enabled in the settings but still cannot serve "
                          "(%s): staying on HTTP", reason.c_str());
        }
    }
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────
// Plain HTTP → HTTPS redirect (stage 167)
// ─────────────────────────────────────────────────────

bool WebServer::httpsRedirectActive() const
{
    return redirect::needed(httpsServer_ != nullptr, pairDeadlineEpoch_.load(),
                            static_cast<int64_t>(std::time(nullptr)));
}

void WebServer::rememberPairDeadline(int64_t notAfterEpoch) const
{
    pairDeadlineEpoch_.store(notAfterEpoch);
}

bool WebServer::redirectIfInForce(httpd_req* req)
{
    if (s_instance == nullptr || !s_instance->httpsRedirectActive()) return false;

    char host[kHostHeaderBytes] = { 0 };
    string hostText;
    const size_t hostLen = httpd_req_get_hdr_value_len(req, "Host");
    if (hostLen > 0 && hostLen < sizeof(host) &&
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        hostText = host;
    }

    const string target = redirect::targetFrom(hostText, req->uri, kHttpsPort);
    // A request that named no usable address has nowhere to be sent: answering
    // it the way it would have been answered is the only honest thing left.
    if (target.empty()) return false;

    sendHttpsRedirect(req, target);
    return true;
}

esp_err_t WebServer::httpsRedirectGate(httpd_req* req)
{
    const WebRoute* route = static_cast<const WebRoute*>(req->user_ctx);
    if (route == nullptr) {
        // Cannot happen: registerRoutes() puts the route in user_ctx for every
        // gate it installs. Answering keeps a mistake here from becoming a crash
        // on somebody's request.
        return err404Handler(req, HTTPD_404_NOT_FOUND);
    }

    if (redirectIfInForce(req)) return ESP_OK;
    return route->handler(req);
}

esp_err_t WebServer::err404RedirectHandler(httpd_req* req, httpd_err_code_t err)
{
    (void)err;
    if (redirectIfInForce(req)) return ESP_OK;
    return err404Handler(req, HTTPD_404_NOT_FOUND);
}

esp_err_t WebServer::sendHttpsRedirect(httpd_req* req, const string& target)
{
    // The message is short and in both languages, like the 404 page: whoever
    // sees it has a client that did not follow the redirect, and the address is
    // the only thing that helps then.
    const string link = redirect::htmlEscaped(target);
    const string page =
        "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "<title>HTTPS - DHCPServer</title>"
        "<style>"
        "body{margin:0;font-family:'Segoe UI',Arial,sans-serif;background:#0f172a;"
        "color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;}"
        ".box{text-align:center;padding:2rem;max-width:90vw;}"
        ".box h1{font-size:2.5rem;margin:0;color:#38bdf8;}"
        ".box p{font-size:1.15rem;color:#94a3b8;margin:0.5rem 0 0;}"
        ".box a{display:inline-block;margin-top:1.5rem;padding:0.6rem 1.4rem;border-radius:6px;"
        "background:#38bdf8;color:#0f172a;text-decoration:none;font-weight:500;}"
        "</style></head><body>"
        "<div class=\"box\"><h1>HTTPS</h1>"
        "<p>\u0418\u043d\u0442\u0435\u0440\u0444\u0435\u0439\u0441 \u043e\u0442\u0434\u0430\u0451\u0442\u0441\u044f \u043f\u043e HTTPS / "
        "The interface is served over HTTPS</p>"
        "<a href=\"" + link + "\">" + link + "</a>"
        "</div></body></html>";

    // 301 for a request that repeats itself, 308 for one whose method has to
    // survive the redirect (see RedirectPolicy.h). The operator asked for a
    // permanent redirect: a browser keeps it, which is what makes the second
    // visit go straight to TLS — and why turning HTTPS off later will not bring
    // the plain address back to a browser that already saw this answer.
    // req->method is an int (see httpd_req::method), so it is named through the
    // parser's own table; an unsupported method comes out as such and then gets
    // the code that keeps methods — never the one that turns it into a GET.
    const http_method method = static_cast<http_method>(req->method);
    httpd_resp_set_status(req, redirect::statusLineForMethod(http_method_str(method)));
    httpd_resp_set_hdr(req, "Location", target.c_str());
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, page.c_str());
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// Route registration
// ─────────────────────────────────────────────────────

// The routes this server serves: one row per reachable path, and the httpd's
// handler limit is taken from this array's own size (routes() below). The limit
// used to be written by hand, and a hand-written limit drifts: 82 routes next to
// 81 slots cost the route registered last — /pages/version.html — its slot, and
// that page answered 404 while every other page worked (see RouteTable.h).
const WebRoute WebServer::kRoutes[] = {
    // REST API routes
    { "/api/status",                      RouteMethod::Get,   &WebServer::getStatusHandler },
    { "/api/version",                     RouteMethod::Get,   &WebServer::getVersionHandler },
    { "/api/dhcp/settings",               RouteMethod::Get,   &WebServer::getDhcpSettingsHandler },
    { "/api/dhcp/settings",               RouteMethod::Post,  &WebServer::postDhcpSettingsHandler },
    { "/api/dhcp/static-bindings",        RouteMethod::Get,   &WebServer::getStaticBindingsHandler },
    { "/api/dhcp/static-bindings",        RouteMethod::Post,  &WebServer::postStaticBindingsHandler },
    { "/api/dhcp/allowed",                RouteMethod::Get,   &WebServer::getAllowedComputersHandler },
    { "/api/dhcp/allowed",                RouteMethod::Post,  &WebServer::postAllowedComputersHandler },
    { "/api/dhcp/lookup-name",            RouteMethod::Post,  &WebServer::postLookupClientNameHandler },
    { "/api/dhcp/leases",                 RouteMethod::Get,   &WebServer::getLeasesHandler },
    { "/api/dns/settings",                RouteMethod::Get,   &WebServer::getDnsSettingsHandler },
    { "/api/dns/settings",                RouteMethod::Post,  &WebServer::postDnsSettingsHandler },
    { "/api/dns/local-hosts",             RouteMethod::Get,   &WebServer::getLocalHostsHandler },
    { "/api/dns/local-hosts",             RouteMethod::Post,  &WebServer::postLocalHostsHandler },
    { "/api/security/settings",           RouteMethod::Get,   &WebServer::getSecuritySettingsHandler },
    { "/api/security/settings",           RouteMethod::Post,  &WebServer::postSecuritySettingsHandler },
    // HTTPS certificate pair (Settings → Security → Certificates): the state of
    // the pair on both volumes, and the actions on it (stage 160).
    { "/api/security/certificates",       RouteMethod::Get,   &WebServer::getCertificatesHandler },
    { "/api/security/certificates",       RouteMethod::Post,  &WebServer::postCertificatesHandler },
    { "/api/security/certificates/download", RouteMethod::Get, &WebServer::getCertificateDownloadHandler },
    { "/api/ota/upload",                  RouteMethod::Post,  &WebServer::postOtaUploadHandler },
    { "/api/web/file",                    RouteMethod::Post,  &WebServer::postWebFileHandler },
    { "/api/test-connection",             RouteMethod::Post,  &WebServer::postTestConnectionHandler },
    { "/api/settings/export",             RouteMethod::Get,   &WebServer::getSettingsExportHandler },
    { "/api/settings/import",             RouteMethod::Post,  &WebServer::postSettingsImportHandler },
    { "/api/settings/reset",              RouteMethod::Post,  &WebServer::postSettingsResetHandler },
    { "/api/device/reboot",               RouteMethod::Post,  &WebServer::postRebootHandler },
    { "/api/device/reboot/prepare",       RouteMethod::Post,  &WebServer::postRebootPrepareHandler },
    // Statistics a planned restart asks for (Statistica.dat on FAT): the write
    // runs as a background job, this reports whether it finished or failed.
    { "/api/dns/stats/progress",          RouteMethod::Get,   &WebServer::getStatsProgressHandler },
    // Built-in (PSRAM) DNS cache persistence file (cache.dat on FAT)
    { "/api/dns/internal-cache/file",     RouteMethod::Get,   &WebServer::getInternalCacheFileHandler },
    { "/api/dns/internal-cache/progress", RouteMethod::Get,   &WebServer::getInternalCacheProgressHandler },
    { "/api/dns/internal-cache/save",     RouteMethod::Post,  &WebServer::postInternalCacheSaveHandler },
    { "/api/dns/internal-cache/load",     RouteMethod::Post,  &WebServer::postInternalCacheLoadHandler },
    { "/api/dns/internal-cache/reset",    RouteMethod::Post,  &WebServer::postInternalCacheResetHandler },
    // Time (NTP) server
    { "/api/time/settings",               RouteMethod::Get,   &WebServer::getTimeSettingsHandler },
    { "/api/time/settings",               RouteMethod::Post,  &WebServer::postTimeSettingsHandler },
    { "/api/time/now",                    RouteMethod::Get,   &WebServer::getTimeNowHandler },
    { "/api/time/set",                    RouteMethod::Post,  &WebServer::postTimeSetHandler },
    // File explorer (FAT volumes)
    { "/api/files/volumes",               RouteMethod::Get,   &WebServer::getFileVolumesHandler },
    { "/api/files/list",                  RouteMethod::Get,   &WebServer::getFileListHandler },
    { "/api/files/mkdir",                 RouteMethod::Post,  &WebServer::postFileMkdirHandler },
    { "/api/files/rename",                RouteMethod::Post,  &WebServer::postFileRenameHandler },
    { "/api/files/delete",                RouteMethod::Post,  &WebServer::postFileDeleteHandler },
    { "/api/files/format",                RouteMethod::Post,  &WebServer::postFileFormatHandler },
    { "/api/files/download",              RouteMethod::Get,   &WebServer::getFileDownloadHandler },
    { "/api/files/upload",                RouteMethod::Post,  &WebServer::postFileUploadHandler },
    { "/api/files/upload/offset",         RouteMethod::Get,   &WebServer::getFileUploadOffsetHandler },
    { "/api/files/upload/cancel",         RouteMethod::Post,  &WebServer::postFileUploadCancelHandler },
    { "/api/files/text",                  RouteMethod::Get,   &WebServer::getFileTextHandler },
    { "/api/files/text",                  RouteMethod::Post,  &WebServer::postFileTextHandler },
    { "/api/files/settings",              RouteMethod::Get,   &WebServer::getFileSettingsHandler },
    { "/api/files/settings",              RouteMethod::Post,  &WebServer::postFileSettingsHandler },
    // Read-only volume check (long-running: POST starts it, GET polls the report)
    { "/api/files/check",                 RouteMethod::Post,  &WebServer::postFileCheckHandler },
    { "/api/files/check/cancel",          RouteMethod::Post,  &WebServer::postFileCheckCancelHandler },
    { "/api/files/check",                 RouteMethod::Get,   &WebServer::getFileCheckHandler },
    // Copy / move between volumes (long-running: POST starts it, GET polls)
    { "/api/files/transfer",              RouteMethod::Post,  &WebServer::postFileTransferHandler },
    { "/api/files/transfer/cancel",       RouteMethod::Post,  &WebServer::postFileTransferCancelHandler },
    { "/api/files/transfer",              RouteMethod::Get,   &WebServer::getFileTransferHandler },
    // Task scheduler: one list for every long-running operation.
    { "/api/jobs",                        RouteMethod::Get,   &WebServer::getJobsHandler },
    { "/api/jobs/cancel",                 RouteMethod::Post,  &WebServer::postJobCancelHandler },
    // Static file handlers (explicit routes — wildcards unreliable in ESP-IDF)
    // serves login.html
    { "/",                                RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/login.html",                      RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/index.html",                      RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/header.html",                     RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/footer.html",                     RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/css/style.css",                   RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/js/app.js",                       RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/i18n/ru.json",                    RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/i18n/en.json",                    RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dhcp_setup.html",           RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dhcp_logging.html",         RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dhcp_dns.html",             RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dhcp_static.html",          RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dns_setup.html",            RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dns_logging.html",          RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dns_cache.html",            RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dns_internal.html",         RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/dns_local_hosts.html",      RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/ntp_setup.html",            RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/ntp_logging.html",          RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/security.html",             RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/security_allowed.html",     RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/certs.html",                RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/files.html",                RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/settings_export.html",      RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/settings_import.html",      RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/settings_device.html",      RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/jobs.html",                 RouteMethod::Get,   &WebServer::staticFileHandler },
    { "/pages/version.html",              RouteMethod::Get,   &WebServer::staticFileHandler },
};

const WebRoute* WebServer::routes(size_t& count)
{
    count = sizeof(kRoutes) / sizeof(kRoutes[0]);
    return kRoutes;
}

void WebServer::registerRoutes(httpd_handle_t server, bool redirectPlainTraffic)
{
    size_t count = 0;
    const WebRoute* table = routes(count);

    // The table is data, so it is checked before anything is registered: a row
    // with no URI (or no handler) could only ever answer 404, and a repeated pair
    // of (URI, method) shadows the earlier route without saying anything.
    for (size_t i = 0; i < count; i++) {
        if (!RouteTable::isValid(table[i])) {
            ESP_LOGE(TAG, "Route %zu is unusable (uri=%s, handler=%s)", i,
                     table[i].uri ? table[i].uri : "(none)",
                     table[i].handler ? "set" : "null");
        } else if (RouteTable::repeatsEarlier(table, i)) {
            ESP_LOGE(TAG, "Route %zu (%s) repeats an earlier route with the same method",
                     i, table[i].uri);
        }
    }

    size_t registered = 0;
    size_t failed = 0;
    for (size_t i = 0; i < count; i++) {
        const WebRoute& route = table[i];
        httpd_uri_t uriDesc;
        memset(&uriDesc, 0, sizeof(uriDesc));
        uriDesc.uri      = route.uri;
        uriDesc.method   = (route.method == RouteMethod::Post) ? HTTP_POST : HTTP_GET;
        uriDesc.handler  = route.handler;   // RouteHandler is the httpd signature
        uriDesc.user_ctx = nullptr;
        if (redirectPlainTraffic) {
            // Every plain route goes through one gate, and the route it guards
            // rides along in user_ctx — a field of httpd_uri_t that nothing else
            // in the project reads (stage 167). The TLS server installs the
            // handlers themselves: it is already the address a browser wants.
            uriDesc.handler  = &WebServer::httpsRedirectGate;
            uriDesc.user_ctx = const_cast<WebRoute*>(&route);
        }

        const esp_err_t err = httpd_register_uri_handler(server, &uriDesc);
        if (err == ESP_OK) {
            registered++;
            ESP_LOGD(TAG, "Registered %s %s", methodName(route.method), route.uri);
        } else {
            failed++;
            ESP_LOGE(TAG, "Failed to register %s %s: %s", methodName(route.method),
                     route.uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Registered %zu/%zu routes", registered, count);
    if (failed > 0) {
        // A route with no slot is a 404 the operator will meet later with no way
        // to explain it: say it where it can be read without a serial console
        // (Errors.log, stage 123).
        ESP_LOGE(TAG, "%zu of %zu routes could not be registered — the handler "
                      "limit and the table disagree", failed, count);
        if (::dhcp::core::ErrorLog::instance().core()) {
            ::dhcp::core::ErrorLog::instance().errorf("web",
                "%zu of %zu HTTP routes could not be registered", failed, count);
        }
    }
}
// Static file handler
// ─────────────────────────────────────────────────────

esp_err_t WebServer::staticFileHandler(httpd_req* req)
{
    string path = req->uri;

    // Default to login.html (auth gate)
    if (path == "/" || path.empty()) {
        path = "/login.html";
    }

    // Prepend SPIFFS base path
    string filePath = "/spiffs" + path;

    // Try to open and send the file
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", filePath.c_str());
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_OK;
    }

    // Set content type based on extension
    const char* ext = strrchr(path.c_str(), '.');
    if (ext) {
        if (strcasecmp(ext, ".html") == 0) {
            httpd_resp_set_type(req, "text/html; charset=utf-8");
        } else if (strcasecmp(ext, ".css") == 0) {
            httpd_resp_set_type(req, "text/css; charset=utf-8");
        } else if (strcasecmp(ext, ".js") == 0) {
            httpd_resp_set_type(req, "application/javascript; charset=utf-8");
        } else if (strcasecmp(ext, ".json") == 0) {
            httpd_resp_set_type(req, "application/json; charset=utf-8");
        } else if (strcasecmp(ext, ".png") == 0) {
            httpd_resp_set_type(req, "image/png");
        } else if (strcasecmp(ext, ".ico") == 0) {
            httpd_resp_set_type(req, "image/x-icon");
        } else {
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
        }
    } else {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
    }

    // Read and send file in chunks
    char buf[kStaticFileChunkBytes];
    size_t readBytes;
    while ((readBytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, readBytes) != ESP_OK) {
            fclose(f);
            return ESP_OK;
        }
    }
    fclose(f);

    // End chunked response
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// ─────────────────────────────────────────────────────
// Custom 404 error handler
// ─────────────────────────────────────────────────────

esp_err_t WebServer::err404Handler(httpd_req* req, httpd_err_code_t err)
{
    (void)err;
    const char* page =
        "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "<title>404 - DHCPServer</title>"
        "<style>"
        "body{margin:0;font-family:'Segoe UI',Arial,sans-serif;background:#0f172a;"
        "color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;}"
        ".box{text-align:center;padding:2rem;max-width:90vw;}"
        ".box h1{font-size:5rem;margin:0;color:#38bdf8;}"
        ".box p{font-size:1.15rem;color:#94a3b8;margin:0.5rem 0 0;}"
        ".box a{display:inline-block;margin-top:1.5rem;padding:0.6rem 1.4rem;border-radius:6px;"
        "background:#38bdf8;color:#0f172a;text-decoration:none;font-weight:500;}"
        "</style></head><body>"
        "<div class=\"box\"><h1>404</h1>"
        "<p>\u0421\u0442\u0440\u0430\u043d\u0438\u0446\u0430 \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d\u0430 / Page not found</p>"
        "<a href=\"/index.html\">\u041d\u0430 \u0433\u043b\u0430\u0432\u043d\u0443\u044e / Home</a>"
        "</div></body></html>";

    httpd_resp_set_status(req, HTTPD_404);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

} // namespace web
} // namespace dhcp
