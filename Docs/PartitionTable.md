# Partition Table — DHCPServer (ESP32-WROOM-32)

## Overview

The device uses a custom partition table to accommodate dual OTA updates and a SPIFFS partition for web content.

**Flash size:** 4 MB (0x400000)

## Layout

| # | Name        | Type    | SubType    | Offset    | Size       | Description                |
|---|-------------|---------|------------|-----------|------------|----------------------------|
| 0 | phy_init    | data    | phy        | 0x00F000  | 0x001000   | RF calibration data        |
| 1 | otadata     | data    | ota        | 0x010000  | 0x002000   | OTA boot selection         |
| 2 | nvs         | data    | nvs        | 0x012000  | 0x006000   | NVS (WiFi, config, etc.)   |
| 3 | ota_0       | app     | ota_0      | 0x020000  | 0x180000   | OTA app slot 0 (1.5 MB)    |
| 4 | ota_1       | app     | ota_1      | 0x1A0000  | 0x180000   | OTA app slot 1 (1.5 MB)    |
| 5 | spiffs      | data    | spiffs     | 0x320000  | 0x0E0000   | Web interface files (896 KB) |

**Total used:** 0x400000 (4 MB)

## Partition Details

### phy_init (0x00F000, 4 KB)
- Stores RF calibration data.
- Written once during initial manufacturing calibration.
- Never modified by application.

### otadata (0x010000, 8 KB)
- Stores the current OTA boot partition selection.
- Written by `esp_ota_set_boot_partition()` during OTA updates.
- Bootloader reads this to determine which app slot to boot.

### nvs (0x012000, 24 KB)
- Non-volatile storage for application configuration.
- Stores:
  - WiFi SSID/password
  - DHCP server settings (IP range, lease time)
  - Static MAC→IP bindings (max 512 bytes)
  - DNS server settings
  - Security/authentication config
  - DNS cache data (max 512 bytes)
- Accessed via the `nvs_flash` API through `Config` class.

### ota_0 (0x020000, 1.5 MB)
- First OTA application slot.
- Used as primary boot partition after initial flash.

### ota_1 (0x1A0000, 1.5 MB)
- Second OTA application slot.
- Used as fallback during OTA updates:
  1. New firmware is written to the inactive slot.
  2. On success, the boot partition is switched.
  3. Device reboots into the new firmware.
  4. If the new firmware fails, the bootloader falls back to the previous slot.

### spiffs (0x320000, 896 KB)
- SPIFFS (SPI Flash File System) partition.
- Stores all web interface files:
  - `index.html` - Main page
  - `header.html` - Shared header
  - `footer.html` - Shared footer
  - `css/style.css` - Dark theme styles
  - `js/app.js` - SPA logic
  - `i18n/ru.json` - Russian translations
  - `i18n/en.json` - English translations
  - `pages/dhcp_setup.html` - DHCP configuration
  - `pages/dns_setup.html` - DNS configuration
  - `pages/security.html` - Security settings
  - `pages/version.html` - Version info
  - `pages/firmware_update.html` - OTA update page
- Mounted at `/spiffs` by the application.
- Automatically formatted if mount fails.

## Config file

Partition table CSV: `partitions/dhcp_partitions.csv`

```csv
nvs,          data, nvs,      0x9000,   0x6000,
phy_init,     data, phy,      0xF000,   0x1000,
otadata,      data, ota,      0x10000,  0x2000,
ota_0,        app,  ota_0,    0x20000,  0x180000,
ota_1,        app,  ota_1,    0x1A0000, 0x180000,
spiffs,       data, spiffs,   0x320000, 0xE0000,
```

> **Note:** The first line (nvs) shows offset 0x9000 instead of 0x12000 in the table above because the bootloader and partition table occupy the first 0x8000 + 0xC00 bytes of flash. The actual NVS region starts at 0x12000.
