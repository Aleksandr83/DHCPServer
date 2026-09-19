#include "WebServer.h"
#include "RestApi.h"
#include "../core/ErrorLog.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_http_server.h"

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
constexpr size_t kHttpdStackBytes = 8192;    // the server task's stack (TLS)
constexpr size_t kMaxOpenSockets = 16;       // connections served at once
constexpr int kKeepAliveIdleSec = 5;         // idle keep-alive timeout
constexpr int kKeepAliveIntervalSec = 5;     // ...and its probe interval
constexpr int kLingerTimeoutSec = 1;         // how long a close may linger
constexpr size_t kStaticFileChunkBytes = 512; // one chunk of a served file

} // namespace

namespace dhcp {
namespace web {

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

    // Initialize RestApi with subsystem references
    RestApi::init(&wifi_, &dhcpSrv_, &dnsSrv_, &timeSrv_, &auth_, &fileMgr_);

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
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
    config.stack_size = kHttpdStackBytes;
    // Bigger socket pool: the web UI polls /api/status periodically, and with
    // the default pool of 7 (3 reserved for internal use -> only 4 clients)
    // connections pile up in TIME_WAIT and httpd stops accepting
    // ("httpd_accept_conn: error in accept (23)").
    // NOTE: httpd caps max_open_sockets at LWIP_MAX_SOCKETS - 3 (3 are used
    // internally). With LWIP_MAX_SOCKETS=24 the cap is 21; 16 leaves headroom.
    config.max_open_sockets = kMaxOpenSockets;
    // Keep-alive: the browser reuses one connection for the polling instead
    // of opening a new TCP connection every poll.
    config.keep_alive_enable = true;
    config.keep_alive_idle = kKeepAliveIdleSec;
    config.keep_alive_interval = kKeepAliveIntervalSec;
    // Close sockets promptly (avoid TIME_WAIT backlog building up)
    config.enable_so_linger = true;
    config.linger_timeout = kLingerTimeoutSec;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        server_ = nullptr;
        return false;
    }

    // Custom 404 page for missing URIs / files
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, err404Handler);

    registerRoutes();
    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return true;
}

void WebServer::stop()
{
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

bool WebServer::isRunning() const
{
    return server_ != nullptr;
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

void WebServer::registerRoutes()
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

        const esp_err_t err = httpd_register_uri_handler(server_, &uriDesc);
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
    std::string path = req->uri;

    // Default to login.html (auth gate)
    if (path == "/" || path.empty()) {
        path = "/login.html";
    }

    // Prepend SPIFFS base path
    std::string filePath = "/spiffs" + path;

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
