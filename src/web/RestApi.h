#ifndef DHCP_WEB_RESTAPI_H
#define DHCP_WEB_RESTAPI_H

#include <string>
#include <cstdint>
#include "esp_err.h"
#include "IWebServer.h"

struct httpd_req;

// Forward declarations at global scope
namespace dhcp {
namespace wifi { class IWiFiManager; }
namespace dhcp { class IDhcpServer; }
namespace dns  { class DnsServer; }
namespace time { class TimeServer; }
namespace web  { class AuthManager; }
namespace files { class IFileManager; }
namespace security { class CertStore; }
namespace net { class MdnsResponder; class SsdpResponder; class SshServer; }
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
                     ::dhcp::files::IFileManager* fileMgr = nullptr,
                     ::dhcp::web::IWebServer* web = nullptr);

    static esp_err_t handleGetStatus(httpd_req* req);
    static esp_err_t handleGetVersion(httpd_req* req);
    /**
     * @brief The login form's own request (stage 213).
     *
     * Answers the same 200/401 as any authenticated route, and exists for one
     * reason: it is the only request that means "somebody is logging in", so it is
     * the only one that writes a successful login into `Auth.log` when the address
     * had nothing failed before it.
     */
    static esp_err_t handlePostAuthLogin(httpd_req* req);
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
    /**
     * @brief Give the RestApi the store the HTTPS pair lives in (stage 160).
     *
     * A setter of its own rather than an argument of @ref init: the store is
     * built in `main.cpp` where the volumes are, and the order of that call and
     * `init` (called by `WebServer::begin`) must not decide whether the
     * certificates page works.
     */
    static void setCertificateStore(::dhcp::security::CertStore* store);
    /**
     * @brief Give the REST layer the mDNS responder (stage 219).
     *
     * Same reason as the certificate store above: the responder lives in
     * `main.cpp`, where the network is, and the web layer only needs to ask it
     * to start, stop or rename.
     */
    static void setMdnsResponder(::dhcp::net::MdnsResponder* responder);
    /// @brief Give the REST layer the SSDP responder (stage 220), same reason.
    static void setSsdpResponder(::dhcp::net::SsdpResponder* responder);
    /// @brief Give the REST layer the SSH server (stage 222), same reason.
    static void setSshServer(::dhcp::net::SshServer* server);
    /** @brief State of the pair, the storage picker and the HTTPS switch. */
    static esp_err_t handleGetCertificates(httpd_req* req);
    /** @brief `generate` / `delete` / `storage` on the pair. */
    static esp_err_t handlePostCertificates(httpd_req* req);
    /** @brief Download of the certificate file (the key is never sent). */
    static esp_err_t handleGetCertificateDownload(httpd_req* req);
    static esp_err_t handlePostOtaUpload(httpd_req* req);
    static esp_err_t handlePostWebFile(httpd_req* req);
    /**
     * @brief Make the device's web tree equal to the uploaded folder (stage 169).
     *
     * Body: `{"paths":[…],"delete":true|false}` — a dry run unless `delete` is
     * true; the answer names what a prune would remove (`extra`) and what it did
     * remove (`deleted`). Removes every file on `/spiffs` the list does not hold;
     * an empty list is refused and every name has to pass the upload's own
     * validation (see `WebPrune`).
     */
    static esp_err_t handlePostWebSync(httpd_req* req);
    static esp_err_t handlePostTestConnection(httpd_req* req);
    /**
     * @brief Settings ▸ General (stage 218): the device's own host name.
     *
     * `GET` answers what the device carries, `POST` takes the name the operator
     * typed — validated against the RFC 1123 label rules, saved in NVS and given
     * to the network interface at once, so the page never has to say "takes
     * effect after a reboot".
     */
    static esp_err_t handleGetSettingsGeneral(httpd_req* req);
    static esp_err_t handlePostSettingsGeneral(httpd_req* req);
    /**
     * @brief The SSH server's settings (stage 222).
     *
     * `GET` answers the switch, the port, the LAN-only filter and whether the
     * listener is up right now; `POST` applies the change live — a changed port
     * restarts the listener on the new one rather than waiting for a reboot.
     */
    static esp_err_t handleGetSshSettings(httpd_req* req);
    static esp_err_t handlePostSshSettings(httpd_req* req);
    /**
     * @brief The UPnP device description (`GET /upnp/device.xml`, stage 220).
     *
     * Public on purpose: SSDP/UPnP clients cannot be given a password, and this
     * document is what turns the advertisement into an entry Windows can show.
     * It carries the device's name, its model and the firmware version — nothing
     * that is not already visible on the LAN.
     */
    static esp_err_t handleGetUpnpDevice(httpd_req* req);
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

    // The countdowns of the two autosaves (stage 189), one route for both.
    static esp_err_t handleGetAutosave(httpd_req* req);

    // The statistics file itself, on the operator's command (stage 188): its
    // state, a write now, a read back into the counters, and a reset that zeroes
    // the counters and drops the file.
    static esp_err_t handleGetStatsFile(httpd_req* req);
    static esp_err_t handlePostStatsSave(httpd_req* req);
    static esp_err_t handlePostStatsLoad(httpd_req* req);
    static esp_err_t handlePostStatsReset(httpd_req* req);
    // Built-in (PSRAM) DNS cache persistence file (cache.dat on FAT)
    static esp_err_t handleGetInternalCacheFile(httpd_req* req);
    static esp_err_t handleGetInternalCacheProgress(httpd_req* req);
    // Stage 174: the countdown to the next auto-update cycle.
    static esp_err_t handleGetInternalCacheAutoUpdate(httpd_req* req);
    // Stage 177: one refresh cycle right away ("Update now").
    static esp_err_t handlePostInternalCacheAutoUpdate(httpd_req* req);
    static esp_err_t handlePostInternalCacheSave(httpd_req* req);
    static esp_err_t handlePostInternalCacheLoad(httpd_req* req);
    static esp_err_t handlePostInternalCacheReset(httpd_req* req);
    // Stage 209: the cache view page — one page of records, and the delete button
    // that forgets a single one.
    static esp_err_t handleGetInternalCacheEntries(httpd_req* req);
    static esp_err_t handlePostInternalCacheEntryDelete(httpd_req* req);
    // Time (NTP) server
    static esp_err_t handleGetTimeSettings(httpd_req* req);
    static esp_err_t handlePostTimeSettings(httpd_req* req);
    // Stage 194: the "Reset to default" button of the Time Server page puts the
    // settings that page owns — and only those — back to the factory values.
    static esp_err_t handlePostTimeSettingsReset(httpd_req* req);
    // Stage 195: the "Check" button of that page probes the servers in the field
    // and reports which of them answers.
    static esp_err_t handlePostTimeTest(httpd_req* req);
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
     * @brief The same check for the login form, which `Auth.log` is told about.
     *
     * Kept beside `checkAuth` because the two must not drift: only the reason a
     * successful login is *reported* differs, never how credentials are checked.
     */
    static bool checkLoginAuth(httpd_req* req);
    static bool checkAuth(httpd_req* req, bool fromLoginForm);
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

    /**
     * @brief The state of the certificate pair, the storage picker and HTTPS as JSON.
     *
     * One payload for the page and for the answer of every action, so a change
     * never has to be described in prose: the page renders what the device
     * reports (the volume, the reason of `state`, the dates, the days left).
     */
    static std::string certificatesJson(const ::dhcp::security::CertStore* store);

    /**
     * @brief Re-apply the HTTPS switch after the pair changed (stage 160).
     *
     * Deliberately not done inside the request: the switch stops or restarts the
     * TLS listener, and one of the two listeners may be the one serving this very
     * request — closing it from here would cut the answer off. The task waits
     * for the answer to leave, then applies @p enable and logs the outcome; the
     * page re-reads the state instead of being told what should have happened.
     */
    static void scheduleHttpsSwitch(bool enable);
    /** @brief Body of the task above (waits, then switches HTTPS). */
    static void httpsSwitchTask(void* arg);

    /**
     * @brief Apply the local-name switch to the built-in DNS and to mDNS.
     *
     * The DNS half is a pure re-read of the settings; the mDNS half is the only
     * place that can fail, and it is reported rather than hidden: @return false
     * when the switch asks for publishing and the responder could not start.
     */
    static bool applyLocalNameLive();

    /**
     * @brief Apply the SSH settings to the listener (stage 222).
     *
     * The listener is restarted whenever the switch is turned on or the port
     * changes: a port that only takes effect after a reboot is a port the page
     * lies about. @return the state to report (`running`, `stopped`, `error`,
     * `unavailable`), so "on" is never shown while nothing listens.
     */
    static const char* applySshLive();

    static ::dhcp::wifi::IWiFiManager* s_wifi;
    static ::dhcp::dhcp::IDhcpServer*  s_dhcp;
    static ::dhcp::dns::DnsServer*     s_dns;
    static ::dhcp::time::TimeServer*   s_time;
    static ::dhcp::web::AuthManager*   s_auth;
    static ::dhcp::files::IFileManager* s_files;
    /** @brief The web server itself, for switching HTTPS on and off (stage 158). */
    static ::dhcp::web::IWebServer*    s_web;
    /**
     * @brief The HTTPS certificate store of this build (stage 160).
     *
     * `nullptr` on a target without a volume for the pair: the page then reports
     * `no_store` instead of offering buttons that could not work.
     */
    static ::dhcp::security::CertStore* s_certs;
    /** @brief The mDNS responder of this build (stage 219); may be null in a
     *  host test, and unavailable on a target without the component. */
    static ::dhcp::net::MdnsResponder* s_mdns;
    /// @brief The SSDP responder of this build (stage 220).
    static ::dhcp::net::SsdpResponder* s_ssdp;
    /// @brief The SSH server of this build (stage 222).
    static ::dhcp::net::SshServer* s_ssh;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_RESTAPI_H
