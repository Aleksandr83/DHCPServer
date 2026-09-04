#include "WebServer.h"
#include "RestApi.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_http_server.h"

static const char* TAG = "WebServer";

namespace dhcp {
namespace web {

WebServer::WebServer(::dhcp::wifi::IWiFiManager& wifi,
                     ::dhcp::dhcp::IDhcpServer& dhcpSrv,
                     ::dhcp::dns::DnsServer& dnsSrv)
    : wifi_(wifi)
    , dhcpSrv_(dhcpSrv)
    , dnsSrv_(dnsSrv)
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
    RestApi::init(&wifi_, &dhcpSrv_, &dnsSrv_, &auth_);

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 48;
    // The settings export/import handlers build large JSON and read big POST
    // bodies on the httpd task — the default 4096-byte stack overflows (panic:
    // LoadProhibited in the FreeRTOS scheduler, stack filled with 0xa5). Raise
    // the httpd task stack; there is plenty of free heap (~150 KB).
    config.stack_size = 8192;
    // Bigger socket pool: the web UI polls /api/status periodically, and with
    // the default pool of 7 (3 reserved for internal use -> only 4 clients)
    // connections pile up in TIME_WAIT and httpd stops accepting
    // ("httpd_accept_conn: error in accept (23)").
    // NOTE: httpd caps max_open_sockets at LWIP_MAX_SOCKETS - 3 (3 are used
    // internally). With LWIP_MAX_SOCKETS=24 the cap is 21; 16 leaves headroom.
    config.max_open_sockets = 16;
    // Keep-alive: the browser reuses one connection for the polling instead
    // of opening a new TCP connection every poll.
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    // Close sockets promptly (avoid TIME_WAIT backlog building up)
    config.enable_so_linger = true;
    config.linger_timeout = 1;
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

void WebServer::registerRoutes()
{
    // Helper lambda to register a route
    auto reg = [this](const char* uri, httpd_method_t method,
                      esp_err_t (*handler)(httpd_req*)) {
        httpd_uri_t uriDesc;
        memset(&uriDesc, 0, sizeof(uriDesc));
        uriDesc.uri     = uri;
        uriDesc.method  = method;
        uriDesc.handler = handler;
        uriDesc.user_ctx = nullptr;
        esp_err_t err = httpd_register_uri_handler(server_, &uriDesc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s %s: %s",
                     (method == HTTP_GET ? "GET" : "POST"),
                     uri, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Registered %s %s",
                     (method == HTTP_GET ? "GET" : "POST"), uri);
        }
    };

    // REST API routes
    reg("/api/status",               HTTP_GET,   getStatusHandler);
    reg("/api/version",              HTTP_GET,   getVersionHandler);
    reg("/api/dhcp/settings",        HTTP_GET,   getDhcpSettingsHandler);
    reg("/api/dhcp/settings",        HTTP_POST,  postDhcpSettingsHandler);
    reg("/api/dhcp/static-bindings", HTTP_GET,   getStaticBindingsHandler);
    reg("/api/dhcp/static-bindings", HTTP_POST,  postStaticBindingsHandler);
    reg("/api/dhcp/leases",          HTTP_GET,   getLeasesHandler);
    reg("/api/dns/settings",         HTTP_GET,   getDnsSettingsHandler);
    reg("/api/dns/settings",         HTTP_POST,  postDnsSettingsHandler);
    reg("/api/dns/local-hosts",      HTTP_GET,   getLocalHostsHandler);
    reg("/api/dns/local-hosts",      HTTP_POST,  postLocalHostsHandler);
    reg("/api/security/settings",    HTTP_GET,   getSecuritySettingsHandler);
    reg("/api/security/settings",    HTTP_POST,  postSecuritySettingsHandler);
    reg("/api/ota/upload",           HTTP_POST,  postOtaUploadHandler);
    reg("/api/web/file",             HTTP_POST,  postWebFileHandler);
    reg("/api/test-connection",      HTTP_POST,  postTestConnectionHandler);
    reg("/api/settings/export",      HTTP_GET,   getSettingsExportHandler);
    reg("/api/settings/import",      HTTP_POST,  postSettingsImportHandler);
    reg("/api/settings/reset",       HTTP_POST,  postSettingsResetHandler);
    reg("/api/device/reboot",        HTTP_POST,  postRebootHandler);

    // Static file handlers (explicit routes — wildcards unreliable in ESP-IDF)
    reg("/", HTTP_GET, staticFileHandler);         // serves login.html
    reg("/login.html", HTTP_GET, staticFileHandler);
    reg("/index.html", HTTP_GET, staticFileHandler);
    reg("/header.html", HTTP_GET, staticFileHandler);
    reg("/footer.html", HTTP_GET, staticFileHandler);
    reg("/css/style.css", HTTP_GET, staticFileHandler);
    reg("/js/app.js", HTTP_GET, staticFileHandler);
    reg("/i18n/ru.json", HTTP_GET, staticFileHandler);
    reg("/i18n/en.json", HTTP_GET, staticFileHandler);
    reg("/pages/dhcp_setup.html", HTTP_GET, staticFileHandler);
    reg("/pages/dhcp_logging.html", HTTP_GET, staticFileHandler);
    reg("/pages/dhcp_dns.html", HTTP_GET, staticFileHandler);
    reg("/pages/dhcp_static.html", HTTP_GET, staticFileHandler);
    reg("/pages/dns_setup.html", HTTP_GET, staticFileHandler);
    reg("/pages/dns_logging.html", HTTP_GET, staticFileHandler);
    reg("/pages/dns_cache.html", HTTP_GET, staticFileHandler);
    reg("/pages/dns_local_hosts.html", HTTP_GET, staticFileHandler);
    reg("/pages/security.html", HTTP_GET, staticFileHandler);
    reg("/pages/settings_export.html", HTTP_GET, staticFileHandler);
    reg("/pages/settings_import.html", HTTP_GET, staticFileHandler);
    reg("/pages/settings_device.html", HTTP_GET, staticFileHandler);
    reg("/pages/version.html", HTTP_GET, staticFileHandler);
}

// ─────────────────────────────────────────────────────
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
    char buf[512];
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
