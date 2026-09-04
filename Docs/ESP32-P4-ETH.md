# ESP32-P4-ETH — Build, Flash & Web-UI Guide

Flashing and update guide for the **Waveshare ESP32-P4-ETH** build of
DHCPServer. This target is built with the **native ESP-IDF** toolchain
(≥ v6.0) — PlatformIO's `espressif32` platform has no ESP32-P4 support, so
there is no `upload`/`uploadfs` here. The classic ESP32 + ENC28J60 workflow
(PlatformIO) is documented in the [README](../README.md) and
[PartitionTable.md](PartitionTable.md).

---

## Overview

| Item | Value |
|------|-------|
| Board | Waveshare ESP32-P4-ETH |
| Toolchain | native ESP-IDF ≥ v6.0 (`idf.py`) |
| Flash | 32 MB (GigaDevice GD25Q256), internal SPI |
| Console / flashing port | onboard USB-C → CH343P (UART0) |
| Firmware source | branch `ESP32-P4-ETH` |
| Partition table | [`partitions/dhcp_partitions_p4.csv`](../partitions/dhcp_partitions_p4.csv) |
| Target defaults | [`sdkconfig.defaults.esp32p4`](../sdkconfig.defaults.esp32p4) |

The web interface (`data/`) is **not** compiled into the firmware — it lives in
the `spiffs` flash partition and is uploaded separately (a native-ESP-IDF
counterpart of PlatformIO's `uploadfs`). See [Partition Table](#partition-table)
below.

---

## Prerequisites

1. ESP-IDF v6.0+ installed with the RISC-V toolchain
   (`riscv32-esp-elf`). The setup used during development lives in
   `C:\esp\v6.0.1\esp-idf` with the Python venv at
   `C:\Espressif\tools\python\v6.0.1\venv`.
2. A terminal with the ESP-IDF environment exported (`export.ps1` on Windows /
   `export.sh` on Linux/macOS), **or** an "ESP-IDF PowerShell" shortcut from the
   ESP-IDF installer.
3. The board connected over USB-C (CH343P exposes the UART0 console and the ROM
   bootloader).

> ⚠️ **Stale `IDF_TARGET`.** If you previously built the ESP32 target in the
> same shell, the environment variable `IDF_TARGET=esp32` may still be set and
> `idf.py` will try to flash an ESP32-P4 as an ESP32:
> `esptool: This chip is ESP32-P4, not ESP32. Wrong chip argument?`
>
> Fix: unset it and re-export, or open a fresh ESP-IDF terminal:
>
> ```powershell
> Remove-Item Env:IDF_TARGET
> . C:\esp\v6.0.1\esp-idf\export.ps1
> ```

---

## First-time setup (once per clean checkout)

```powershell
# Select the ESP32-P4 target (applies sdkconfig.defaults.esp32p4)
idf.py set-target esp32p4

# Build
idf.py build
```

> The Waveshare ESP32-P4-ETH ships with **silicon rev v1.3**, which belongs to
> the `< 3.0` family. `sdkconfig.defaults.esp32p4` already opts out of the
> `≥ 3.0`-only default (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` +
> `CONFIG_ESP32P4_REV_MIN_100=y`, CPU capped at 360 MHz). Without these, the
> bootloader refuses to run on rev v1.3 silicon.

---

## Flashing the firmware

```powershell
idf.py -p COMx flash        # build + flash (bootloader, partition table, app)
idf.py -p COMx monitor      # open the UART console
```

- Replace `COMx` with the board's port (e.g. `COM3`).
- The app is flashed into the **OTA app slot 0** (`ota_0`).
- After flashing you should see in the log:
  `SPIFFS mounted at /spiffs` and `Web server started`.

---

## Uploading the web interface (SPIFFS)

Because this is a native ESP-IDF build, the `data/` web content must be
uploaded to the `spiffs` partition manually. Use the helper script
[`scripts/upload_web_p4.ps1`](../scripts/upload_web_p4.ps1):

```powershell
# Upload web UI to the board on COM3 (default port)
.\scripts\upload_web_p4.ps1

# Different port / custom baud
.\scripts\upload_web_p4.ps1 -Port COM5 -Baud 460800

# Build the SPIFFS image only, without flashing
.\scripts\upload_web_p4.ps1 -BuildOnly
```

What the script does:

1. Locates the ESP-IDF Python venv and `spiffsgen.py`
   (`IDF_PYTHON_ENV_PATH` → `C:\Espressif\tools\python\v6.0.1\venv` → `PATH`;
   `IDF_PATH` → `C:\esp\v6.0.1\esp-idf`).
2. Reads the `spiffs` partition offset/size from
   [`partitions/dhcp_partitions_p4.csv`](../partitions/dhcp_partitions_p4.csv).
3. Builds a full-size SPIFFS image from `data/` with the **same geometry** as
   the firmware build (page 256, block 4096, obj-name-len 32, meta-len 4,
   magic on) so no auto-format happens on mount.
4. Flashes it with `esptool --chip esp32p4` at the `spiffs` partition offset.

> **Order matters:** flash the firmware first, then the web UI. The script's
> geometry must match `CONFIG_SPIFFS_*` in the sdkconfig the firmware was built
> with — it does by default.

---

## Accessing the device

| Service | Address |
|---------|---------|
| Web UI | `http://192.168.1.201` |
| Default login | `admin` / `admin` |
| UART console | `idf.py -p COMx monitor` (baud 115200) |

---

## OTA updates

The web UI's **Version → firmware update** page can update the firmware
over-the-air. The dual-OTA layout (`ota_0`/`ota_1`, 5 MB each) lets the device
fall back to the previous image if the new one fails to boot. The OTA flow only
updates the **app** — the web interface (SPIFFS) and settings (NVS) are kept.

---

## Partition table

Layout for the 32 MB flash (see [PartitionTable.md](PartitionTable.md)):

| Partition | Offset | Size | Usage |
|-----------|--------|------|-------|
| nvs | 0x9000 | 24 KB | Configuration storage |
| otadata | 0x10000 | 8 KB | OTA boot selection |
| ota_0 | 0x20000 | 5 MB | OTA app slot 0 |
| ota_1 | 0x520000 | 5 MB | OTA app slot 1 |
| spiffs | 0xA20000 | 8 MB | Web interface files |

> The `spiffs` partition was sized **8 MB** deliberately: ESP-IDF's SPIFFS
> driver numbers pages with a 16-bit counter (`spiffs_page_ix`, max 65 535).
> At the default 256-byte page the largest mountable partition is 16 MB, and the
> original 22 MB layout exceeded it — SPIFFS then failed to mount with
> `spiffs partition is too large for spiffs_page_ix type` and the web UI
> returned "Not Found". The web interface is ~127 KB, so 8 MB leaves ample room.

---

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `esptool: This chip is ESP32-P4, not ESP32` | Stale `IDF_TARGET=esp32` in the shell — unset it / use a fresh ESP-IDF terminal (see [above](#prerequisites)). |
| Bootloader refuses to boot on rev v1.3 | `sdkconfig.defaults.esp32p4` is missing the `< 3.0` rev options — see [First-time setup](#first-time-setup-once-per-clean-checkout). |
| Web UI shows plain "Not Found" | SPIFFS not mounted (check boot log for `SPIFFS mount failed` / `too large for spiffs_page_ix`) or web UI not uploaded yet — run [`scripts/upload_web_p4.ps1`](../scripts/upload_web_p4.ps1). |
| `SPIFFS mount failed (ESP_ERR_INVALID_ARG)` + `spiffs partition is too large for spiffs_page_ix type` | The `spiffs` partition exceeds the page-count limit. Shrink the partition to ≤ 16 MB (keep page 256) or raise `CONFIG_SPIFFS_PAGE_SIZE` to 1024 (see [Partition table](#partition-table)). |
| `File not found: /spiffs/...` in the web server log | The file really is missing from SPIFFS — re-run the web-UI upload script. |
| Web UI upload geometry mismatch wipes data | Image geometry must match the firmware's `CONFIG_SPIFFS_*` (the script uses the defaults above). If it doesn't, SPIFFS auto-formats on mount. |
