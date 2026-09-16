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
namespace time { class TimeServer; }
namespace files { class IFileManager; }
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
              ::dhcp::dns::DnsServer& dnsSrv,
              ::dhcp::time::TimeServer& timeSrv,
              ::dhcp::files::IFileManager& fileMgr);
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
    static esp_err_t postWebFileHandler(httpd_req* req) { return RestApi::handlePostWebFile(req); }
    static esp_err_t postTestConnectionHandler(httpd_req* req) { return RestApi::handlePostTestConnection(req); }
    static esp_err_t getSettingsExportHandler(httpd_req* req) { return RestApi::handleGetSettingsExport(req); }
    static esp_err_t postSettingsImportHandler(httpd_req* req) { return RestApi::handlePostSettingsImport(req); }
    static esp_err_t postSettingsResetHandler(httpd_req* req) { return RestApi::handlePostSettingsReset(req); }
    static esp_err_t postRebootHandler(httpd_req* req) { return RestApi::handlePostDeviceReboot(req); }
    static esp_err_t getInternalCacheFileHandler(httpd_req* req) { return RestApi::handleGetInternalCacheFile(req); }
    static esp_err_t getInternalCacheProgressHandler(httpd_req* req) { return RestApi::handleGetInternalCacheProgress(req); }
    static esp_err_t postInternalCacheSaveHandler(httpd_req* req) { return RestApi::handlePostInternalCacheSave(req); }
    static esp_err_t postInternalCacheLoadHandler(httpd_req* req) { return RestApi::handlePostInternalCacheLoad(req); }
    static esp_err_t getTimeSettingsHandler(httpd_req* req) { return RestApi::handleGetTimeSettings(req); }
    static esp_err_t postTimeSettingsHandler(httpd_req* req) { return RestApi::handlePostTimeSettings(req); }
    static esp_err_t getTimeNowHandler(httpd_req* req) { return RestApi::handleGetTimeNow(req); }
    static esp_err_t postTimeSetHandler(httpd_req* req) { return RestApi::handlePostTimeSet(req); }
    // File explorer (FAT volumes)
    static esp_err_t getFileVolumesHandler(httpd_req* req) { return RestApi::handleGetFileVolumes(req); }
    static esp_err_t getFileListHandler(httpd_req* req) { return RestApi::handleGetFileList(req); }
    static esp_err_t postFileMkdirHandler(httpd_req* req) { return RestApi::handlePostFileMkdir(req); }
    static esp_err_t postFileRenameHandler(httpd_req* req) { return RestApi::handlePostFileRename(req); }
    static esp_err_t postFileDeleteHandler(httpd_req* req) { return RestApi::handlePostFileDelete(req); }
    static esp_err_t postFileFormatHandler(httpd_req* req) { return RestApi::handlePostFileFormat(req); }
    static esp_err_t getFileDownloadHandler(httpd_req* req) { return RestApi::handleGetFileDownload(req); }
    static esp_err_t postFileUploadHandler(httpd_req* req) { return RestApi::handlePostFileUpload(req); }
    static esp_err_t getFileTextHandler(httpd_req* req) { return RestApi::handleGetFileText(req); }
    static esp_err_t postFileTextHandler(httpd_req* req) { return RestApi::handlePostFileText(req); }
    static esp_err_t getFileSettingsHandler(httpd_req* req) { return RestApi::handleGetFileSettings(req); }
    static esp_err_t postFileSettingsHandler(httpd_req* req) { return RestApi::handlePostFileSettings(req); }
    static esp_err_t postFileCheckHandler(httpd_req* req) { return RestApi::handlePostFileCheck(req); }
    static esp_err_t postFileCheckCancelHandler(httpd_req* req) { return RestApi::handlePostFileCheckCancel(req); }
    static esp_err_t getFileCheckHandler(httpd_req* req) { return RestApi::handleGetFileCheck(req); }

    httpd_handle_t server_ = nullptr;
    ::dhcp::web::AuthManager auth_;
    ::dhcp::wifi::IWiFiManager& wifi_;
    ::dhcp::dhcp::IDhcpServer& dhcpSrv_;
    ::dhcp::dns::DnsServer& dnsSrv_;
    ::dhcp::time::TimeServer& timeSrv_;
    ::dhcp::files::IFileManager& fileMgr_;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_WEBSERVER_H
