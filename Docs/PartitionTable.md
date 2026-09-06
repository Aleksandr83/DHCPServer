# Partition Table — DHCPServer

## Overview

The device uses custom partition tables to accommodate dual OTA updates and a
SPIFFS partition for web content. Two tables ship with the project:

| Target | File | Flash size |
|--------|------|-----------|
| ESP32 + ENC28J60 | `partitions/dhcp_partitions.csv` | 4 MB |
| ESP32-P4-ETH | `partitions/dhcp_partitions_p4.csv` | 32 MB |

## ESP32-P4-ETH — 32 MB layout

Applies to the Waveshare ESP32-P4-ETH (GigaDevice 25Q256EY1G, 32 MB).
The `phy_init`/RF-calibration partition does not exist on the ESP32-P4 — the
chip has no 2.4 GHz radio — so the released space goes to much larger OTA
slots, a 1 MB SPIFFS and a ~21 MB FAT data partition.

### Layout

| # | Name    | Type    | SubType | Offset     | Size      | Description                   |
|---|---------|---------|---------|------------|-----------|-------------------------------|
| 0 | nvs     | data    | nvs     | 0x009000   | 0x006000  | NVS (config, etc.) 24 KB      |
| 1 | otadata | data    | ota     | 0x010000   | 0x002000  | OTA boot selection 8 KB       |
| 2 | ota_0   | app     | ota_0   | 0x020000   | 0x500000  | OTA app slot 0 (5 MB)         |
| 3 | ota_1   | app     | ota_1   | 0x520000   | 0x500000  | OTA app slot 1 (5 MB)         |
| 4 | spiffs  | data    | spiffs  | 0xA20000   | 0x100000  | Web interface files (1 MB)    |
| 5 | fat     | data    | fat     | 0xB20000   | 0x14E0000 | FAT data partition (~21 MB)   |

**Total used:** 0x2000000 (32 MB) — the full flash is now allocated. The FAT
row fills the ~21 MB left over after NVS/otadata/OTA/SPIFFS. The SPIFFS
partition was shrunk from 8 MB to 1 MB (the web UI is ~144 KB — ample room) so
more space goes to the read-write FAT data partition.

Selected for ESP32-P4 builds via `sdkconfig.defaults.esp32p4`
(`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/dhcp_partitions_p4.csv"`).

## Legacy ESP32-WROOM-32 — 4 MB layout

The device uses a custom partition table to accommodate dual OTA updates and a
SPIFFS partition for web content.

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

Legacy ESP32 table: `partitions/dhcp_partitions.csv`

```csv
nvs,          data, nvs,      0x9000,   0x6000,
phy_init,     data, phy,      0xF000,   0x1000,
otadata,      data, ota,      0x10000,  0x2000,
ota_0,        app,  ota_0,    0x20000,  0x180000,
ota_1,        app,  ota_1,    0x1A0000, 0x180000,
spiffs,       data, spiffs,   0x320000, 0xE0000,
```

ESP32-P4-ETH table: `partitions/dhcp_partitions_p4.csv`

```csv
nvs,          data, nvs,      0x9000,    0x6000,
otadata,      data, ota,      0x10000,   0x2000,
ota_0,        app,  ota_0,    0x20000,   0x500000,
ota_1,        app,  ota_1,    0x520000,  0x500000,
spiffs,       data, spiffs,   0xA20000,  0x100000,
fat,          data, fat,      0xB20000,  0x14E0000,
```

> **Note:** NVS lives at offset 0x9000; the bootloader and partition-table
> metadata occupy the first 0x9000 bytes of flash. In the ESP32 layout table
> above the NVS offset was misprinted as 0x012000 — 0x9000 is authoritative.
