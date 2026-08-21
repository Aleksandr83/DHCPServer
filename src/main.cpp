/**
 * @file main.cpp
 * @brief DHCPServer — ESP32 DHCP + Caching DNS server
 *
 * Hardware: ESP32-WROOM-32
 * Framework: ESP-IDF (PlatformIO)
 */

#include <stdio.h>
#include <string>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"

#include "core/Version.h"
#include "core/Config.h"
#include "core/CpuMonitor.h"
#include "wifi/WiFiManager.h"
#include "eth/EthManager.h"
#include "eth/EthWifiAdapter.h"
#include "led/LedController.h"
#include "menu/TerminalMenu.h"
#include "dhcp/DhcpServer.h"
#include "dns/DnsServer.h"
#include "web/WebServer.h"

static const char* TAG = "DHCPServer";

// Global instances
static dhcp::eth::EthManager    s_ethManager;
static dhcp::eth::EthWifiAdapter s_netAdapter(s_ethManager);  // wraps Eth as IWiFiManager
static dhcp::led::LedController s_ledController;
static dhcp::dhcp::DhcpServer   s_dhcpServer;
static dhcp::dns::DnsServer     s_dnsServer;
static dhcp::web::WebServer     s_webServer(s_netAdapter, s_dhcpServer, s_dnsServer);
// TerminalMenu needs the AuthManager reference, so it must be constructed
// after s_webServer (static init order = declaration order).
static dhcp::menu::TerminalMenu s_terminalMenu(s_netAdapter, s_ledController,
                                               &s_webServer.auth());

// Forward declarations
static void onNetworkConnected();
static void onNetworkDisconnected();
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
        .max_files = 10,
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
    }

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

    // ─── Initialize Ethernet ────────────────────────
    s_ethManager.init();

    // ─── Wire DHCP → DNS (client IP → MAC fallback for DNS REST logs) ──
    s_dnsServer.setDhcpServer(&s_dhcpServer);

    // ─── Initialize LED (start: off) ────────────────
    s_ledController.turnOff();

    // ─── Start terminal menu ────────────────────────
    s_terminalMenu.start();

    ESP_LOGI(TAG, "DHCPServer initialized. Type 'help' in terminal.");

    // ─── Main loop ──────────────────────────────────
    s_terminalMenu.print("dhcp> ");
    uint32_t heartbeat = 0;
    while (1) {
        char buf[256];
        if (fgets(buf, sizeof(buf), stdin)) {
            // Remove trailing newline(s)
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                buf[--len] = '\0';
            }
            if (len > 0) {
                s_terminalMenu.processLine(std::string(buf));
                s_terminalMenu.print("dhcp> ");
            }
        }

        // Heap heartbeat every ~30 s (600 * 50 ms) — diagnostics for hangs
        // caused by memory leaks.
        if (++heartbeat % 600 == 0) {
            ESP_LOGI(TAG, "HEAP: free=%lu largest_block=%lu",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
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
    std::snprintf(buf, sizeof(buf), "    IPv4: %s\r\n    IPv6: %s",
                  s_netAdapter.ip4().c_str(), s_netAdapter.ip6().c_str());
    s_terminalMenu.println(buf);

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
