#ifndef DHCP_WEB_WEBSERVER_H
#define DHCP_WEB_WEBSERVER_H

#include "IWebServer.h"
#include "AuthManager.h"
#include "RestApi.h"
#include <cstdint>
#include "esp_err.h"
#include "esp_http_server.h"

struct httpd_req;

// Forward declarations at global scope
namespace dhcp {
namespace wifi { class IWiFiManager; }
namespace dhcp { class IDhcpServer; }
namespace dns  { class DnsServer; }
} // namespace dhcp

namespace dhcp {
namespace web {

/**
 * @brief ESP-IDF HTTP server implementation.
 */
class WebServer : public IWebServer {
public:
    WebServer(::dhcp::wifi::IWiFiManager& wifi,
              ::dhcp::dhcp::IDhcpServer& dhcpSrv,
              ::dhcp::dns::DnsServer& dnsSrv);
    ~WebServer() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;

    ::dhcp::web::AuthManager& auth() { return auth_; }

private:
    void registerRoutes();
    static esp_err_t staticFileHandler(httpd_req* req);
    static esp_err_t err404Handler(httpd_req* req, httpd_err_code_t err);

    static esp_err_t getStatusHandler(httpd_req* req)  { return RestApi::handleGetStatus(req); }
    static esp_err_t getVersionHandler(httpd_req* req) { return RestApi::handleGetVersion(req); }
    static esp_err_t getDhcpSettingsHandler(httpd_req* req)  { return RestApi::handleGetDhcpSettings(req); }
    static esp_err_t postDhcpSettingsHandler(httpd_req* req) { return RestApi::handlePostDhcpSettings(req); }
    static esp_err_t getStaticBindingsHandler(httpd_req* req)  { return RestApi::handleGetStaticBindings(req); }
    static esp_err_t postStaticBindingsHandler(httpd_req* req) { return RestApi::handlePostStaticBindings(req); }
    static esp_err_t getLeasesHandler(httpd_req* req)  { return RestApi::handleGetLeases(req); }
    static esp_err_t getDnsSettingsHandler(httpd_req* req)  { return RestApi::handleGetDnsSettings(req); }
    static esp_err_t postDnsSettingsHandler(httpd_req* req) { return RestApi::handlePostDnsSettings(req); }
    static esp_err_t getLocalHostsHandler(httpd_req* req)  { return RestApi::handleGetLocalHosts(req); }
    static esp_err_t postLocalHostsHandler(httpd_req* req) { return RestApi::handlePostLocalHosts(req); }
    static esp_err_t getSecuritySettingsHandler(httpd_req* req)  { return RestApi::handleGetSecuritySettings(req); }
    static esp_err_t postSecuritySettingsHandler(httpd_req* req) { return RestApi::handlePostSecuritySettings(req); }
    static esp_err_t postOtaUploadHandler(httpd_req* req) { return RestApi::handlePostOtaUpload(req); }
    static esp_err_t postTestConnectionHandler(httpd_req* req) { return RestApi::handlePostTestConnection(req); }

    httpd_handle_t server_ = nullptr;
    ::dhcp::web::AuthManager auth_;
    ::dhcp::wifi::IWiFiManager& wifi_;
    ::dhcp::dhcp::IDhcpServer& dhcpSrv_;
    ::dhcp::dns::DnsServer& dnsSrv_;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_WEBSERVER_H
