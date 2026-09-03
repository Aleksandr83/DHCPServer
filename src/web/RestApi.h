#ifndef DHCP_WEB_RESTAPI_H
#define DHCP_WEB_RESTAPI_H

#include <string>
#include <cstdint>
#include "esp_err.h"

struct httpd_req;

// Forward declarations at global scope
namespace dhcp {
namespace wifi { class IWiFiManager; }
namespace dhcp { class IDhcpServer; }
namespace dns  { class DnsServer; }
namespace web  { class AuthManager; }
} // namespace dhcp

namespace dhcp {
namespace web {

/**
 * @brief REST API handler.
 */
class RestApi {
public:
    static void init(::dhcp::wifi::IWiFiManager* wifi,
                     ::dhcp::dhcp::IDhcpServer* dhcpSrv,
                     ::dhcp::dns::DnsServer* dnsSrv,
                     ::dhcp::web::AuthManager* auth);

    static esp_err_t handleGetStatus(httpd_req* req);
    static esp_err_t handleGetVersion(httpd_req* req);
    static esp_err_t handleGetDhcpSettings(httpd_req* req);
    static esp_err_t handlePostDhcpSettings(httpd_req* req);
    static esp_err_t handleGetStaticBindings(httpd_req* req);
    static esp_err_t handlePostStaticBindings(httpd_req* req);
    static esp_err_t handleGetLeases(httpd_req* req);
    static esp_err_t handleGetDnsSettings(httpd_req* req);
    static esp_err_t handlePostDnsSettings(httpd_req* req);
    static esp_err_t handleGetLocalHosts(httpd_req* req);
    static esp_err_t handlePostLocalHosts(httpd_req* req);
    static esp_err_t handleGetSecuritySettings(httpd_req* req);
    static esp_err_t handlePostSecuritySettings(httpd_req* req);
    static esp_err_t handlePostOtaUpload(httpd_req* req);
    static esp_err_t handlePostTestConnection(httpd_req* req);
    static esp_err_t handleGetSettingsExport(httpd_req* req);
    static esp_err_t handlePostSettingsImport(httpd_req* req);

private:
    static bool checkAuth(httpd_req* req);
    static esp_err_t respondUnauthorized(httpd_req* req);
    static std::string getClientIp(httpd_req* req);

    static void addJsonString(std::string& json, const std::string& key,
                              const std::string& val, bool addComma);
    static void addJsonBool(std::string& json, const std::string& key,
                            bool val, bool addComma);
    static void addJsonInt(std::string& json, const std::string& key,
                           int64_t val, bool addComma);
    static std::string readBody(httpd_req* req, size_t maxLen = 4096);

    static ::dhcp::wifi::IWiFiManager* s_wifi;
    static ::dhcp::dhcp::IDhcpServer*  s_dhcp;
    static ::dhcp::dns::DnsServer*     s_dns;
    static ::dhcp::web::AuthManager*   s_auth;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_RESTAPI_H
