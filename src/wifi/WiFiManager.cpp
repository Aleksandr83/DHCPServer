#include "WiFiManager.h"
#include <cstring>
#include <algorithm>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

static const char* TAG = "WiFiManager";

// Default external DNS server
#define DEFAULT_DNS_SERVER "192.168.1.1"

namespace dhcp {
namespace wifi {

WiFiManager::WiFiManager(const std::string& ip4,
                         const std::string& gateway,
                         const std::string& netmask,
                         const std::string& ip6)
    : staticIp4_(ip4)
    , gateway_(gateway)
    , netmask_(netmask)
    , staticIp6_(ip6)
{
}

WiFiManager::~WiFiManager()
{
    deinit();
}

void WiFiManager::setStaticIp(const std::string& ip4, const std::string& gateway,
                               const std::string& netmask)
{
    staticIp4_ = ip4;
    gateway_ = gateway;
    netmask_ = netmask;
    ESP_LOGI(TAG, "Static IP reconfigured: %s / %s / %s",
             ip4.c_str(), netmask.c_str(), gateway.c_str());
}

void WiFiManager::init()
{
    if (initialized_) return;

    // Initialize netif
    esp_netif_init();
    esp_event_loop_create_default();

    // Create default STA netif
    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    assert(netif);

    // Configure static IPv4
    esp_netif_ip_info_t ipInfo;
    inet_pton(AF_INET, staticIp4_.c_str(), &ipInfo.ip);
    inet_pton(AF_INET, gateway_.c_str(), &ipInfo.gw);
    inet_pton(AF_INET, netmask_.c_str(), &ipInfo.netmask);
    esp_netif_dhcpc_stop(netif);
    esp_netif_set_ip_info(netif, &ipInfo);

    // Configure static IPv6 (set address after interface is up)
    // IPv6 will be available on the interface after WiFi connects

    // Set DNS server
    esp_netif_dns_info_t dnsInfo;
    inet_pton(AF_INET, DEFAULT_DNS_SERVER, &dnsInfo.ip.u_addr.ip4);
    dnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dnsInfo);

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_GOT_IP6, &eventHandler, this, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    initialized_ = true;
    ESP_LOGI(TAG, "WiFi initialized (static IP: %s / %s)", staticIp4_.c_str(), staticIp6_.c_str());
}

void WiFiManager::deinit()
{
    if (!initialized_) return;

    esp_wifi_stop();
    esp_wifi_deinit();

    esp_netif_deinit();
    initialized_ = false;
    status_ = WiFiStatus::DISCONNECTED;
    ESP_LOGI(TAG, "WiFi deinitialized");
}

void WiFiManager::connect(const std::string& ssid, const std::string& password)
{
    if (!initialized_) {
        ESP_LOGE(TAG, "WiFi not initialized, call init() first");
        return;
    }

    currentSsid_ = ssid;
    status_ = WiFiStatus::CONNECTING;

    wifi_config_t wifiCfg = {};
    std::strncpy(reinterpret_cast<char*>(wifiCfg.sta.ssid), ssid.c_str(), sizeof(wifiCfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifiCfg.sta.password), password.c_str(), sizeof(wifiCfg.sta.password) - 1);

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid.c_str());

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiCfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void WiFiManager::disconnect()
{
    if (status_ == WiFiStatus::CONNECTED || status_ == WiFiStatus::CONNECTING) {
        esp_wifi_disconnect();
        status_ = WiFiStatus::DISCONNECTED;
        ip4_.clear();
        ip6_.clear();
        ESP_LOGI(TAG, "WiFi disconnected");
    }
}

void WiFiManager::eventHandler(void* arg, esp_event_base_t eventBase,
                                int32_t eventId, void* eventData)
{
    WiFiManager* self = static_cast<WiFiManager*>(arg);
    if (self) {
        self->handleEvent(eventBase, eventId, eventData);
    }
}

void WiFiManager::handleEvent(esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGI(TAG, "WiFi STA connected (static IP: %s)", staticIp4_.c_str());
            // With static IP (DHCPC stopped), set status immediately
            ip4_ = staticIp4_;
            status_ = WiFiStatus::CONNECTED;
            if (onConnected_) onConnected_();
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            ESP_LOGW(TAG, "WiFi disconnected");
            status_ = WiFiStatus::DISCONNECTED;
            ip4_.clear();
            ip6_.clear();
            if (onDisconnected_) onDisconnected_();
            // Auto-reconnect
            esp_wifi_connect();
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        if (event) {
            char buf[16];
            inet_ntop(AF_INET, &event->ip_info.ip, buf, sizeof(buf));
            ip4_ = buf;
        }

        status_ = WiFiStatus::CONNECTED;
        ESP_LOGI(TAG, "Got IPv4: %s", ip4_.c_str());

        // Configure static IPv6 on the interface
        if (!staticIp6_.empty()) {
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("STA_DEF");
            if (netif) {
                esp_ip6_addr_t ip6Addr = {};  // zero-initialize (includes zone field)
                if (inet_pton(AF_INET6, staticIp6_.c_str(), &ip6Addr.addr) == 1) {
                    esp_err_t err = esp_netif_add_ip6_address(netif, ip6Addr, true);
                    if (err == ESP_OK) {
                        ip6_ = staticIp6_;
                        ESP_LOGI(TAG, "Static IPv6 configured: %s", ip6_.c_str());
                    } else {
                        ESP_LOGW(TAG, "Failed to set static IPv6: %s", esp_err_to_name(err));
                        // Fallback: show configured address anyway
                        ip6_ = staticIp6_;
                    }
                }
            } else {
                ip6_ = staticIp6_;
            }
        }

        if (onConnected_) onConnected_();

    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        // IPv6 address available from network — use it
        auto* event = static_cast<ip_event_got_ip6_t*>(data);
        if (event) {
            char buf6[40];
            inet_ntop(AF_INET6, &event->ip6_info.ip, buf6, sizeof(buf6));
            ip6_ = buf6;
            ESP_LOGI(TAG, "Got IPv6 from network: %s", ip6_.c_str());
        }
    }
}

} // namespace wifi
} // namespace dhcp
