#include "EthManager.h"
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_netif_glue.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "eth_enc28j60_config.h"
#include "enc28j60.h"   /* for enc28j60_spi_init */

/* Our local ENC28J60 MAC driver */
extern "C" esp_eth_mac_t *esp_eth_mac_new_enc28j60(const eth_enc28j60_config_t *enc28j60_config,
                                                     const eth_mac_config_t *mac_config);
/* Our local ENC28J60 PHY driver */
extern "C" esp_eth_phy_t *esp_eth_phy_new_enc28j60(const eth_phy_config_t *config);

static const char* TAG = "EthManager";

/* Enable internal pull-up on a GPIO pin */
static inline esp_err_t gpio_pullup(int pin)
{
    return gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
}

namespace dhcp {
namespace eth {

// ─── Pin configuration ──────────────────────────────
#define ETH_SPI_HOST       SPI2_HOST
#define ETH_PIN_MOSI       23
#define ETH_PIN_MISO       19
#define ETH_PIN_SCLK       18
#define ETH_PIN_CS         5
#define ETH_PIN_INT        4
#define ETH_PIN_RST        16

// ─────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────

EthManager::EthManager(const std::string& ip4,
                       const std::string& gateway,
                       const std::string& netmask,
                       const std::string& ip6)
    : staticIp4_(ip4)
    , gateway_(gateway)
    , netmask_(netmask)
    , staticIp6_(ip6)
{
}

EthManager::~EthManager()
{
    if (ethHandle_) {
        esp_eth_stop(ethHandle_);
        esp_eth_driver_uninstall(ethHandle_);
    }
}

// ─────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────

void EthManager::init()
{
    if (initialized_) return;

    ESP_LOGI(TAG, "Initializing ENC28J60 Ethernet...");

    // ─── 0a. Initialize default event loop (required for eth, netif, etc.) ─
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop init failed: %s", esp_err_to_name(err));
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }

    // ─── 0b. Initialize netif (required for Ethernet netif) ────────────────
    esp_netif_init();

    // ─── 1. Create default Ethernet netif ────────────
    esp_netif_config_t netifCfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* ethNetif = esp_netif_new(&netifCfg);
    if (!ethNetif) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }

    // ─── 0c. Enable pull-ups on SPI/control pins ───
    gpio_pullup(ETH_PIN_CS);   /* CS — critical to prevent floating during boot */
    gpio_pullup(ETH_PIN_RST);  /* RST */
    gpio_pullup(ETH_PIN_INT);  /* INT */

    // ─── 1b. Init SPI bus + device for ENC28J60 ────
    err = enc28j60_spi_init(ETH_SPI_HOST, ETH_PIN_MOSI, ETH_PIN_MISO,
                             ETH_PIN_SCLK, ETH_PIN_CS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ENC28J60 SPI init failed: %s", esp_err_to_name(err));
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }
    ESP_LOGI(TAG, "SPI ready (CS=%d)", ETH_PIN_CS);

    // ─── 2. ENC28J60 MAC config ─────────────────────
    eth_enc28j60_config_t enc28j60Cfg = ETH_ENC28J60_DEFAULT_CONFIG(ETH_PIN_CS);
    enc28j60Cfg.int_gpio_num = ETH_PIN_INT;

    eth_mac_config_t macCfg = ETH_MAC_DEFAULT_CONFIG();
    macCfg.flags = 0;

    esp_eth_mac_t* mac = esp_eth_mac_new_enc28j60(&enc28j60Cfg, &macCfg);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create ENC28J60 MAC");
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }
    // ─── 3. ENC28J60 PHY config ─────────────────────
    eth_phy_config_t phyCfg = ETH_PHY_DEFAULT_CONFIG();
    phyCfg.phy_addr = 0;
    phyCfg.reset_gpio_num = ETH_PIN_RST;
    phyCfg.reset_timeout_ms = 100;

    esp_eth_phy_t* phy = esp_eth_phy_new_enc28j60(&phyCfg);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create ENC28J60 PHY");
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }

    // ─── 4. Install Ethernet driver ─────────────────
    esp_eth_config_t ethCfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&ethCfg, &ethHandle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }
    ESP_LOGI(TAG, "Ethernet driver installed");

    // ─── 5. Attach netif glue ───────────────────────
    esp_eth_netif_glue_handle_t netifGlue = esp_eth_new_netif_glue(ethHandle_);
    if (!netifGlue) {
        ESP_LOGE(TAG, "Failed to create netif glue");
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }
    err = esp_netif_attach(ethNetif, netifGlue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach netif: %s", esp_err_to_name(err));
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }

    // ─── 5a. Configure static IP on the netif ──────
    // Must be done after esp_netif_attach, before Ethernet starts.
    {
        esp_netif_dhcpc_stop(ethNetif);
        esp_netif_ip_info_t ipInfo;
        memset(&ipInfo, 0, sizeof(ipInfo));
        inet_pton(AF_INET, staticIp4_.c_str(), &ipInfo.ip);
        inet_pton(AF_INET, gateway_.c_str(), &ipInfo.gw);
        inet_pton(AF_INET, netmask_.c_str(), &ipInfo.netmask);
        esp_netif_set_ip_info(ethNetif, &ipInfo);
        ip4_ = staticIp4_;
        ESP_LOGI(TAG, "Static IP configured on netif: %s", ip4_.c_str());
    }

    // ─── 5b. Use the built-in DNS server (this device) as the system
    // resolver ──────────────────────────────────────────
    // Local ".lo" hosts (e.g. dhcpserverweb.lo) are only known by the built-in
    // DNS server, not by the router. Pointing the netif DNS here lets the
    // ESP32's own outbound connections (REST log sender) resolve those names,
    // keep the hostname in the URL and thus send a correct Host header for
    // Apache name-based vhosts. External names are forwarded by the built-in
    // DNS server to the configured external DNS.
    {
        esp_netif_dns_info_t dnsInfo;
        memset(&dnsInfo, 0, sizeof(dnsInfo));
        dnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
        inet_pton(AF_INET, staticIp4_.c_str(), &dnsInfo.ip.u_addr.ip4);
        esp_netif_set_dns_info(ethNetif, ESP_NETIF_DNS_MAIN, &dnsInfo);
        ESP_LOGI(TAG, "System DNS set to built-in DNS server: %s", staticIp4_.c_str());
    }

    // ─── 6. Register event handlers ─────────────────
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &eventHandler, this, nullptr));

    // ─── 7. Start Ethernet ──────────────────────────
    status_ = EthStatus::CONNECTING;
    err = esp_eth_start(ethHandle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        status_ = EthStatus::ERROR;
        if (onError_) onError_();
        return;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Ethernet ENC28J60 initialized (static IP: %s)", staticIp4_.c_str());
}

// ─────────────────────────────────────────────────────
// Static IP
// ─────────────────────────────────────────────────────

void EthManager::setStaticIp(const std::string& ip4, const std::string& gateway,
                              const std::string& netmask)
{
    staticIp4_ = ip4;
    gateway_ = gateway;
    netmask_ = netmask;
    ESP_LOGI(TAG, "Static IP reconfigured: %s / %s / %s",
             ip4.c_str(), netmask.c_str(), gateway.c_str());
}

// ─────────────────────────────────────────────────────
// Event handler
// ─────────────────────────────────────────────────────

void EthManager::eventHandler(void* arg, esp_event_base_t eventBase,
                               int32_t eventId, void* eventData)
{
    EthManager* self = static_cast<EthManager*>(arg);
    if (self) {
        self->handleEvent(eventBase, eventId, eventData);
    }
}

void EthManager::handleEvent(esp_event_base_t base, int32_t id, void* data)
{
    if (base == ETH_EVENT) {
        switch (id) {
        case ETHERNET_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "Ethernet link up");
            status_ = EthStatus::CONNECTED;
            if (onConnected_) onConnected_();
            break;
        }

        case ETHERNET_EVENT_DISCONNECTED: {
            ESP_LOGW(TAG, "Ethernet link down");
            status_ = EthStatus::DISCONNECTED;
            ip4_.clear();
            ip6_.clear();
            if (onDisconnected_) onDisconnected_();
            break;
        }

        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            break;

        default:
            break;
        }
    }
}

} // namespace eth
} // namespace dhcp
