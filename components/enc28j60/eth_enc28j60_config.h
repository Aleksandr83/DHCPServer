/*
 * ENC28J60 configuration structures.
 * Replaces the missing esp_eth_enc28j60.h from ESP-IDF v5+.
 */

#ifndef ETH_ENC28J60_CONFIG_H
#define ETH_ENC28J60_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default ENC28J60 configuration.
 * This macro provides default configuration for the ENC28J60 driver.
 */
#define ETH_ENC28J60_DEFAULT_CONFIG(cs_gpio) \
    { .cs_gpio_num = (cs_gpio), .int_gpio_num = -1 }

/**
 * @brief ENC28J60 specific configuration.
 */
typedef struct {
    /**
     * @brief GPIO number for CS (Chip Select) line.
     */
    int cs_gpio_num;

    /**
     * @brief GPIO number for INT (Interrupt) line.
     * Set to -1 if not used (polling mode).
     */
    int int_gpio_num;
} eth_enc28j60_config_t;

#ifdef __cplusplus
}
#endif

#endif /* ETH_ENC28J60_CONFIG_H */
