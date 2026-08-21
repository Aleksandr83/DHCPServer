/*
 * Minimal ENC28J60 internal PHY driver.
 * Link status is read via MII from PHSTAT2.LSTAT (bit 10).
 * NOTE: earlier attempts read ESTAT bit 4 as "PHYLNK" — that bit is LATECOL, not link.
 * MII reads previously appeared to hang because ECON2.AUTOINC was misdefined as 0x20
 * (which is actually PWRSV), putting the chip into power-save mode. With AUTOINC=0x80
 * fixed, MII reads complete normally.
 */

#include "enc28j60.h"
#include "esp_eth.h"
#include "esp_eth_phy.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdlib>
#include <cinttypes>

static const char* TAG = "enc28j60_phy";

typedef struct {
    esp_eth_phy_t parent;
    esp_eth_mediator_t *eth;
    uint32_t addr;
    eth_link_t link_status;
    int reset_gpio;
} enc28j60_phy_t;

static esp_err_t set_mediator(esp_eth_phy_t *phy, esp_eth_mediator_t *eth)
{
    ((enc28j60_phy_t*)phy)->eth = eth;
    return ESP_OK;
}

static esp_err_t init(esp_eth_phy_t *phy)
{
    (void)phy;
    /* НЕ дёргаем RST — mac_init уже сделал софт-резет (0xFF).
       Если дёрнуть RST после mac_init, все регистры сбросятся в POR. */
    ESP_LOGI(TAG, "PHY initialized (real link status via PHSTAT2)");
    return ESP_OK;
}

static esp_err_t deinit(esp_eth_phy_t *phy) { (void)phy; return ESP_OK; }
static esp_err_t del(esp_eth_phy_t *phy) { free(phy); return ESP_OK; }
static esp_err_t reset(esp_eth_phy_t *phy) { (void)phy; return ESP_OK; }

static esp_err_t reset_hw(esp_eth_phy_t *phy)
{
    /* RST is shared with MAC — don't pulse it here to avoid resetting MAC */
    (void)phy;
    return ESP_OK;
}

static esp_err_t autonego_ctrl(esp_eth_phy_t *phy, eth_phy_autoneg_cmd_t cmd, bool *autonego_en_stat)
{
    (void)phy;
    if (cmd == ESP_ETH_PHY_AUTONEGO_G_STAT) *autonego_en_stat = true;
    return ESP_OK;
}

static esp_err_t get_link(esp_eth_phy_t *phy)
{
    auto *ctx = (enc28j60_phy_t*)phy;
    if (!ctx->eth) return ESP_ERR_INVALID_STATE;

    /* Real link status is in PHSTAT2.LSTAT (bit 10) via MII.
     * ESTAT has NO PHYLNK bit (bit 4 is LATECOL), so we read PHSTAT2.
     * This goes through mediator -> mac->read_phy_reg -> MII. */
    uint32_t phstat2 = 0;
    esp_err_t ret = ctx->eth->phy_reg_read(ctx->eth, ctx->addr, ENC28J60_PHSTAT2, &phstat2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PHSTAT2 read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    bool link_up = (phstat2 & ENC28J60_PHSTAT2_LSTAT) != 0;
    eth_link_t new_link = link_up ? ETH_LINK_UP : ETH_LINK_DOWN;

    if (new_link != ctx->link_status) {
        ctx->link_status = new_link;
        ESP_LOGI(TAG, "Link %s (PHSTAT2=0x%04X)", new_link == ETH_LINK_UP ? "UP" : "DOWN", (uint16_t)phstat2);
        ctx->eth->on_state_changed(ctx->eth, ETH_STATE_LINK, (void*)new_link);
    }
    return ESP_OK;
}

static esp_err_t set_link(esp_eth_phy_t *phy, eth_link_t link)
{
    ((enc28j60_phy_t*)phy)->link_status = link;
    return ESP_OK;
}

static esp_err_t pwrctl(esp_eth_phy_t *phy, bool enable) { (void)phy; (void)enable; return ESP_OK; }
static esp_err_t set_addr(esp_eth_phy_t *phy, uint32_t addr) { ((enc28j60_phy_t*)phy)->addr = addr; return ESP_OK; }
static esp_err_t get_addr(esp_eth_phy_t *phy, uint32_t *addr) { *addr = ((enc28j60_phy_t*)phy)->addr; return ESP_OK; }
static esp_err_t set_speed(esp_eth_phy_t *phy, eth_speed_t s) { (void)phy; (void)s; return ESP_OK; }
static esp_err_t set_duplex(esp_eth_phy_t *phy, eth_duplex_t d) { (void)phy; (void)d; return ESP_OK; }
static esp_err_t advertise_pause_ability(esp_eth_phy_t *phy, uint32_t a) { (void)phy; (void)a; return ESP_OK; }
static esp_err_t loopback(esp_eth_phy_t *phy, bool en) { (void)phy; (void)en; return ESP_OK; }

// ─── Factory ────────────────────────────────────────

extern "C" esp_eth_phy_t *esp_eth_phy_new_enc28j60(const eth_phy_config_t *config)
{
    auto *ctx = (enc28j60_phy_t*)calloc(1, sizeof(enc28j60_phy_t));
    if (!ctx) return nullptr;

    ctx->addr = config->phy_addr;
    ctx->reset_gpio = config->reset_gpio_num;
    ctx->link_status = ETH_LINK_DOWN;

    ctx->parent.set_mediator = set_mediator;
    ctx->parent.reset = reset;
    ctx->parent.reset_hw = reset_hw;
    ctx->parent.init = init;
    ctx->parent.deinit = deinit;
    ctx->parent.del = del;
    ctx->parent.autonego_ctrl = autonego_ctrl;
    ctx->parent.get_link = get_link;
    ctx->parent.set_link = set_link;
    ctx->parent.pwrctl = pwrctl;
    ctx->parent.set_addr = set_addr;
    ctx->parent.get_addr = get_addr;
    ctx->parent.set_speed = set_speed;
    ctx->parent.set_duplex = set_duplex;
    ctx->parent.advertise_pause_ability = advertise_pause_ability;
    ctx->parent.loopback = loopback;

    ESP_LOGI(TAG, "PHY created (addr=%" PRIu32 ", RST=%d)", ctx->addr, ctx->reset_gpio);
    return &ctx->parent;
}
