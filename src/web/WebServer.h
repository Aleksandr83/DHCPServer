#ifndef DHCP_WEB_WEBSERVER_H
#define DHCP_WEB_WEBSERVER_H

#include "IWebServer.h"
#include "AuthManager.h"
#include "RestApi.h"
#include "RouteTable.h"
#include <atomic>
#include <cstddef>
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
namespace security { class CertStore; }
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

    /**
     * @brief The certificate store the HTTPS server reads its pair from.
     *
     * Handed over by `main.cpp` after the data volumes are registered, which is
     * later than this object is constructed — and only on the target that has
     * those volumes at all (the classic ESP32 build passes nothing, and HTTPS is
     * then honestly reported as unavailable).
     */
    void setCertificateStore(::dhcp::security::CertStore* store) { certs_ = store; }

    // IWebServer — HTTPS control
    bool httpsAvailable(std::string* status = nullptr) const override;
    bool httpsEnabled() const override { return httpsServer_ != nullptr; }
    bool setHttpsEnabled(bool enabled, std::string* detail = nullptr) override;

    /**
     * @brief Judge the stored pair again now that the device clock is a date.
     *
     * The SNTP client answers long after the servers are up, so the verdict the
     * boot had to make — the pair compared against the epoch comes out "not yet
     * valid" — is not the last word (stage 165). This is that second word: it is
     * called from the clock notification, which runs in the network task, so it
     * returns at once and leaves the TLS start to a task of its own.
     */
    void onClockSet();

    ::dhcp::web::AuthManager& auth() { return auth_; }

private:
    /**
     * @brief Register every route of @ref kRoutes on @p server.
     *
     * @param redirectPlainTraffic True for the plain server, whose routes are
     *        all installed behind @ref httpsRedirectGate: while TLS serves a pair
     *        that has not expired, every answer on port 80 is a permanent
     *        redirect to the same address over TLS (stage 167). The TLS server
     *        registers the handlers themselves — a redirect there would send a
     *        browser to the address it is already on.
     */
    void registerRoutes(httpd_handle_t server, bool redirectPlainTraffic);

    /**
     * @brief The httpd settings both servers share; only the port differs.
     *
     * One place for the stack size, the socket pool and the keep-alive: the
     * reason each of those numbers was chosen is written there once, and the
     * HTTPS server cannot drift away from the plain one.
     *
     * The control port is a parameter because it cannot be shared: httpd binds
     * its control socket without SO_REUSEADDR, so a second server on the same
     * port fails to start (stage 166).
     */
    static httpd_config_t makeConfig(uint16_t port, size_t stackBytes, size_t maxSockets,
                                     uint16_t ctrlPort);

    /** @brief Start the TLS server on the stored pair; false when it cannot. */
    bool startHttps();
    /** @brief Stop the TLS server (idempotent). */
    void stopHttps();

    /**
     * @brief The task behind @ref onClockSet: waits, then retries once.
     *
     * It goes through @ref setHttpsEnabled so that the retry writes the setting
     * and the log line to the same places a manual switch does — and so that a
     * second failure stays as harmless as the first.
     */
    static void httpsRetryTask(void* arg);

    /**
     * @brief The route table, defined in WebServer.cpp next to the handlers.
     *
     * `count` receives the number of routes. It is a function of its own
     * because two places need the table: `start()` takes the HTTP server's
     * handler limit from its size, and `registerRoutes()` walks it.
     */
    static const WebRoute* routes(size_t& count);

    /** @brief One row per reachable path, in registration order. */
    static const WebRoute kRoutes[];

    static esp_err_t staticFileHandler(httpd_req* req);
    static esp_err_t err404Handler(httpd_req* req, httpd_err_code_t err);

    /**
     * @brief The gate every plain route is served through (stage 167).
     *
     * One function in front of the whole route table instead of a redirect in
     * each handler: the decision is the same for the pages, the static files and
     * the REST API — the operator asked for every request without exception —
     * and one place cannot forget a route. Which route was reached travels in
     * `user_ctx`, a field nothing else in the project reads.
     */
    static esp_err_t httpsRedirectGate(httpd_req* req);

    /**
     * @brief The plain server's 404: also an answer to a request, so it redirects too.
     *
     * Without it a path that does not exist would be the one thing an operator
     * could still read over plain HTTP, and the operator asked for no exception.
     * The TLS server keeps @ref err404Handler: there is nothing to redirect to.
     */
    static esp_err_t err404RedirectHandler(httpd_req* req, httpd_err_code_t err);

    /** @brief Answer @p req with a permanent redirect to @p target. */
    static esp_err_t sendHttpsRedirect(httpd_req* req, const std::string& target);

    /**
     * @brief Answer @p req with a redirect while one is in force.
     *
     * Both callers — the route gate and the plain server's 404 — ask the same
     * question with the same header, so they ask it here: read `Host`, build the
     * address, and answer. A request that named no usable address is left to the
     * caller, which answers it the way it always did.
     *
     * @return true when the request has been answered.
     */
    static bool redirectIfInForce(httpd_req* req);

    /** @brief Is the redirect in force right now? (see redirect::needed) */
    bool httpsRedirectActive() const;

    /** @brief Remember when the pair HTTPS serves stops being valid (0 = none). */
    void rememberPairDeadline(int64_t notAfterEpoch) const;

    static esp_err_t getStatusHandler(httpd_req* req)  { return RestApi::handleGetStatus(req); }
    static esp_err_t getVersionHandler(httpd_req* req) { return RestApi::handleGetVersion(req); }
    static esp_err_t getDhcpSettingsHandler(httpd_req* req)  { return RestApi::handleGetDhcpSettings(req); }
    static esp_err_t postDhcpSettingsHandler(httpd_req* req) { return RestApi::handlePostDhcpSettings(req); }
    static esp_err_t getStaticBindingsHandler(httpd_req* req)  { return RestApi::handleGetStaticBindings(req); }
    static esp_err_t postStaticBindingsHandler(httpd_req* req) { return RestApi::handlePostStaticBindings(req); }
    static esp_err_t getAllowedComputersHandler(httpd_req* req)  { return RestApi::handleGetAllowedComputers(req); }
    static esp_err_t postAllowedComputersHandler(httpd_req* req) { return RestApi::handlePostAllowedComputers(req); }
    static esp_err_t postLookupClientNameHandler(httpd_req* req) { return RestApi::handlePostLookupClientName(req); }
    static esp_err_t getLeasesHandler(httpd_req* req)  { return RestApi::handleGetLeases(req); }
    static esp_err_t getDnsSettingsHandler(httpd_req* req)  { return RestApi::handleGetDnsSettings(req); }
    static esp_err_t postDnsSettingsHandler(httpd_req* req) { return RestApi::handlePostDnsSettings(req); }
    static esp_err_t getLocalHostsHandler(httpd_req* req)  { return RestApi::handleGetLocalHosts(req); }
    static esp_err_t postLocalHostsHandler(httpd_req* req) { return RestApi::handlePostLocalHosts(req); }
    static esp_err_t getSecuritySettingsHandler(httpd_req* req)  { return RestApi::handleGetSecuritySettings(req); }
    static esp_err_t postSecuritySettingsHandler(httpd_req* req) { return RestApi::handlePostSecuritySettings(req); }
    static esp_err_t getCertificatesHandler(httpd_req* req)  { return RestApi::handleGetCertificates(req); }
    static esp_err_t postCertificatesHandler(httpd_req* req) { return RestApi::handlePostCertificates(req); }
    static esp_err_t getCertificateDownloadHandler(httpd_req* req) { return RestApi::handleGetCertificateDownload(req); }
    static esp_err_t postOtaUploadHandler(httpd_req* req) { return RestApi::handlePostOtaUpload(req); }
    static esp_err_t postWebFileHandler(httpd_req* req) { return RestApi::handlePostWebFile(req); }
    static esp_err_t postWebSyncHandler(httpd_req* req) { return RestApi::handlePostWebSync(req); }
    static esp_err_t postTestConnectionHandler(httpd_req* req) { return RestApi::handlePostTestConnection(req); }
    static esp_err_t getSettingsExportHandler(httpd_req* req) { return RestApi::handleGetSettingsExport(req); }
    static esp_err_t postSettingsImportHandler(httpd_req* req) { return RestApi::handlePostSettingsImport(req); }
    static esp_err_t postSettingsResetHandler(httpd_req* req) { return RestApi::handlePostSettingsReset(req); }
    static esp_err_t postRebootHandler(httpd_req* req) { return RestApi::handlePostDeviceReboot(req); }
    static esp_err_t postRebootPrepareHandler(httpd_req* req) { return RestApi::handlePostDeviceRebootPrepare(req); }
    static esp_err_t getInternalCacheFileHandler(httpd_req* req) { return RestApi::handleGetInternalCacheFile(req); }
    static esp_err_t getInternalCacheProgressHandler(httpd_req* req) { return RestApi::handleGetInternalCacheProgress(req); }
    // Progress/verdict of the statistics write a planned restart asks for.
    static esp_err_t getStatsProgressHandler(httpd_req* req) { return RestApi::handleGetStatsProgress(req); }
    static esp_err_t postInternalCacheSaveHandler(httpd_req* req) { return RestApi::handlePostInternalCacheSave(req); }
    static esp_err_t postInternalCacheLoadHandler(httpd_req* req) { return RestApi::handlePostInternalCacheLoad(req); }
static esp_err_t postInternalCacheResetHandler(httpd_req* req) { return RestApi::handlePostInternalCacheReset(req); }
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
    static esp_err_t getFileUploadOffsetHandler(httpd_req* req) { return RestApi::handleGetFileUploadOffset(req); }
    static esp_err_t postFileUploadCancelHandler(httpd_req* req) { return RestApi::handlePostFileUploadCancel(req); }
    static esp_err_t getFileTextHandler(httpd_req* req) { return RestApi::handleGetFileText(req); }
    static esp_err_t postFileTextHandler(httpd_req* req) { return RestApi::handlePostFileText(req); }
    static esp_err_t getFileSettingsHandler(httpd_req* req) { return RestApi::handleGetFileSettings(req); }
    static esp_err_t postFileSettingsHandler(httpd_req* req) { return RestApi::handlePostFileSettings(req); }
    static esp_err_t postFileCheckHandler(httpd_req* req) { return RestApi::handlePostFileCheck(req); }
    static esp_err_t postFileCheckCancelHandler(httpd_req* req) { return RestApi::handlePostFileCheckCancel(req); }
    static esp_err_t getFileCheckHandler(httpd_req* req) { return RestApi::handleGetFileCheck(req); }
    static esp_err_t postFileTransferHandler(httpd_req* req) { return RestApi::handlePostFileTransfer(req); }
    static esp_err_t getFileTransferHandler(httpd_req* req) { return RestApi::handleGetFileTransfer(req); }
    static esp_err_t postFileTransferCancelHandler(httpd_req* req) { return RestApi::handlePostFileTransferCancel(req); }
    static esp_err_t getJobsHandler(httpd_req* req) { return RestApi::handleGetJobs(req); }
    static esp_err_t postJobCancelHandler(httpd_req* req) { return RestApi::handlePostJobCancel(req); }

    httpd_handle_t server_ = nullptr;
    /** @brief The TLS server on port 443, or nullptr while HTTPS is off. */
    httpd_handle_t httpsServer_ = nullptr;    /** @brief The stored pair, or nullptr when this build has no store for it. */
    ::dhcp::security::CertStore* certs_ = nullptr;

    /**
     * @brief The instance the httpd task reaches the gate through.
     *
     * An httpd handler is a plain function pointer and the request carries no
     * pointer to this object, so the one server that owns the route tables is
     * remembered here — the idiom `RestApi::init` and `ErrorLog::instance()`
     * already use. It is set by @ref start and cleared when the plain server
     * stops, i.e. it lives exactly as long as the routes it guards.
     */
    static WebServer* s_instance;

    /**
     * @brief When the pair the TLS server serves stops being valid, Unix seconds.
     *
     * The redirect is decided per request, and `CertStore::info()` reads and
     * parses a file on a volume to answer — not something an httpd task may do on
     * every GET. The date is therefore remembered where the pair is judged
     * anyway (@ref httpsAvailable), and a request only compares it with the
     * clock. Atomic because the judge is the network task (clock set, retry) and
     * the reader is the httpd task.
     */
    mutable std::atomic<int64_t> pairDeadlineEpoch_{ 0 };
    // True while the clock-triggered retry is on its way: the clock notification
    // may fire again (every sync does), and two tasks starting the same listener
    // would race for the port.
    volatile bool retryPending_ = false;
    // The PEM blocks the TLS server was started with. The server copies what it
    // needs at start, and keeping them here means the text lives exactly as long
    // as the server object it belongs to.
    std::string certPem_;
    std::string keyPem_;
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
