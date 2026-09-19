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
namespace time { class TimeServer; }
namespace web  { class AuthManager; }
namespace files { class IFileManager; }
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
                     ::dhcp::time::TimeServer* timeSrv,
                     ::dhcp::web::AuthManager* auth,
                     ::dhcp::files::IFileManager* fileMgr = nullptr);

    static esp_err_t handleGetStatus(httpd_req* req);
    static esp_err_t handleGetVersion(httpd_req* req);
    static esp_err_t handleGetDhcpSettings(httpd_req* req);
    static esp_err_t handlePostDhcpSettings(httpd_req* req);
    static esp_err_t handleGetStaticBindings(httpd_req* req);
    static esp_err_t handlePostStaticBindings(httpd_req* req);
    // Allowed computers (DHCP allow-list): the list the "assign addresses only
    // to allowed computers" switch in the DHCP settings works with.
    static esp_err_t handleGetAllowedComputers(httpd_req* req);
    static esp_err_t handlePostAllowedComputers(httpd_req* req);
    /**
     * @brief What is this MAC called? (POST /api/dhcp/lookup-name)
     *
     * Answers with the name the client reported about itself and the source it
     * came from, or an empty name: "unknown" is a real answer, not an error.
     */
    static esp_err_t handlePostLookupClientName(httpd_req* req);
    static esp_err_t handleGetLeases(httpd_req* req);
    static esp_err_t handleGetDnsSettings(httpd_req* req);
    static esp_err_t handlePostDnsSettings(httpd_req* req);
    static esp_err_t handleGetLocalHosts(httpd_req* req);
    static esp_err_t handlePostLocalHosts(httpd_req* req);
    static esp_err_t handleGetSecuritySettings(httpd_req* req);
    static esp_err_t handlePostSecuritySettings(httpd_req* req);
    static esp_err_t handlePostOtaUpload(httpd_req* req);
    static esp_err_t handlePostWebFile(httpd_req* req);
    static esp_err_t handlePostTestConnection(httpd_req* req);
    static esp_err_t handleGetSettingsExport(httpd_req* req);
    static esp_err_t handlePostSettingsImport(httpd_req* req);
    static esp_err_t handlePostSettingsReset(httpd_req* req);
    static esp_err_t handlePostDeviceReboot(httpd_req* req);
    // Start the files a planned restart wants (statistics and cache, each as a
    // background job) and report what each step decided, so the page can show
    // what is happening and poll both verdicts.
    static esp_err_t handlePostDeviceRebootPrepare(httpd_req* req);
    // Progress/verdict of the background statistics write (Statistica.dat)
    static esp_err_t handleGetStatsProgress(httpd_req* req);
    // Built-in (PSRAM) DNS cache persistence file (cache.dat on FAT)
    static esp_err_t handleGetInternalCacheFile(httpd_req* req);
    static esp_err_t handleGetInternalCacheProgress(httpd_req* req);
    static esp_err_t handlePostInternalCacheSave(httpd_req* req);
    static esp_err_t handlePostInternalCacheLoad(httpd_req* req);
    // Time (NTP) server
    static esp_err_t handleGetTimeSettings(httpd_req* req);
    static esp_err_t handlePostTimeSettings(httpd_req* req);
    static esp_err_t handleGetTimeNow(httpd_req* req);
    static esp_err_t handlePostTimeSet(httpd_req* req);
    // File explorer (FAT volumes)
    static esp_err_t handleGetFileVolumes(httpd_req* req);
    static esp_err_t handleGetFileList(httpd_req* req);
    static esp_err_t handlePostFileMkdir(httpd_req* req);
    static esp_err_t handlePostFileRename(httpd_req* req);
    static esp_err_t handlePostFileDelete(httpd_req* req);
    static esp_err_t handlePostFileFormat(httpd_req* req);
    static esp_err_t handleGetFileDownload(httpd_req* req);
    static esp_err_t handlePostFileUpload(httpd_req* req);
    static esp_err_t handleGetFileUploadOffset(httpd_req* req);
    static esp_err_t handlePostFileUploadCancel(httpd_req* req);
    static esp_err_t handleGetFileText(httpd_req* req);
    static esp_err_t handlePostFileText(httpd_req* req);
    static esp_err_t handleGetFileSettings(httpd_req* req);
    static esp_err_t handlePostFileSettings(httpd_req* req);
    // Read-only volume check ("Check for errors")
    static esp_err_t handlePostFileCheck(httpd_req* req);
    static esp_err_t handlePostFileCheckCancel(httpd_req* req);
    static esp_err_t handleGetFileCheck(httpd_req* req);
    static esp_err_t handlePostFileTransfer(httpd_req* req);
    static esp_err_t handleGetFileTransfer(httpd_req* req);
    static esp_err_t handlePostFileTransferCancel(httpd_req* req);
    // Task scheduler (long-running operations)
    static esp_err_t handleGetJobs(httpd_req* req);
    static esp_err_t handlePostJobCancel(httpd_req* req);

private:
    static bool checkAuth(httpd_req* req);
    /**
     * @brief LAN-only gate for the file endpoints.
     *
     * Uses the **socket** peer address (never `X-Forwarded-For`: a client
     * must not be able to talk itself into the allowed subnet) and the filter
     * from `IFileManager`. Answers `403` itself.
     */
    static bool checkFileAccess(httpd_req* req);
    static esp_err_t respondUnauthorized(httpd_req* req);
    static std::string getClientIp(httpd_req* req);
    /** @brief Peer IPv4 of the request in host byte order (0 when unknown). */
    static uint32_t getClientIp4(httpd_req* req);

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
    static ::dhcp::time::TimeServer*   s_time;
    static ::dhcp::web::AuthManager*   s_auth;
    static ::dhcp::files::IFileManager* s_files;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_RESTAPI_H
