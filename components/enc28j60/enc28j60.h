/*
 * Minimal ENC28J60 register map and SPI interface.
 */

#ifndef ENC28J60_H
#define ENC28J60_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPI opcodes */
#define ENC28J60_OP_RCR  0x00  /* Read Control Register */
#define ENC28J60_OP_WCR  0x40  /* Write Control Register */
#define ENC28J60_OP_BFS  0x80  /* Bit Field Set */
#define ENC28J60_OP_BFC  0xA0  /* Bit Field Clear */
#define ENC28J60_OP_RBM  0x3A  /* Read Buffer Memory */
#define ENC28J60_OP_WBM  0x7A  /* Write Buffer Memory */

/* Register banks */
#define ENC28J60_BANK0(reg)  (reg)
#define ENC28J60_BANK1(reg)  ((reg) | 0x100)
#define ENC28J60_BANK2(reg)  ((reg) | 0x200)
#define ENC28J60_BANK3(reg)  ((reg) | 0x300)

/* Bank 0 registers */
#define ENC28J60_ERDPTL    ENC28J60_BANK0(0x00)
#define ENC28J60_ERDPTH    ENC28J60_BANK0(0x01)
#define ENC28J60_EWRPTL    ENC28J60_BANK0(0x02)
#define ENC28J60_EWRPTH    ENC28J60_BANK0(0x03)
#define ENC28J60_ETXSTL    ENC28J60_BANK0(0x04)
#define ENC28J60_ETXSTH    ENC28J60_BANK0(0x05)
#define ENC28J60_ETXNDL    ENC28J60_BANK0(0x06)
#define ENC28J60_ETXNDH    ENC28J60_BANK0(0x07)
#define ENC28J60_ERXSTL    ENC28J60_BANK0(0x08)
#define ENC28J60_ERXSTH    ENC28J60_BANK0(0x09)
#define ENC28J60_ERXNDL    ENC28J60_BANK0(0x0A)
#define ENC28J60_ERXNDH    ENC28J60_BANK0(0x0B)
#define ENC28J60_ERXRDPTL  ENC28J60_BANK0(0x0C)
#define ENC28J60_ERXRDPTH  ENC28J60_BANK0(0x0D)
#define ENC28J60_ERXWRPTL  ENC28J60_BANK0(0x0E)
#define ENC28J60_ERXWRPTH  ENC28J60_BANK0(0x0F)
#define ENC28J60_EDMASTL   ENC28J60_BANK0(0x10)
#define ENC28J60_EDMASTH   ENC28J60_BANK0(0x11)
#define ENC28J60_EDMANDL   ENC28J60_BANK0(0x12)
#define ENC28J60_EDMANDH   ENC28J60_BANK0(0x13)
#define ENC28J60_EDMADSTL  ENC28J60_BANK0(0x14)
#define ENC28J60_EDMADSTH  ENC28J60_BANK0(0x15)
#define ENC28J60_EDMACSL   ENC28J60_BANK0(0x16)
#define ENC28J60_EDMACH    ENC28J60_BANK0(0x17)

/* Bank 1 registers */
#define ENC28J60_EHT0      ENC28J60_BANK1(0x00)
#define ENC28J60_EHT1      ENC28J60_BANK1(0x01)
#define ENC28J60_EHT2      ENC28J60_BANK1(0x02)
#define ENC28J60_EHT3      ENC28J60_BANK1(0x03)
#define ENC28J60_EHT4      ENC28J60_BANK1(0x04)
#define ENC28J60_EHT5      ENC28J60_BANK1(0x05)
#define ENC28J60_EHT6      ENC28J60_BANK1(0x06)
#define ENC28J60_EHT7      ENC28J60_BANK1(0x07)
#define ENC28J60_EPMM0     ENC28J60_BANK1(0x08)
#define ENC28J60_EPMM1     ENC28J60_BANK1(0x09)
#define ENC28J60_EPMM2     ENC28J60_BANK1(0x0A)
#define ENC28J60_EPMM3     ENC28J60_BANK1(0x0B)
#define ENC28J60_EPMM4     ENC28J60_BANK1(0x0C)
#define ENC28J60_EPMM5     ENC28J60_BANK1(0x0D)
#define ENC28J60_EPMM6     ENC28J60_BANK1(0x0E)
#define ENC28J60_EPMM7     ENC28J60_BANK1(0x0F)
#define ENC28J60_EPMCSL    ENC28J60_BANK1(0x10)
#define ENC28J60_EPMCSH    ENC28J60_BANK1(0x11)
#define ENC28J60_EPMOL     ENC28J60_BANK1(0x14)
#define ENC28J60_EPMOH     ENC28J60_BANK1(0x15)
#define ENC28J60_ERXFCON   ENC28J60_BANK1(0x18)
#define ENC28J60_EPKTCNT   ENC28J60_BANK1(0x19)

/* Bank 2 registers */
#define ENC28J60_MACON1    ENC28J60_BANK2(0x00)
#define ENC28J60_MACON2    ENC28J60_BANK2(0x01)
#define ENC28J60_MACON3    ENC28J60_BANK2(0x02)
#define ENC28J60_MACON4    ENC28J60_BANK2(0x03)
#define ENC28J60_MABBIPG   ENC28J60_BANK2(0x04)
#define ENC28J60_MAIPGL    ENC28J60_BANK2(0x06)
#define ENC28J60_MAIPGH    ENC28J60_BANK2(0x07)
#define ENC28J60_MACLCON1  ENC28J60_BANK2(0x08)
#define ENC28J60_MACLCON2  ENC28J60_BANK2(0x09)
#define ENC28J60_MAMXFLL   ENC28J60_BANK2(0x0A)
#define ENC28J60_MAMXFLH   ENC28J60_BANK2(0x0B)
#define ENC28J60_MAPLL     ENC28J60_BANK2(0x0C)
#define ENC28J60_MAPLPH    ENC28J60_BANK2(0x0D)
#define ENC28J60_MICMD     ENC28J60_BANK2(0x12)
#define ENC28J60_MIREGADR  ENC28J60_BANK2(0x14)
#define ENC28J60_MIWRL     ENC28J60_BANK2(0x16)
#define ENC28J60_MIWRH     ENC28J60_BANK2(0x17)
#define ENC28J60_MIRDL     ENC28J60_BANK2(0x18)
#define ENC28J60_MIRDH     ENC28J60_BANK2(0x19)
/* MAC address registers — Bank 3, regs 0x00-0x05 (DS39662C Table 3-1).
 * MAADR1 = MSB (byte 5 of MAC), MAADR6 = LSB (byte 0 of MAC).
 * Verified against Linux drivers/net/ethernet/microchip/enc28j60_hw.h.
 * NOTE: earlier this was Bank 2 regs 0x00-0x05, which aliased MACON1..MABBIPG
 * and corrupted the MAC config while never setting the real MAC address. */
#define ENC28J60_MAADR1    ENC28J60_BANK3(0x00)  /* MAC byte 5 (MSB) */
#define ENC28J60_MAADR2    ENC28J60_BANK3(0x01)  /* MAC byte 4 */
#define ENC28J60_MAADR3    ENC28J60_BANK3(0x02)  /* MAC byte 3 */
#define ENC28J60_MAADR4    ENC28J60_BANK3(0x03)  /* MAC byte 2 */
#define ENC28J60_MAADR5    ENC28J60_BANK3(0x04)  /* MAC byte 1 */
#define ENC28J60_MAADR6    ENC28J60_BANK3(0x05)  /* MAC byte 0 (LSB) */

/* Bank 3 registers */
#define ENC28J60_EBSTSD    ENC28J60_BANK3(0x06)
#define ENC28J60_EBSTCON   ENC28J60_BANK3(0x07)
#define ENC28J60_EBSTCSL   ENC28J60_BANK3(0x08)
#define ENC28J60_EBSTCSH   ENC28J60_BANK3(0x09)
#define ENC28J60_MISTAT    ENC28J60_BANK3(0x0A)
#define ENC28J60_EREVID    ENC28J60_BANK3(0x12)
#define ENC28J60_ECOCON    ENC28J60_BANK3(0x15)
#define ENC28J60_EFLOCON   ENC28J60_BANK3(0x17)
#define ENC28J60_EPAUSL    ENC28J60_BANK3(0x18)
#define ENC28J60_EPAUSH    ENC28J60_BANK3(0x19)

/* Common (bankless) registers */
#define ENC28J60_EIE       0x1B
#define ENC28J60_EIR       0x1C
#define ENC28J60_ESTAT     0x1D
#define ENC28J60_ECON2     0x1E
#define ENC28J60_ECON1     0x1F

/* EIE bits */
#define ENC28J60_EIE_INTIE    (1 << 7)
#define ENC28J60_EIE_PKTIE    (1 << 6)
#define ENC28J60_EIE_LINKIE   (1 << 4)
#define ENC28J60_EIE_TXIE     (1 << 3)

/* EIR bits */
#define ENC28J60_EIR_PKTIF    (1 << 6)
#define ENC28J60_EIR_LINKIF   (1 << 4)
#define ENC28J60_EIR_TXIF     (1 << 3)

/* ESTAT bits — NOTE: ESTAT has NO PHYLNK bit (bit 4 is LATECOL).
 * Link status is read from PHSTAT2.LSTAT via MII (see PHSTAT2 defines). */
#define ENC28J60_ESTAT_CLKRDY  (1 << 0)
#define ENC28J60_ESTAT_TXABRT  (1 << 1)
#define ENC28J60_ESTAT_RXBUSY  (1 << 2)
#define ENC28J60_ESTAT_LATECOL (1 << 4)

/* ECON1 bits */
#define ENC28J60_ECON1_BSEL0  (1 << 0)
#define ENC28J60_ECON1_BSEL1  (1 << 1)
#define ENC28J60_ECON1_RXEN   (1 << 2)
#define ENC28J60_ECON1_TXRTS  (1 << 3)
#define ENC28J60_ECON1_DMAST  (1 << 7)

/* ECON2 bits — DS39662C: AUTOINC=bit7 (0x80), PKTDEC=bit6 (0x40), PWRSV=bit5 (0x20).
 * IMPORTANT: AUTOINC was previously (1<<5)=0x20, which is actually PWRSV (power-save).
 * Setting it put the chip to sleep -> CLKRDY cleared, PHY off, no RX (ESTAT=0x00, PKT=0). */
#define ENC28J60_ECON2_AUTOINC (1 << 7)  /* Auto-Increment buffer pointers (0x80) */
#define ENC28J60_ECON2_PKTDEC  (1 << 6)
#define ENC28J60_ECON2_PWRSV   (1 << 5)

/* ERXFCON bits — строго по даташиту DS39662C */
#define ENC28J60_ERXFCON_UCEN  (1 << 7)
#define ENC28J60_ERXFCON_ANDOR (1 << 6)
#define ENC28J60_ERXFCON_CRCEN (1 << 5)
#define ENC28J60_ERXFCON_PMEN  (1 << 4)
#define ENC28J60_ERXFCON_MPEN  (1 << 3)
#define ENC28J60_ERXFCON_HTEN  (1 << 2)
#define ENC28J60_ERXFCON_MCEN  (1 << 1)
#define ENC28J60_ERXFCON_BCEN  (1 << 0)

/* MACON4 bits */
#define ENC28J60_MACON4_DEFER   (1 << 6)
#define ENC28J60_MACON4_BPEN    (1 << 5)
#define ENC28J60_MACON4_NOBKOFF (1 << 4)

/* MACON1 bits — DS39662C: bit0=MARXEN, bit1=PASSALL */
#define ENC28J60_MACON1_MARXEN  (1 << 0)
#define ENC28J60_MACON1_PASSALL (1 << 1)

/* MACON3 bits — строго по даташиту DS39662C */
#define ENC28J60_MACON3_PADCFG0  (1 << 5)
#define ENC28J60_MACON3_PADCFG1  (1 << 6)
#define ENC28J60_MACON3_PADCFG2  (1 << 7)
#define ENC28J60_MACON3_TXCRCEN  (1 << 4)
#define ENC28J60_MACON3_FRMLNEN  (1 << 1)
#define ENC28J60_MACON3_FULDPX   (1 << 0)

/* MICMD bits */
#define ENC28J60_MICMD_MIIRD     (1 << 0)

/* MISTAT bits */
#define ENC28J60_MISTAT_BUSY     (1 << 0)

/* PHY register addresses (accessed via MII interface) */
#define ENC28J60_PHCON1         0x00  /* PHY Control Register 1 */
#define ENC28J60_PHCON2         0x10  /* PHY Control Register 2 */
#define ENC28J60_PHSTAT2        0x11  /* PHY Status Register 2 */

/* PHCON1 bits */
#define ENC28J60_PHCON1_PDPXMD  (1 << 8)  /* Full-Duplex mode */
#define ENC28J60_PHCON1_RENEG   (1 << 9)  /* Restart auto-negotiation */

/* PHCON2 bits */
#define ENC28J60_PHCON2_FRCLNK  (1 << 8)  /* Force Link Up */
#define ENC28J60_PHCON2_TXDIS   (1 << 9)  /* Disable Twisted-Pair Transmitter */

/* PHSTAT2 bits — DS39662C / Linux enc28j60_hw.h: LSTAT=bit10 (0x400), DPXSTAT=bit9 (0x200) */
#define ENC28J60_PHSTAT2_LSTAT   (1 << 10) /* Link Status (read-only) */
#define ENC28J60_PHSTAT2_DPXSTAT (1 << 9)  /* Duplex Status (read-only) */

/* Buffer sizes */
#define ENC28J60_RX_BUF_START   0x0000
#define ENC28J60_RX_BUF_END     0x0FFF  /* 4KB receive buffer */
#define ENC28J60_TX_BUF_START   0x1000
#define ENC28J60_TX_BUF_END     0x1FFF  /* 4KB transmit buffer */
#define ENC28J60_TX_BUF_SIZE    0x1000
#define ENC28J60_BUF_SIZE       0x2000  /* 8KB total */
#define ENC28J60_MAX_PACKET     1518
#define ENC28J60_RSV_SIZE       6       /* next-packet-ptr(2) + RSV(4) */

/* ─── SPI Interface ────────────────────────────────── */

/**
 * @brief Initialize SPI bus and CS pin for ENC28J60.
 * Must be called once before any register access.
 */
esp_err_t enc28j60_spi_init(int host, int mosi, int miso, int sclk, int cs);

/**
 * @brief Read a control register.
 */
uint8_t enc28j60_rcr(uint16_t addr);

/**
 * @brief Write a control register.
 */
void enc28j60_wcr(uint16_t addr, uint8_t val);

/**
 * @brief Bit field set.
 */
void enc28j60_bfs(uint16_t addr, uint8_t mask);

/**
 * @brief Bit field clear.
 */
void enc28j60_bfc(uint16_t addr, uint8_t mask);

/**
 * @brief Read buffer memory.
 */
void enc28j60_rbm(uint8_t* buf, size_t len);

/**
 * @brief Write buffer memory.
 */
void enc28j60_wbm(const uint8_t* buf, size_t len);

/**
 * @brief Select register bank.
 */
void enc28j60_set_bank(uint8_t bank);

/**
 * @brief Read PHY register via MII.
 */
uint16_t enc28j60_phy_read(uint8_t addr);

/**
 * @brief Write PHY register via MII.
 */
void enc28j60_phy_write(uint8_t addr, uint16_t val);

/**
 * @brief Software reset ENC28J60.
 */
void enc28j60_reset(void);

/**
 * @brief Configure receive filter to accept ALL frames (promiscuous).
 * Required for ENC28J60 (clone) parts that reject unicast frames even to
 * the programmed MAC address.
 */
void enc28j60_set_rx_filter_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ENC28J60_H */
