# ESP-Prog Debug & Flashing Guide

## Overview

ESP-Prog is Espressif's official JTAG debug/programming tool for ESP32. It provides:

- **Flashing** firmware via JTAG (faster than UART)
- **Debugging** with GDB/OpenOCD
- **Reset control** without manual button pressing

---

## Wiring

### Pin Connections

| ESP-Prog | ESP32-WROOM-32 | Notes |
|----------|----------------|-------|
| TDI (pin 5) | GPIO12 (TDI) | JTAG data in |
| TDO (pin 13) | GPIO15 (TDO) | JTAG data out |
| TCK (pin 9) | GPIO13 (TCK) | JTAG clock |
| TMS (pin 7) | GPIO14 (TMS) | JTAG mode select |
| GND | GND | Common ground |
| VCC (3.3V) | 3.3V | Power (if not powered separately) |
| GPIO0 | GPIO0 | Flash control (GND for flash mode) |
| EN | EN | Reset control |

> **Note:** ESP32-WROOM-32 must be powered either from ESP-Prog's 3.3V or from its own power source (USB). Do not connect both simultaneously unless they share the same power rail.

### Wiring Diagram

```
ESP-Prog                  ESP32
┌─────────┐             ┌─────────┐
│ TDI     │────────────▶│ GPIO12  │
│ TDO     │◄────────────│ GPIO15  │
│ TCK     │────────────▶│ GPIO13  │
│ TMS     │────────────▶│ GPIO14  │
│ GND     │────────────▶│ GND     │
│ 3.3V    │────────────▶│ 3.3V    │
│ GPIO0   │────────────▶│ GPIO0   │
│ EN      │────────────▶│ EN      │
└─────────┘             └─────────┘
```

---

## Driver Installation

### Windows

1. Download FTDI VCP drivers: https://ftdichip.com/drivers/vcp-drivers/
2. Install the driver for the dual-channel FT2232HL chip
3. After connecting ESP-Prog, check device manager for:
   - `USB Serial Port` (COMx) — UART interface
   - `USB JTAG` interface — JTAG interface
4. OpenOCD is bundled with PlatformIO — no additional installation needed

---

## PlatformIO Configuration

The project already has the `[env:esp32dev-debug]` environment configured:

```ini
[env:esp32dev-debug]
platform = espressif32 @ >=6.0.0
board = esp32dev
framework = espidf
board_build.flash_mode = dio
board_build.f_cpu = 240000000L
board_build.f_flash = 40000000L
board_build.partitions = partitions/dhcp_partitions.csv

debug_tool = esp-prog
upload_protocol = esp-prog
debug_init_break = tbreak app_main
debug_speed = 20000
```

---

## Commands

### Build & Flash via ESP-Prog

```bash
# Build only
python -m platformio run -e esp32dev-debug

# Build + Flash firmware via JTAG
python -m platformio run -e esp32dev-debug --target upload

# Build + Flash firmware (отдельно от uploadfs!)
python -m platformio run -e esp32dev-debug --target upload

# Upload SPIFFS web files (отдельно!)
python -m platformio run -e esp32dev-debug --target uploadfs
```

### Debug via ESP-Prog

**CLI:**
```bash
python -m platformio debug -e esp32dev-debug
```

**VS Code (launch.json provided):**
1. Open Run & Debug panel (`Ctrl+Shift+D`)
2. Select configuration: **ESP-Prog Debug**
3. Press `F5`
4. Debugger halts at `app_main()` (breakpoint set via `debug_init_break`)

The VS Code configuration (`launch.json`) uses:
- `cortex-debug` extension
- OpenOCD with `board/esp32-wrover.cfg`
- FreeRTOS task awareness
- SVD file for peripheral register view

---

## Troubleshooting

### OpenOCD cannot find ESP-Prog

- Check FTDI drivers are installed
- Verify USB connection
- Check device manager for the JTAG interface
- Try different USB cable (some cables are power-only)

### "Error: libusb_open failed"

- On Windows: run OpenOCD as Administrator once
- Or install the WinUSB driver for the JTAG interface using Zadig

### Flashing fails mid-way

- Reduce `debug_speed` (try 5000 instead of 20000)
- Check wiring — loose connections cause intermittent failures
- Ensure stable power supply

### Cannot connect — "no device found"

- Verify all JTAG pins are connected correctly
- GPIO0 must be LOW (GND) during start for flash mode
- Press EN button on ESP-Prog to reset the ESP32
