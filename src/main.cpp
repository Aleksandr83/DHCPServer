/**
 * @file main.cpp
 * @brief DHCPServer — ESP32 DHCP + Caching DNS server
 *
 * Hardware: ESP32-WROOM-32 (+ ENC28J60) or ESP32-P4 (Waveshare ESP32-P4-ETH)
 * Framework: ESP-IDF (PlatformIO)
 */

#include <stdio.h>
#include <memory>
#include <string>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_partition.h"
#endif

#include "core/Version.h"
#include "core/Config.h"
#include "core/CpuMonitor.h"
#include "core/ErrorLog.h"
#include "eth/EthManager.h"
#include "eth/EthWifiAdapter.h"
#include "files/FileManager.h"
#include "led/LedController.h"
#include "menu/TerminalMenu.h"
#include "dhcp/DhcpServer.h"
#include "dns/DnsServer.h"
#include "storage/FatFileSystem.h"
#include "storage/SdFileSystem.h"
#include "time/TimeServer.h"
#include "web/WebServer.h"
#include "security/CertStore.h"

using namespace std;

static const char* TAG = "DHCPServer";

// Global instances
// Rule 39: what the console loop and the SPIFFS mount are configured with.
constexpr int kSpiffsMaxFiles = 10;    // files the VFS keeps open at once
constexpr int kConsoleBufBytes = 256;  // one line typed at the console
constexpr int kHeartbeatLoops = 600;   // log the heap every 600 loops (30 s)
constexpr int kMainLoopMs = 50;        // poll the console every 50 ms

static dhcp::eth::EthManager    s_ethManager;
static dhcp::eth::EthWifiAdapter s_netAdapter(s_ethManager);  // wraps Eth as IWiFiManager
static dhcp::led::LedController s_ledController(
#if CONFIG_IDF_TARGET_ESP32P4
    -1  // Waveshare ESP32-P4-ETH has no user LED
#else
    dhcp::led::LedController::kDefaultLedGpio
#endif
);
static dhcp::dhcp::DhcpServer   s_dhcpServer;
static dhcp::dns::DnsServer     s_dnsServer;
static dhcp::time::TimeServer   s_timeServer;
// File explorer volumes (internal FAT always; microSD on the ESP32-P4 only).
static dhcp::files::FileManager s_fileManager;
static dhcp::web::WebServer     s_webServer(s_netAdapter, s_dhcpServer,
                                            s_dnsServer, s_timeServer,
                                            s_fileManager);
// TerminalMenu needs the AuthManager reference, so it must be constructed
// after s_webServer (static init order = declaration order).
static dhcp::menu::TerminalMenu s_terminalMenu(s_netAdapter, s_ledController,
                                               &s_webServer.auth());

// Forward declarations
static void onNetworkConnected();
static void onNetworkDisconnected();
static void onClockSet();
static void updateLedByNetworkStatus();

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "DHCPServer v%s starting...",
             dhcp::core::Version::instance().toString().c_str());

    // ─── Initialize NVS ─────────────────────────────
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase and re-init");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // ─── Start CPU/heap monitor ─────────────────────
    dhcp::core::CpuMonitor::start();

    // ─── Initialize SPIFFS ──────────────────────────
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = kSpiffsMaxFiles,
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
    }

#if CONFIG_IDF_TARGET_ESP32P4
    // ─── File explorer volumes ──────────────────────
    // The internal FAT data partition (mounted the same way it used to be done
    // here, now through FatFileSystem) plus the external microSD card. The card
    // is mounted lazily: FileManager retries in the background, so inserting it
    // later works without a reboot.
    s_fileManager.addVolume(
        make_unique<dhcp::storage::FatFileSystem>("fat", "fat", "/fat"));
    s_fileManager.addVolume(make_unique<dhcp::storage::SdFileSystem>());
    s_fileManager.mountAll();

    // ─── Error log ──────────────────────────────────
    // Critical errors go to /fat/logs/Errors.log: the terminal is not always
    // there (that is how a failed Statistica.dat write stayed unexplained), and
    // the web file explorer can read the file. One low-priority task writes it
    // through a queue in PSRAM, so no caller ever waits for the FAT.
    dhcp::core::ErrorLog::instance().start("/fat");
    // LAN-only access policy (device address + netmask from the DHCP settings).
    s_fileManager.applyAccessFilter();

    // ─── Certificate store for HTTPS ────────────────
    // The pair lives on one of the two data volumes, and the setting says which.
    // The store is built here because this is the only place that knows those
    // volumes exist; the web server receives a pointer to it and starts the TLS
    // listener with what it finds there (stage 158). The classic ESP32 build
    // never reaches this block, passes nothing, and its interface reports HTTPS
    // as unavailable instead of hiding the reason.
    {
        auto* internalVolume = s_fileManager.find("fat");
        auto* cardVolume = s_fileManager.find("sd");
        if (internalVolume != nullptr && cardVolume != nullptr) {
            static dhcp::security::CertStore certStore(*internalVolume, *cardVolume);
            certStore.setStorage(dhcp::core::Config::instance().getSecurity().certStorage);
            s_webServer.setCertificateStore(&certStore);
            // The REST layer gets the same store: the certificates page reads and
            // changes the pair through it, and a second store would be a second
            // answer to "which volume holds the certificate" (stage 160).
            dhcp::web::RestApi::setCertificateStore(&certStore);
        } else {
            ESP_LOGW(TAG, "certificate store not wired: a data volume is missing");
        }
    }
#endif

    // ─── Configure server IP from config ────────────
    {
        auto dhcpCfg = dhcp::core::Config::instance().getDhcp();
        s_ethManager.setStaticIp(dhcpCfg.serverIp, dhcpCfg.gateway, dhcpCfg.subnet);
    }

    // Register network callbacks BEFORE Ethernet init — otherwise the
    // ETHERNET_EVENT_CONNECTED fired during init() can be missed and the
    // DHCP/DNS/Web servers would never start.
    s_ethManager.setOnConnected(onNetworkConnected);
    s_ethManager.setOnDisconnected(onNetworkDisconnected);

    // The clock is a service of its own: SNTP answers after the network is up
    // (and so after the web server is started), and a certificate cannot be
    // judged before there is a date to judge it against. The web server is told
    // when the clock arrives and retries the TLS listener then (stage 165).
    s_timeServer.setOnClockSet(onClockSet);

    // ─── Initialize Ethernet ────────────────────────
    s_ethManager.init();

    // ─── Wire DHCP → DNS (client IP → MAC fallback for DNS REST logs) ──
    s_dnsServer.setDhcpServer(&s_dhcpServer);

    // ─── Initialize LED (start: off) ────────────────
    s_ledController.turnOff();

    // ─── Start terminal menu ────────────────────────
    // The console follows the same policy as the web UI: `reboot` keeps the
    // main-page statistics (Statistica.dat) and the cache (cache.dat), a factory
    // reset deletes the statistics file.
    s_terminalMenu.setLifecycleHooks(
        []() { s_dnsServer.saveStatsBeforeRestart(); s_dnsServer.saveCacheBeforeRestart(); },
        []() { s_dnsServer.deleteStatsFile(); });
    s_terminalMenu.start();

    ESP_LOGI(TAG, "DHCPServer initialized. Type 'help' in terminal.");

    // ─── Main loop ──────────────────────────────────
    s_terminalMenu.print("dhcp> ");
    uint32_t heartbeat = 0;
    while (1) {
        char buf[kConsoleBufBytes];
        if (fgets(buf, sizeof(buf), stdin)) {
            // Remove trailing newline(s)
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                buf[--len] = '\0';
            }
            if (len > 0) {
                s_terminalMenu.processLine(string(buf));
                s_terminalMenu.print("dhcp> ");
            }
        }

        // Heap heartbeat every ~30 s (kHeartbeatLoops * kMainLoopMs) —
        // diagnostics for hangs caused by memory leaks. Pinned to internal RAM
        // so it stays useful on chips where PSRAM is enabled.
        if (++heartbeat % kHeartbeatLoops == 0) {
            ESP_LOGI(TAG, "HEAP: free=%lu largest_block=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }

        vTaskDelay(pdMS_TO_TICKS(kMainLoopMs));
    }
}

// ─── Network callbacks ───────────────────────────────

static void onNetworkConnected()
{
    ESP_LOGI(TAG, "Network connected! IP: %s", s_netAdapter.ip4().c_str());
    updateLedByNetworkStatus();
    s_terminalMenu.println("");
    s_terminalMenu.println("*** Network connected ***");
    char buf[128];
    snprintf(buf, sizeof(buf), "    IPv4: %s\r\n    IPv6: %s",
                  s_netAdapter.ip4().c_str(), s_netAdapter.ip6().c_str());
    s_terminalMenu.println(buf);

    // Allowed computers (DHCP allow-list): build the PSRAM MAC hash table from
    // NVS before the server starts, and regardless of whether it is enabled --
    // the web page reports the table state, so it must exist either way.
    s_dhcpServer.reloadAllowedComputers();

    // Start DHCP server (check config enabled flag)
    if (!s_dhcpServer.isRunning()) {
        auto dhcpCfg = ::dhcp::core::Config::instance().getDhcp();
        if (dhcpCfg.enabled) {
            if (s_dhcpServer.start()) {
                ESP_LOGI(TAG, "DHCP server started");
                s_terminalMenu.println("*** DHCP server started ***");
            } else {
                ESP_LOGE(TAG, "DHCP server failed to start");
            }
        } else {
            ESP_LOGI(TAG, "DHCP server is disabled in config, skipping");
        }
    }

    // Start DNS server (check config enabled flag)
    if (!s_dnsServer.isRunning()) {
        auto dnsCfg = ::dhcp::core::Config::instance().getDns();
        if (dnsCfg.enabled) {
            if (s_dnsServer.start()) {
                ESP_LOGI(TAG, "DNS server started");
                s_terminalMenu.println("*** DNS server started ***");
            } else {
                ESP_LOGE(TAG, "DNS server failed to start");
            }
        } else {
            ESP_LOGI(TAG, "DNS server is disabled in config, skipping");
        }
    }
    // Keep DHCP in sync with the built-in DNS server running state
    s_dhcpServer.setDnsServerRunning(s_dnsServer.isRunning());

    // Time (NTP): the clock-sync client and the NTP server are independent.
    {
        auto timeCfg = ::dhcp::core::Config::instance().getTime();
        s_timeServer.setServerName(timeCfg.externalNtp);
        s_timeServer.setSyncIntervalSec(timeCfg.syncIntervalSec);
        s_timeServer.setUtcOffsetHours(timeCfg.utcOffsetHours);
        s_timeServer.setTimezoneName(timeCfg.timezone);
        s_timeServer.logger().setLogTerminal(timeCfg.logTerminal);
        s_timeServer.logger().setLogRest(timeCfg.logRest);
        s_timeServer.logger().setLogUrl(timeCfg.logUrl);
        s_timeServer.logger().setLogAuth(timeCfg.logAuthEnabled,
                                         timeCfg.logAuthUser,
                                         timeCfg.logAuthPassword);

        // SNTP clock sync (independent of serving time).
        if (timeCfg.syncEnabled) {
            s_timeServer.startSync();
        } else {
            ESP_LOGI(TAG, "NTP clock sync is disabled in config, skipping");
        }

        // NTP server (serving LAN clients).
        if (timeCfg.enabled) {
            if (!s_timeServer.isRunning() && s_timeServer.start()) {
                ESP_LOGI(TAG, "NTP server started");
                s_terminalMenu.println("*** NTP server started ***");
            }
        } else {
            ESP_LOGI(TAG, "NTP server is disabled in config, skipping");
        }
    }

    // Start web server
    if (!s_webServer.isRunning()) {
        if (s_webServer.start()) {
            ESP_LOGI(TAG, "Web server started");
            s_terminalMenu.println("*** Web server started ***");
        } else {
            ESP_LOGE(TAG, "Web server failed to start");
        }
    }
}

static void onNetworkDisconnected()
{
    ESP_LOGW(TAG, "Network disconnected");
    updateLedByNetworkStatus();

    // Stop DHCP server
    if (s_dhcpServer.isRunning()) {
        s_dhcpServer.stop();
        ESP_LOGI(TAG, "DHCP server stopped");
    }

    // Stop DNS server
    if (s_dnsServer.isRunning()) {
        s_dnsServer.stop();
        ESP_LOGI(TAG, "DNS server stopped");
    }
    s_dhcpServer.setDnsServerRunning(false);

    // Stop NTP (time) server
    if (s_timeServer.isRunning()) {
        s_timeServer.stop();
        ESP_LOGI(TAG, "NTP server stopped");
    }
    // Stop the SNTP clock-sync client as well (no network → no sync).
    s_timeServer.stopSync();

    // Stop web server
    if (s_webServer.isRunning()) {
        s_webServer.stop();
        ESP_LOGI(TAG, "Web server stopped");
    }
}

static void updateLedByNetworkStatus()
{
    if (s_netAdapter.isConnected()) {
        s_ledController.turnOn();
    } else {
        s_ledController.turnOff();
    }
}

static void onClockSet()
{
    // The notification itself comes from the network task, so the web server is
    // asked to deal with it there — it decides and returns, and the retry runs on
    // its own task (see WebServer::onClockSet).
    s_webServer.onClockSet();
}
