/*
 * Minimal ENC28J60 SPI driver.
 *
 * Uses a single SPI device with command_bits=0, address_bits=0.
 * All opcodes are sent as raw bytes in the data phase:
 *
 *   RCR: send [0x00|reg, 0xFF], receive [dummy, regval]
 *   WCR: send [0x40|reg, val]
 *   BFS: send [0x80|reg, mask]
 *   BFC: send [0xA0|reg, mask]
 *   RBM: send [0x3A, 0xFF, 0xFF, ...], receive [dummy, data...]
 *   WBM: send [0x7A, data0, data1, ...]
 *   SRC: send [0xFF]
 *
 * Small transactions (RCR, WCR, BFS, BFC) use SPI_TRANS_USE_TXDATA/RXDATA.
 * Large transactions (RBM, WBM) use pre-allocated DMA-safe buffers.
 *
 * CS is controlled by the SPI hardware (spics_io_num = cs pin).
 */

#include "enc28j60.h"
#include <cstring>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char* TAG = "enc28j60";

static spi_device_handle_t s_spi_dev = nullptr;

/* ENC28J60 is accessed from several tasks: the RX task, the esp_eth link-check
 * timer (which calls phy->get_link() -> MII read), the lwIP TX path and the app
 * context. spi_device_polling_transmit() is NOT safe to call from two tasks at
 * once ("previous polling transaction not terminated"), so every SPI operation
 * must be serialized. A recursive mutex is used because set_bank() calls
 * bfs()/bfc() internally. */
static SemaphoreHandle_t s_spi_mutex = nullptr;

static inline void spi_lock(void)
{
    if (s_spi_mutex) {
        xSemaphoreTakeRecursive(s_spi_mutex, portMAX_DELAY);
    }
}

static inline void spi_unlock(void)
{
    if (s_spi_mutex) {
        xSemaphoreGiveRecursive(s_spi_mutex);
    }
}

/* Current bank tracking — starts at 0xFF to force initial switch */
static uint8_t s_current_bank = 0xFF;

/* Pre-allocated DMA-safe buffer for WBM writes (opcode + data) */
static uint8_t *s_wbm_buf = nullptr;

/* Pre-allocated DMA-safe buffer for RBM reads (opcode + data) */
static uint8_t *s_rbm_buf = nullptr;

#define ENC28J60_BUF_OPCODE_SIZE 1     /* one byte for the opcode */
#define ENC28J60_BUF_MAX_PAYLOAD  (ENC28J60_MAX_PACKET + 8)  /* worst-case frame + slack */

/* Bank helper: extract bank number from encoded address */
static inline uint8_t enc28j60_bank_of(uint16_t addr)
{
    return (uint8_t)((addr >> 8) & 0x03);
}

static inline uint8_t enc28j60_reg_of(uint16_t addr)
{
    return (uint8_t)(addr & 0x1F);
}

/* ─── Public API ───────────────────────────────────── */

/* CS hold time per ENC28J60 datasheet (min 210ns) */
#define ENC28J60_CS_HOLD_TIME_MIN_NS 210

static uint8_t enc28j60_cal_spi_cs_hold_time(int clock_speed_hz)
{
    if (clock_speed_hz <= 0) return 0;
    int clock_speed_mhz = (clock_speed_hz + 999999) / 1000000;
    if (clock_speed_mhz > 20) clock_speed_mhz = 20;
    int temp = clock_speed_mhz * ENC28J60_CS_HOLD_TIME_MIN_NS;
    uint8_t cs_posttrans = temp / 1000;
    if (temp % 1000) {
        cs_posttrans += 1;
    }
    return cs_posttrans;
}

esp_err_t enc28j60_spi_init(int host, int mosi, int miso, int sclk, int cs)
{
    if (s_spi_dev) {
        return ESP_OK;
    }

    /* Create the serialization mutex before any SPI traffic */
    if (!s_spi_mutex) {
        s_spi_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_spi_mutex) {
            ESP_LOGE(TAG, "Failed to create SPI mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    spi_bus_config_t bus_cfg;
    memset(&bus_cfg, 0, sizeof(bus_cfg));
    bus_cfg.mosi_io_num = mosi;
    bus_cfg.miso_io_num = miso;
    bus_cfg.sclk_io_num = sclk;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;

    /* Enable DMA for reliable large SPI transfers (RBM/WBM) */
    esp_err_t err = spi_bus_initialize((spi_host_device_t)host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Single SPI device with command_bits=0, address_bits=0.
     * All opcodes are sent as raw bytes in the data phase. */
    spi_device_interface_config_t dev_cfg;
    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.mode = 0;
    dev_cfg.clock_speed_hz = 2 * 1000 * 1000;
    dev_cfg.spics_io_num = cs;
    dev_cfg.queue_size = 1;
    dev_cfg.flags = 0;
    dev_cfg.command_bits = 0;
    dev_cfg.address_bits = 0;
    dev_cfg.cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(dev_cfg.clock_speed_hz);

    err = spi_bus_add_device((spi_host_device_t)host, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Pre-allocate DMA-safe buffers for RBM and WBM */
    s_wbm_buf = (uint8_t*)heap_caps_malloc(ENC28J60_BUF_OPCODE_SIZE + ENC28J60_BUF_MAX_PAYLOAD, MALLOC_CAP_DMA);
    s_rbm_buf = (uint8_t*)heap_caps_malloc(ENC28J60_BUF_OPCODE_SIZE + ENC28J60_BUF_MAX_PAYLOAD, MALLOC_CAP_DMA);
    if (!s_wbm_buf || !s_rbm_buf) {
        ESP_LOGE(TAG, "Failed to allocate WBM/RBM buffers");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SPI init done (CS=%d, raw opcode mode)", cs);
    return ESP_OK;
}

void enc28j60_set_bank(uint8_t bank)
{
    spi_lock();
    if (bank != s_current_bank) {
        enc28j60_bfc(ENC28J60_ECON1, 0x03);
        enc28j60_bfs(ENC28J60_ECON1, bank & 0x03);
        s_current_bank = bank;
    }
    spi_unlock();
}

uint8_t enc28j60_rcr(uint16_t addr)
{
    spi_lock();

    uint8_t reg = enc28j60_reg_of(addr);
    uint8_t bank = enc28j60_bank_of(addr);

    if (reg < 0x1B) {
        enc28j60_set_bank(bank);
    }

    /*
     * RCR: send [opcode, 0xFF], receive [dummy, regval]
     * opcode = 0x00 | reg
     */
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;  /* 2 bytes: opcode + dummy */
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.tx_data[0] = ENC28J60_OP_RCR | reg;  /* 0x00 | reg */
    t.tx_data[1] = 0xFF;  /* dummy byte for clocking */

    spi_device_polling_transmit(s_spi_dev, &t);
    uint8_t val = t.rx_data[1];  /* second byte is the register value */

    spi_unlock();
    return val;
}

void enc28j60_wcr(uint16_t addr, uint8_t val)
{
    spi_lock();

    uint8_t reg = enc28j60_reg_of(addr);
    uint8_t bank = enc28j60_bank_of(addr);

    if (reg < 0x1B) {
        enc28j60_set_bank(bank);
    }

    /*
     * WCR: send [opcode, val]
     * opcode = 0x40 | reg
     */
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;  /* 2 bytes: opcode + data */
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = ENC28J60_OP_WCR | reg;  /* 0x40 | reg */
    t.tx_data[1] = val;

    spi_device_polling_transmit(s_spi_dev, &t);

    spi_unlock();
}

void enc28j60_bfs(uint16_t addr, uint8_t mask)
{
    spi_lock();

    uint8_t reg = enc28j60_reg_of(addr);
    uint8_t bank = enc28j60_bank_of(addr);

    if (reg < 0x1B) {
        enc28j60_set_bank(bank);
    }

    /*
     * BFS: send [opcode, mask]
     * opcode = 0x80 | reg
     */
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;  /* 2 bytes: opcode + data */
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = ENC28J60_OP_BFS | reg;  /* 0x80 | reg */
    t.tx_data[1] = mask;

    spi_device_polling_transmit(s_spi_dev, &t);

    spi_unlock();
}

void enc28j60_bfc(uint16_t addr, uint8_t mask)
{
    spi_lock();

    uint8_t reg = enc28j60_reg_of(addr);
    uint8_t bank = enc28j60_bank_of(addr);

    if (reg < 0x1B) {
        enc28j60_set_bank(bank);
    }

    /*
     * BFC: send [opcode, mask]
     * opcode = 0xA0 | reg
     */
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;  /* 2 bytes: opcode + data */
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = ENC28J60_OP_BFC | reg;  /* 0xA0 | reg */
    t.tx_data[1] = mask;

    spi_device_polling_transmit(s_spi_dev, &t);

    spi_unlock();
}

/*
 * ENC28J60 buffer memory access (RBM/WBM).
 *
 * Uses the same SPI device (command_bits=0, address_bits=0) with the
 * opcode byte prepended to the data.
 *
 * For WBM: tx_buffer = [0x7A, data0, data1, ...]
 * For RBM: tx_buffer = [0x3A, 0xFF, 0xFF, ...], rx_buffer receives [dummy, data...]
 */

void enc28j60_rbm(uint8_t* buf, size_t len)
{
    if (!buf || len == 0) return;
    if (!s_rbm_buf) {
        ESP_LOGE(TAG, "RBM: buffer not allocated");
        return;
    }
    if (len > ENC28J60_BUF_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "RBM: len %u exceeds max %u", (unsigned)len, (unsigned)ENC28J60_BUF_MAX_PAYLOAD);
        return;
    }

    spi_lock();

    /* Multi-byte RBM. ECON2.AUTOINC (0x80) is set during normal operation,
     * so ERDPT auto-advances and we read the whole buffer in ONE SPI
     * transaction (fast path — the old byte-at-a-time workaround was only
     * needed because AUTOINC was wrongly set to PWRSV=0x20). */
    s_rbm_buf[0] = ENC28J60_OP_RBM;  /* 0x3A */
    memset(s_rbm_buf + 1, 0xFF, len);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8 * (len + 1);  /* opcode + len data bytes */
    t.tx_buffer = s_rbm_buf;
    t.rx_buffer = s_rbm_buf;

    esp_err_t err = spi_device_polling_transmit(s_spi_dev, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RBM len %u failed: %s", (unsigned)len, esp_err_to_name(err));
        spi_unlock();
        return;
    }

    memcpy(buf, s_rbm_buf + 1, len);
    spi_unlock();
}

void enc28j60_wbm(const uint8_t* buf, size_t len)
{
    if (!buf || len == 0) return;
    if (!s_wbm_buf) {
        ESP_LOGE(TAG, "WBM: buffer not allocated");
        return;
    }
    if (len > ENC28J60_BUF_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "WBM: len %u exceeds max %u", (unsigned)len, (unsigned)ENC28J60_BUF_MAX_PAYLOAD);
        return;
    }

    spi_lock();

    /* Multi-byte WBM. With AUTOINC set, EWRPT auto-advances so the whole
     * payload is written in ONE SPI transaction (fast path). */
    s_wbm_buf[0] = ENC28J60_OP_WBM;  /* 0x7A */
    memcpy(s_wbm_buf + 1, buf, len);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8 * (len + 1);  /* opcode + len data bytes */
    t.tx_buffer = s_wbm_buf;

    esp_err_t err = spi_device_polling_transmit(s_spi_dev, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WBM len %u failed: %s", (unsigned)len, esp_err_to_name(err));
        spi_unlock();
        return;
    }

    spi_unlock();
}

uint16_t enc28j60_phy_read(uint8_t addr)
{
    spi_lock();

    enc28j60_wcr(ENC28J60_MIREGADR, addr);
    enc28j60_bfs(ENC28J60_MICMD, ENC28J60_MICMD_MIIRD);

    int timeout = 1000;
    bool busy_cleared = false;
    while (timeout--) {
        uint8_t mistat = enc28j60_rcr(ENC28J60_MISTAT);
        if (!(mistat & ENC28J60_MISTAT_BUSY)) {
            busy_cleared = true;
            break;
        }
        /* Busy-spin: phy_read may be called from the esp_timer link-check
         * callback, where vTaskDelay() is not allowed. */
        esp_rom_delay_us(10);
    }

    if (!busy_cleared) {
        ESP_LOGW(TAG, "PHY read timeout for reg %u", addr);
    }

    enc28j60_bfc(ENC28J60_MICMD, ENC28J60_MICMD_MIIRD);
    uint16_t val = enc28j60_rcr(ENC28J60_MIRDL);
    val |= (uint16_t)enc28j60_rcr(ENC28J60_MIRDH) << 8;

    spi_unlock();
    return val;
}

void enc28j60_phy_write(uint8_t addr, uint16_t val)
{
    spi_lock();
    enc28j60_wcr(ENC28J60_MIREGADR, addr);
    enc28j60_wcr(ENC28J60_MIWRL, val & 0xFF);
    enc28j60_wcr(ENC28J60_MIWRH, (val >> 8) & 0xFF);
    vTaskDelay(pdMS_TO_TICKS(2));
    spi_unlock();
}

void enc28j60_reset(void)
{
    /*
     * Software reset: send 0xFF repeatedly while holding CS low.
     * Per ENC28J60 datasheet (Section 6.1 "Reset"):
     *   "The System Reset (SC) command is executed by holding CS low
     *    and clocking in at least 5 bytes of 0xFF."
     * We send 6 bytes to be safe.
     *
     * NOTE: SPI_TRANS_USE_TXDATA only supports up to 32 bits (4 bytes),
     * so for 6 bytes we must use a DMA-safe buffer.
     */
    if (!s_spi_dev) return;

    spi_lock();

    /* Use pre-allocated WBM buffer for the reset sequence (6 bytes of 0xFF) */
    uint8_t* reset_buf = s_wbm_buf;  /* DMA-capable, 1519+1 bytes */
    memset(reset_buf, 0xFF, 6);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 48;  /* 6 bytes: 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF */
    t.tx_buffer = reset_buf;
    t.rx_buffer = NULL;  /* no RX needed for reset */

    esp_err_t err = spi_device_polling_transmit(s_spi_dev, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Software reset SPI transaction failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify CLKRDY after reset */
    int timeout = 2000;
    while (timeout--) {
        if (enc28j60_rcr(ENC28J60_ESTAT) & ENC28J60_ESTAT_CLKRDY) {
            ESP_LOGI(TAG, "ENC28J60 ready (CLKRDY)");
            s_current_bank = 0;  /* reset forces bank 0 */
            spi_unlock();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(TAG, "ENC28J60 CLKRDY timeout after 2s — continuing anyway");
    spi_unlock();
}

void enc28j60_set_rx_filter_all(void)
{
    /* Accept all frames (promiscuous) without relying on UCEN or the pattern
     * match — both are unreliable on this ENC28J60 clone.
     *
     * Empirical findings on this unit:
     *   - ERXFCON.ANDOR=0 => OR mode (any enabled check passes).
     *     ERXFCON.ANDOR=1 => AND mode (ALL enabled checks must pass).
     *   - The pattern match (PMEN) does NOT match frames here, and UCEN rejects
     *     unicast even to the correctly programmed MAC.
     *
     * So: set ANDOR=1 (AND) with NO address-filter checks enabled -> the address
     * filter passes vacuously (AND over an empty set = true); only the CRC gate
     * remains, so every valid frame is accepted. ERXFCON = 0x60 = CRCEN|ANDOR
     * (same value used by the tuxgraphics ENC28J60 stack). lwIP filters frames
     * by destination address afterwards. */
    enc28j60_wcr(ENC28J60_ERXFCON,
                 ENC28J60_ERXFCON_CRCEN |
                 ENC28J60_ERXFCON_ANDOR);
}
