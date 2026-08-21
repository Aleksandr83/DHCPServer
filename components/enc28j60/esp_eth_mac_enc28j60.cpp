/*
 * ESP-IDF Ethernet MAC driver for ENC28J60 (SPI).
 * Implements the esp_eth_mac_t interface.
 */

#include "enc28j60.h"
#include "enc28j60_logging.h"
#include "eth_enc28j60_config.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdlib>

static const char* TAG = "enc28j60_mac";

/* ─── Context ──────────────────────────────────────── */

typedef struct {
    esp_eth_mac_t parent;
    esp_eth_mediator_t *eth;
    uint8_t mac_addr[6];
    int pin_cs;
    int pin_int;
    int pin_rst;
    bool is_init;
    TaskHandle_t rx_task_hdl;
} enc28j60_mac_t;

/* ─── Constants ──────────────────────────────── */
#define ENC28J60_RX_TASK_PRIO  10
#define ENC28J60_RX_TASK_STACK 4096

/* ─── Forward declarations ─────────────────────────── */

static esp_err_t enc28j60_mac_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth);
static esp_err_t enc28j60_mac_init(esp_eth_mac_t *mac);
static esp_err_t enc28j60_mac_deinit(esp_eth_mac_t *mac);
static esp_err_t enc28j60_mac_start(esp_eth_mac_t *mac);
static esp_err_t enc28j60_mac_stop(esp_eth_mac_t *mac);
static esp_err_t enc28j60_mac_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length);
static esp_err_t enc28j60_mac_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length);
static esp_err_t enc28j60_mac_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_val);
static esp_err_t enc28j60_mac_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_val);
static esp_err_t enc28j60_mac_set_addr(esp_eth_mac_t *mac, uint8_t *addr);
static esp_err_t enc28j60_mac_get_addr(esp_eth_mac_t *mac, uint8_t *addr);
static esp_err_t enc28j60_mac_set_link(esp_eth_mac_t *mac, eth_link_t link);
static esp_err_t enc28j60_mac_set_speed(esp_eth_mac_t *mac, eth_speed_t speed);
static esp_err_t enc28j60_mac_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex);
static esp_err_t enc28j60_mac_del(esp_eth_mac_t *mac);

/* Forward declaration of RX task (defined below mac_init) */
static void enc28j60_rx_task(void *arg);

/* ─── Internal helpers ─────────────────────────────── */

/* ERXRDPT must always be odd per ENC28J60 errata */
static inline uint16_t align_erxrdpt_odd(uint16_t addr)
{
    uint16_t end_addr;
    if (addr == ENC28J60_RX_BUF_START) {
        end_addr = ENC28J60_RX_BUF_END;
    } else {
        end_addr = addr - 1;
    }
    /* Ensure result is odd */
    if (end_addr % 2 == 0) {
        end_addr--;
    }
    /* Bounds check (uint16_t is always >= RX_BUF_START) */
    if (end_addr > ENC28J60_RX_BUF_END) {
        end_addr = ENC28J60_RX_BUF_END;
    }
    return end_addr;
}

/* Track next packet pointer for packet reading */
static uint16_t s_next_pkt_ptr = ENC28J60_RX_BUF_START;

static void enc28j60_init_regs(const uint8_t *mac_addr)
{
    enc28j60_reset();
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Reset ECON1 to POR state before any register init */
    enc28j60_wcr(ENC28J60_ECON1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));

    /* RX buffer: 0x0000 - 0x0FFF */
    enc28j60_wcr(ENC28J60_ERXSTL, 0x00);
    enc28j60_wcr(ENC28J60_ERXSTH, 0x00);
    enc28j60_wcr(ENC28J60_ERXNDL, 0xFF);
    enc28j60_wcr(ENC28J60_ERXNDH, 0x0F);

    /* TX buffer: 0x1000 - 0x1FFF */
    enc28j60_wcr(ENC28J60_ETXSTL, 0x00);
    enc28j60_wcr(ENC28J60_ETXSTH, 0x10);

    /* RX read pointer to start — ERXRDPT must be ODD per datasheet errata, write LOW first */
    s_next_pkt_ptr = ENC28J60_RX_BUF_START;
    uint16_t rxrdpt_init = align_erxrdpt_odd(0);
    enc28j60_wcr(ENC28J60_ERXRDPTL, rxrdpt_init & 0xFF);
    enc28j60_wcr(ENC28J60_ERXRDPTH, (rxrdpt_init >> 8) & 0xFF);

    /* MACON1: enable MAC rx + pass all */
    enc28j60_wcr(ENC28J60_MACON1, ENC28J60_MACON1_MARXEN | ENC28J60_MACON1_PASSALL);
    /* MACON3: full duplex, pad, CRC, frame length check */
    enc28j60_wcr(ENC28J60_MACON3,
                 ENC28J60_MACON3_FULDPX |
                 ENC28J60_MACON3_PADCFG0 |
                 ENC28J60_MACON3_TXCRCEN |
                 ENC28J60_MACON3_FRMLNEN);
    enc28j60_wcr(ENC28J60_MACON4, ENC28J60_MACON4_DEFER);

    /* Inter-packet gaps (full duplex) */
    enc28j60_wcr(ENC28J60_MABBIPG, 0x15);
    enc28j60_wcr(ENC28J60_MAIPGL, 0x12);
    enc28j60_wcr(ENC28J60_MAIPGH, 0x0C);

    /* Max frame length */
    enc28j60_wcr(ENC28J60_MAMXFLL, 0xFF);
    enc28j60_wcr(ENC28J60_MAMXFLH, 0x05);  /* 1535 bytes */

    /* Receive filter: accept ALL frames (promiscuous).
     * Some ENC28J60 (clone) parts reject unicast frames even to the programmed
     * MAC address (broadcasts/multicasts pass, unicast does not) — which made
     * ping fail. Accept-all sidesteps this; lwIP filters by destination. */
    enc28j60_set_rx_filter_all();
    ESP_LOGI(TAG, "RX filter: accept all (promiscuous)");

    /* MAC address — MAADR1=MSB(byte5) .. MAADR6=LSB(byte0)
       DS39662C Table 3-1: MAADR1=Bank3/0x00, MAADR6=Bank3/0x05 */
    enc28j60_wcr(ENC28J60_MAADR6, mac_addr[0]);  /* LSB */
    enc28j60_wcr(ENC28J60_MAADR5, mac_addr[1]);
    enc28j60_wcr(ENC28J60_MAADR4, mac_addr[2]);
    enc28j60_wcr(ENC28J60_MAADR3, mac_addr[3]);
    enc28j60_wcr(ENC28J60_MAADR2, mac_addr[4]);
    enc28j60_wcr(ENC28J60_MAADR1, mac_addr[5]);  /* MSB */

    /* Diagnostic: read back hardware MAC to verify the RX filter address
     * (MAADR are MAC/MII "SPRD" registers — if the readback is wrong, the
     * RX filter may be rejecting unicast frames addressed to us). */
    {
        uint8_t hwmac[6];
        hwmac[5] = enc28j60_rcr(ENC28J60_MAADR1);
        hwmac[4] = enc28j60_rcr(ENC28J60_MAADR2);
        hwmac[3] = enc28j60_rcr(ENC28J60_MAADR3);
        hwmac[2] = enc28j60_rcr(ENC28J60_MAADR4);
        hwmac[1] = enc28j60_rcr(ENC28J60_MAADR5);
        hwmac[0] = enc28j60_rcr(ENC28J60_MAADR6);
        ESP_LOGI(TAG, "MAADR readback: %02x:%02x:%02x:%02x:%02x:%02x (expect %02x:%02x:%02x:%02x:%02x:%02x)",
                 hwmac[0], hwmac[1], hwmac[2], hwmac[3], hwmac[4], hwmac[5],
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    }

    /* LEDs: LEDA = link, LEDB = activity */
    enc28j60_wcr(ENC28J60_ECOCON, 0x00);
    enc28j60_wcr(ENC28J60_EFLOCON, 0x00);

    /* Ensure bankless registers are in known state (not corrupted by any register access) */
    enc28j60_wcr(ENC28J60_EIE, 0x00);    /* disable all interrupts */
    enc28j60_wcr(ENC28J60_EIR, 0xFF);    /* clear all interrupt flags */
    enc28j60_wcr(ENC28J60_ECON2, 0x00);  /* clear ECON2 */

    /* Configure internal PHY for full-duplex (no FRCLNK — let real link detection work) */
    enc28j60_phy_write(ENC28J60_PHCON1, ENC28J60_PHCON1_PDPXMD);  /* Full duplex */
    /* Note: FRCLNK is NOT set here so that ESTAT.PHYLNK reflects real link status.
       The PHY driver reads ESTAT.PHYLNK to detect link state. */
    ESP_LOGI(TAG, "PHY configured: full duplex, real link detection");

    /* Flush any stale packets from RX buffer — walk through properly */
    {
        uint16_t walk_ptr = ENC28J60_RX_BUF_START;
        while (enc28j60_rcr(ENC28J60_EPKTCNT) > 0) {
            enc28j60_wcr(ENC28J60_ERDPTH, (walk_ptr >> 8) & 0xFF);
            enc28j60_wcr(ENC28J60_ERDPTL, walk_ptr & 0xFF);
            uint8_t hdr[6];
            enc28j60_rbm(hdr, 6);
            uint16_t nxt = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
            /* Validate nxt — if garbage, reset to RX_BUF_START */
            if (nxt > ENC28J60_RX_BUF_END) {
                nxt = ENC28J60_RX_BUF_START;
            }
            walk_ptr = nxt;
            /* Set ERXRDPT to odd value — LOW byte first per datasheet */
            uint16_t odd_nxt = align_erxrdpt_odd(nxt);
            enc28j60_wcr(ENC28J60_ERXRDPTL, odd_nxt & 0xFF);
            enc28j60_wcr(ENC28J60_ERXRDPTH, (odd_nxt >> 8) & 0xFF);
            enc28j60_bfs(ENC28J60_ECON2, ENC28J60_ECON2_PKTDEC);
        }
        s_next_pkt_ptr = ENC28J60_RX_BUF_START;
    }

    /* Force ECON1 to clean bank 0 */
    enc28j60_wcr(ENC28J60_ECON1, 0x00);

    /* Force ECON1 to bank 0 */
    enc28j60_wcr(ENC28J60_ECON1, 0x00);

    /* Diagnostic: WBM+RBM self-test with AUTOINC — validates the multi-byte
     * buffer access fast path used by RX/TX. */
    enc28j60_bfs(ENC28J60_ECON2, ENC28J60_ECON2_AUTOINC);
    {
        uint8_t test_wr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        enc28j60_wcr(ENC28J60_EWRPTL, 0x06);
        enc28j60_wcr(ENC28J60_EWRPTH, 0x00);
        enc28j60_wbm(test_wr, sizeof(test_wr));
        enc28j60_wcr(ENC28J60_ERDPTL, 0x06);
        enc28j60_wcr(ENC28J60_ERDPTH, 0x00);
        uint8_t test_rd[6];
        enc28j60_rbm(test_rd, sizeof(test_rd));
        ESP_LOGI(TAG, "Multi-byte WBM/RBM test: %s",
                 (memcmp(test_wr, test_rd, sizeof(test_wr)) == 0) ? "OK" : "MISMATCH");
    }
    /* Restore AUTOINC for normal operation */
    enc28j60_bfs(ENC28J60_ECON2, ENC28J60_ECON2_AUTOINC);

    /* Reset ERDPT to start of RX buffer */
    enc28j60_wcr(ENC28J60_ERDPTL, 0x00);
    enc28j60_wcr(ENC28J60_ERDPTH, 0x00);

    /* Enable reception LAST (after all registers configured) */
    enc28j60_bfc(ENC28J60_ECON1, ENC28J60_ECON1_DMAST);
    enc28j60_bfc(ENC28J60_EIR, 0xFF);
    enc28j60_bfs(ENC28J60_ECON1, ENC28J60_ECON1_RXEN);

    ESP_LOGI(TAG, "ENC28J60 registers initialized, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);
    /* Diagnostic: read key status registers */
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t estat_val = enc28j60_rcr(ENC28J60_ESTAT);
    uint8_t econ1_val = enc28j60_rcr(ENC28J60_ECON1);
    vTaskDelay(pdMS_TO_TICKS(1));
    uint8_t econ1_val2 = enc28j60_rcr(ENC28J60_ECON1);
    /* Read all bankless registers for diagnostics */
    uint8_t eie_val  = enc28j60_rcr(ENC28J60_EIE);
    uint8_t eir_val  = enc28j60_rcr(ENC28J60_EIR);
    uint8_t estat2   = enc28j60_rcr(ENC28J60_ESTAT);
    uint8_t econ2_val = enc28j60_rcr(ENC28J60_ECON2);
    uint8_t econ1_val3 = enc28j60_rcr(ENC28J60_ECON1);
    ESP_LOGI(TAG, "Bankless dump: EIE=0x%02X EIR=0x%02X ESTAT=0x%02X ECON2=0x%02X ECON1=0x%02X",
             eie_val, eir_val, estat2, econ2_val, econ1_val3);
    /* Also try reading ESTAT with explicit bank 0 */
    enc28j60_set_bank(0);
    uint8_t estat_bank0 = enc28j60_rcr(ENC28J60_ESTAT);
    /* Real link status lives in PHSTAT2.LSTAT (MII) — ESTAT has no PHYLNK bit */
    uint16_t phstat2_init = enc28j60_phy_read(ENC28J60_PHSTAT2);
    ESP_LOGI(TAG, "ESTAT=0x%02X (bank0), ECON1=0x%02X/0x%02X, PHSTAT2=0x%04X LSTAT=%d",
             estat_bank0, econ1_val, econ1_val2, phstat2_init,
             (phstat2_init & ENC28J60_PHSTAT2_LSTAT) ? 1 : 0);
}

/* ─── MAC interface ────────────────────────────────── */

static esp_err_t enc28j60_mac_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth)
{
    auto *ctx = (enc28j60_mac_t *)mac;
    ctx->eth = eth;
    return ESP_OK;
}

static esp_err_t enc28j60_mac_init(esp_eth_mac_t *mac)
{
    auto *ctx = (enc28j60_mac_t *)mac;
    if (ctx->is_init) return ESP_OK;

    /* SPI should already be initialized by EthManager */

    /* Pulse hardware reset before software init.
     * ENC28J60 RST pin requires minimum 5us low pulse.
     * After RST goes high, wait 50ms for internal POR + clock startup
     * before any SPI communication. */
    if (ctx->pin_rst >= 0) {
        gpio_set_direction((gpio_num_t)ctx->pin_rst, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)ctx->pin_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)ctx->pin_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(50));  /* increased from 10ms to 50ms for PHY stability */
        ESP_LOGI(TAG, "Hardware reset pulsed on GPIO%d", ctx->pin_rst);
    }

    /* Initialize ENC28J60 registers */
    enc28j60_init_regs(ctx->mac_addr);

    /* Create RX polling task (must be after init_regs which includes RXEN now) */
    BaseType_t ret = xTaskCreate(enc28j60_rx_task, "enc28j60_rx",
                                  ENC28J60_RX_TASK_STACK, ctx,
                                  ENC28J60_RX_TASK_PRIO, &ctx->rx_task_hdl);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
    } else {
        ESP_LOGI(TAG, "RX task created");
    }

    ESP_LOGI(TAG, "ENC28J60 MAC init done (rev=%d)", enc28j60_rcr(ENC28J60_EREVID));
    ctx->is_init = true;
    return ESP_OK;
}

static esp_err_t enc28j60_mac_deinit(esp_eth_mac_t *mac)
{
    auto *ctx = (enc28j60_mac_t *)mac;
    ctx->is_init = false;
    return ESP_OK;
}

/* ─── RX polling task ─────────────────────────── */

static void enc28j60_rx_task(void *arg)
{
    auto *ctx = (enc28j60_mac_t *)arg;
    uint8_t *buf = (uint8_t *)malloc(ENC28J60_MAX_PACKET);
    if (!buf) {
        ESP_LOGE(TAG, "RX task: malloc failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RX task started");

    /* Periodic ERXFCON refresh using BFS (more reliable than WCR for this reg) */
    uint32_t erxf_refresh = 0;
#ifdef ENC28J60_PKT_LOG_ENABLED
    uint32_t diag_counter = 0;
#endif
    while (1) {
        uint32_t length = ENC28J60_MAX_PACKET;
        esp_err_t err = enc28j60_mac_receive(&ctx->parent, buf, &length);
        if (err == ESP_OK) {
#ifdef ENC28J60_PKT_LOG_ENABLED
            ESP_LOGI(TAG, "RX: got pkt len=%lu", (unsigned long)length);
#endif
            /* Strip 4-byte CRC before forwarding to stack */
            if (length >= 4) length -= 4;
#ifdef ENC28J60_PKT_LOG_ENABLED
            /* Diagnostic: dump frame header (dst/src MAC + ethertype) */
            if (length >= 14) {
                ESP_LOGI(TAG, "RX frame: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%04x",
                         buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
                         buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
                         ((uint16_t)buf[12] << 8) | buf[13]);
                /* For ARP frames also print opcode + target IP (ARP payload:
                 * htype@14, ptype@16, hlen@18, plen@19, oper@20-21,
                 * sha@22-27, spa@28-31, tha@32-37, tpa@38-41) */
                if (length >= 42 && buf[12] == 0x08 && buf[13] == 0x06) {
                    uint16_t oper = ((uint16_t)buf[20] << 8) | buf[21];
                    ESP_LOGI(TAG, "  ARP: op=%u tgt=%u.%u.%u.%u",
                             oper, buf[38], buf[39], buf[40], buf[41]);
                }
            } else {
                ESP_LOGW(TAG, "RX frame too short: len=%lu", (unsigned long)length);
            }
#endif
            if (ctx->eth && ctx->eth->stack_input) {
                esp_err_t in_ret = ctx->eth->stack_input(ctx->eth, buf, length);
                if (in_ret != ESP_OK) {
                    ESP_LOGW(TAG, "stack_input returned: %s", esp_err_to_name(in_ret));
                }
                /* stack_input takes ownership — allocate new buffer */
                buf = (uint8_t *)malloc(ENC28J60_MAX_PACKET);
                if (!buf) {
                    ESP_LOGE(TAG, "RX task: buffer alloc failed");
                    vTaskDelete(NULL);
                    return;
                }
            } else {
                ESP_LOGW(TAG, "RX: stack_input not ready — dropping %lu bytes", (unsigned long)length);
            }
        } else {
            /* Refresh ERXFCON every ~1 second */
            if (++erxf_refresh >= 100) {
                erxf_refresh = 0;
                /* Refresh receive filter (accept-all) */
                enc28j60_set_rx_filter_all();
#ifdef ENC28J60_PKT_LOG_ENABLED
                /* Read real ESTAT/ECON2 from BANK0 (common regs at 0x1B-0x1F
                   are shadowed by MAADR when bank=2) */
                uint8_t saved_bank = enc28j60_rcr(ENC28J60_ECON1) & 0x03;
                enc28j60_set_bank(0);
                uint8_t eie   = enc28j60_rcr(ENC28J60_EIE);
                uint8_t eir   = enc28j60_rcr(ENC28J60_EIR);
                uint8_t estat = enc28j60_rcr(ENC28J60_ESTAT);
                uint8_t econ2 = enc28j60_rcr(ENC28J60_ECON2);
                uint8_t econ1 = enc28j60_rcr(ENC28J60_ECON1);
                enc28j60_set_bank(saved_bank);
                uint8_t pktcnt = enc28j60_rcr(ENC28J60_EPKTCNT);
                uint16_t phstat2 = enc28j60_phy_read(ENC28J60_PHSTAT2);
                ESP_LOGI(TAG, "DIAG: EIE=0x%02X EIR=0x%02X EST=0x%02X E2=0x%02X E1=0x%02X PKT=%u PHSTAT2=0x%04X LSTAT=%d",
                         eie, eir, estat, econ2, econ1, pktcnt, phstat2,
                         (phstat2 & ENC28J60_PHSTAT2_LSTAT) ? 1 : 0);
#endif
            }
#ifdef ENC28J60_PKT_LOG_ENABLED
            /* Diagnostic every 10 seconds */
            if (++diag_counter >= 1000) {
                diag_counter = 0;
                uint8_t econ1 = enc28j60_rcr(ENC28J60_ECON1);
                uint8_t pktcnt = enc28j60_rcr(ENC28J60_EPKTCNT);
                uint8_t erxfc = enc28j60_rcr(ENC28J60_ERXFCON);
                ESP_LOGI(TAG, "DIAG: ECON1=0x%02X PKTCNT=%u ERXFC=0x%02X",
                         econ1, pktcnt, erxfc);
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static esp_err_t enc28j60_mac_start(esp_eth_mac_t *mac)
{
    /* Note: ESP-IDF does NOT call mac->start(). Reception is enabled in mac->init(). */
    (void)mac;
    return ESP_OK;
}

static esp_err_t enc28j60_mac_stop(esp_eth_mac_t *mac)
{
    /* Note: ESP-IDF does NOT call mac->stop(). Kept for interface completeness. */
    (void)mac;
    return ESP_OK;
}

static esp_err_t enc28j60_mac_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length)
{
    (void)mac;

#ifdef ENC28J60_PKT_LOG_ENABLED
    /* Diagnostic: show what the stack asks us to send */
    ESP_LOGI(TAG, "TX: len=%lu dst=%02x:%02x:%02x:%02x:%02x:%02x type=0x%04x",
             (unsigned long)length,
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
             ((uint16_t)buf[12] << 8) | buf[13]);
#endif

    if (length < 60) length = 60;

    /* Wait for previous TX to complete */
    int timeout = 200;
    while (timeout-- && (enc28j60_rcr(ENC28J60_ECON1) & ENC28J60_ECON1_TXRTS)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!timeout) ESP_LOGW(TAG, "TX timeout waiting for TXRTS clear");

    uint16_t tx_start = ENC28J60_TX_BUF_START;
    uint16_t tx_end = tx_start + length;

    /* Set TX start/end — write LOW byte first (proven STM32 order) */
    enc28j60_wcr(ENC28J60_ETXSTL, tx_start & 0xFF);
    enc28j60_wcr(ENC28J60_ETXSTH, (tx_start >> 8) & 0xFF);
    enc28j60_wcr(ENC28J60_ETXNDL, tx_end & 0xFF);
    enc28j60_wcr(ENC28J60_ETXNDH, (tx_end >> 8) & 0xFF);

    /* Write TX buffer: per-packet control byte + data */
    enc28j60_wcr(ENC28J60_EWRPTL, tx_start & 0xFF);
    enc28j60_wcr(ENC28J60_EWRPTH, (tx_start >> 8) & 0xFF);
    uint8_t pkt_ctrl = 0x00;
    enc28j60_wbm(&pkt_ctrl, 1);
    enc28j60_wbm(buf, length);

    /* Request transmission */
    enc28j60_bfs(ENC28J60_ECON1, ENC28J60_ECON1_TXRTS);

    return ESP_OK;
}

static esp_err_t enc28j60_mac_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length)
{
    (void)mac;

    uint8_t pktcnt = enc28j60_rcr(ENC28J60_EPKTCNT);
    if (pktcnt == 0) {
        *length = 0;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGD(TAG, "RX: pktcnt=%u nxt=0x%04X", pktcnt, s_next_pkt_ptr);

    /* Move read pointer to next packet — write LOW byte first per ENC28J60 spec */
    enc28j60_wcr(ENC28J60_ERDPTL, s_next_pkt_ptr & 0xFF);
    enc28j60_wcr(ENC28J60_ERDPTH, (s_next_pkt_ptr >> 8) & 0xFF);

    /* Read 6-byte header: Next Packet Pointer(2) + RSV(4) */
    uint8_t hdr[6];
    enc28j60_rbm(hdr, 6);

    uint16_t next_pkt = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    /* RSV[0] (hdr[2]) = Frame Length[7:0], RSV[1] (hdr[3]) bits[3:0] = Frame Length[11:8] */
    uint16_t frame_len = (uint16_t)hdr[2] | (((uint16_t)hdr[3] & 0x0F) << 8);

    if (frame_len > 1520 || frame_len == 0) {
        uint8_t pktcnt_now = enc28j60_rcr(ENC28J60_EPKTCNT);
        /* Diagnostic: read ERXWRPT to see where hardware writes packets */
        uint16_t erxwrpt = enc28j60_rcr(ENC28J60_ERXWRPTL);
        erxwrpt |= (uint16_t)enc28j60_rcr(ENC28J60_ERXWRPTH) << 8;
        ESP_LOGW(TAG, "Bad frame len: %u (nxt=0x%04X, hdr=%02X %02X %02X %02X %02X %02X, pktcnt=%u, ERXWRPT=0x%04X) — resetting to start",
                 frame_len, s_next_pkt_ptr,
                 hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5],
                 pktcnt_now, erxwrpt);
        /* Read packet header at ERXWRPT if it differs from current position */
        if (erxwrpt != s_next_pkt_ptr && erxwrpt <= ENC28J60_RX_BUF_END) {
            enc28j60_wcr(ENC28J60_ERDPTL, erxwrpt & 0xFF);
            enc28j60_wcr(ENC28J60_ERDPTH, (erxwrpt >> 8) & 0xFF);
            uint8_t hdr2[6];
            enc28j60_rbm(hdr2, 6);
            uint16_t nxt2 = (uint16_t)hdr2[0] | ((uint16_t)hdr2[1] << 8);
            uint16_t len2 = (uint16_t)hdr2[2] | (((uint16_t)hdr2[3] & 0x0F) << 8);
            ESP_LOGW(TAG, "  Data at ERXWRPT: nxt=0x%04X len=%u hdr=%02X %02X %02X %02X %02X %02X",
                     nxt2, len2, hdr2[0], hdr2[1], hdr2[2], hdr2[3], hdr2[4], hdr2[5]);
        }
        *length = 0;
        /* Bad frame — discard it and reset to start of RX buffer (low byte first) */
        uint16_t odd_start = align_erxrdpt_odd(ENC28J60_RX_BUF_START);
        enc28j60_wcr(ENC28J60_ERXRDPTL, odd_start & 0xFF);
        enc28j60_wcr(ENC28J60_ERXRDPTH, (odd_start >> 8) & 0xFF);
        enc28j60_bfs(ENC28J60_ECON2, ENC28J60_ECON2_PKTDEC);
        s_next_pkt_ptr = ENC28J60_RX_BUF_START;
        return ESP_ERR_INVALID_SIZE;
    }

    /* Read packet data (skip 6-byte header, already positioned after it) */
    if (buf && *length >= frame_len) {
        enc28j60_rbm(buf, frame_len);
        *length = frame_len;
    } else {
        auto* skip = (uint8_t*)malloc(frame_len);
        if (skip) {
            enc28j60_rbm(skip, frame_len);
            free(skip);
        }
        *length = 0;
    }

    /* Free buffer: set ERXRDPT to odd value before PKTDEC (low byte first) */
    uint16_t odd_rpt = align_erxrdpt_odd(next_pkt);
    enc28j60_wcr(ENC28J60_ERXRDPTL, odd_rpt & 0xFF);
    enc28j60_wcr(ENC28J60_ERXRDPTH, (odd_rpt >> 8) & 0xFF);
    enc28j60_bfs(ENC28J60_ECON2, ENC28J60_ECON2_PKTDEC);
    s_next_pkt_ptr = next_pkt;

    return ESP_OK;
}

static esp_err_t enc28j60_mac_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr,
                                            uint32_t phy_reg, uint32_t *reg_val)
{
    (void)mac;
    (void)phy_addr;
    *reg_val = enc28j60_phy_read((uint8_t)phy_reg);
    return ESP_OK;
}

static esp_err_t enc28j60_mac_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr,
                                             uint32_t phy_reg, uint32_t reg_val)
{
    (void)mac;
    (void)phy_addr;
    enc28j60_phy_write((uint8_t)phy_reg, (uint16_t)reg_val);
    return ESP_OK;
}

static esp_err_t enc28j60_mac_set_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    auto *ctx = (enc28j60_mac_t *)mac;
    memcpy(ctx->mac_addr, addr, 6);
    if (ctx->is_init) {
        enc28j60_wcr(ENC28J60_MAADR6, addr[0]);  /* LSB */
        enc28j60_wcr(ENC28J60_MAADR5, addr[1]);
        enc28j60_wcr(ENC28J60_MAADR4, addr[2]);
        enc28j60_wcr(ENC28J60_MAADR3, addr[3]);
        enc28j60_wcr(ENC28J60_MAADR2, addr[4]);
        enc28j60_wcr(ENC28J60_MAADR1, addr[5]);  /* MSB */
    }
    return ESP_OK;
}

static esp_err_t enc28j60_mac_get_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    auto *ctx = (enc28j60_mac_t *)mac;
    memcpy(addr, ctx->mac_addr, 6);
    return ESP_OK;
}

static esp_err_t enc28j60_mac_set_link(esp_eth_mac_t *mac, eth_link_t link)
{
    (void)mac;
    if (link == ETH_LINK_UP) {
        /* Ensure RXEN is set when link comes up */
        enc28j60_bfs(ENC28J60_ECON1, ENC28J60_ECON1_RXEN);
    }
    return ESP_OK;
}

static esp_err_t enc28j60_mac_set_speed(esp_eth_mac_t *mac, eth_speed_t speed)
{
    /* ENC28J60 is 10Mbps only */
    (void)mac;
    (void)speed;
    return ESP_OK;
}

static esp_err_t enc28j60_mac_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex)
{
    /* Duplex was configured in init_regs, don't override */
    (void)mac;
    (void)duplex;
    return ESP_OK;
}

static esp_err_t enc28j60_mac_add_mac_filter(esp_eth_mac_t *mac, uint8_t *addr)
{
    (void)mac;
    (void)addr;
    /* ENC28J60 has no per-address filter; all multicast is accepted via MCEN in ERXFCON */
    return ESP_OK;
}

static esp_err_t enc28j60_mac_rm_mac_filter(esp_eth_mac_t *mac, uint8_t *addr)
{
    (void)mac;
    (void)addr;
    return ESP_OK;
}

static esp_err_t enc28j60_mac_del(esp_eth_mac_t *mac)
{
    free(mac);
    return ESP_OK;
}

/* ─── Factory function ─────────────────────────────── */

extern "C" esp_eth_mac_t *esp_eth_mac_new_enc28j60(const eth_enc28j60_config_t *enc28j60_config,
                                         const eth_mac_config_t *mac_config)
{
    auto *ctx = (enc28j60_mac_t*)calloc(1, sizeof(enc28j60_mac_t));
    if (!ctx) return nullptr;

    ctx->pin_cs = enc28j60_config->cs_gpio_num;
    ctx->pin_int = enc28j60_config->int_gpio_num;
    ctx->pin_rst = 16;

    /* Generate unique MAC from ESP32 base MAC.
     * Use the ESP32's built-in MAC (from eFuse) as the basis,
     * but set the locally-administered bit (bit 1 of first byte)
     * to avoid conflicts with the ESP32's WiFi/BT MAC. */
    {
        uint8_t base_mac[6] = {0};
        esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
        /* Copy 5 bytes from ESP32 base MAC, use a custom last byte */
        ctx->mac_addr[0] = base_mac[0] | 0x02;  /* set locally-administered bit */
        ctx->mac_addr[1] = base_mac[1];
        ctx->mac_addr[2] = base_mac[2];
        ctx->mac_addr[3] = base_mac[3];
        ctx->mac_addr[4] = base_mac[4];
        ctx->mac_addr[5] = 0xED;  /* custom suffix to distinguish from WiFi MAC */
        ESP_LOGI(TAG, "Generated MAC: %02x:%02x:%02x:%02x:%02x:%02x (from ESP32 base)",
                 ctx->mac_addr[0], ctx->mac_addr[1], ctx->mac_addr[2],
                 ctx->mac_addr[3], ctx->mac_addr[4], ctx->mac_addr[5]);
    }

    /* Fill function pointers */
    ctx->parent.set_mediator = enc28j60_mac_set_mediator;
    ctx->parent.init = enc28j60_mac_init;
    ctx->parent.deinit = enc28j60_mac_deinit;
    ctx->parent.start = enc28j60_mac_start;
    ctx->parent.stop = enc28j60_mac_stop;
    ctx->parent.transmit = enc28j60_mac_transmit;
    ctx->parent.receive = enc28j60_mac_receive;
    ctx->parent.read_phy_reg = enc28j60_mac_read_phy_reg;
    ctx->parent.write_phy_reg = enc28j60_mac_write_phy_reg;
    ctx->parent.set_addr = enc28j60_mac_set_addr;
    ctx->parent.get_addr = enc28j60_mac_get_addr;
    ctx->parent.set_link = enc28j60_mac_set_link;
    ctx->parent.set_speed = enc28j60_mac_set_speed;
    ctx->parent.set_duplex = enc28j60_mac_set_duplex;
    ctx->parent.add_mac_filter = enc28j60_mac_add_mac_filter;
    ctx->parent.rm_mac_filter = enc28j60_mac_rm_mac_filter;
    ctx->parent.del = enc28j60_mac_del;

    ESP_LOGI(TAG, "ENC28J60 MAC created (CS=%d, INT=%d)", ctx->pin_cs, ctx->pin_int);
    return &ctx->parent;
}
