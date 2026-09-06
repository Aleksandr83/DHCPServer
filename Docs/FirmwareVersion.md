# Firmware Versioning

## Version Format

```
aa.bb.xxx.cc.YY.MM.RR
```

| Segment | Name | Range | Current | Description |
|---------|------|-------|---------|-------------|
| `aa` | Global version | 00–99 | `01` | Global firmware version |
| `bb` | Device/Product code | 00–99 | `02`/`03` | `02` = ESP32 + ENC28J60 (legacy), `03` = ESP32-P4-ETH |
| `xxx` | Release number | 000–999 | `036` | Major release |
| `cc` | Sub-release | 00–99 | `00` | Incremented on each reflash |
| `YY` | Year | 00–99 | `26` | Last 2 digits of year (2026) |
| `MM` | Month | 01–12 | `09` | Month |
| `RR` | Region | 2 chars | `RU` | Region code |

**Example (ESP32-P4-ETH):** `01.03.036.00.26.09.RU`

> Device code: classic **ESP32 + ENC28J60** builds report `02`; the current
> **Waveshare ESP32-P4-ETH** target reports `03`. The P4 value is set in
> `sdkconfig.defaults.esp32p4` (`CONFIG_FW_VER_DEVICE=3`), which overrides the
> shared `sdkconfig.defaults` (`02`).

---

## Configuration Files

The version is defined in several places (release/month must match):

### 1. `sdkconfig.defaults` (shared ESP-IDF defaults — classic ESP32)
```
CONFIG_FW_VER_GLOBAL=1
CONFIG_FW_VER_DEVICE=2
CONFIG_FW_VER_RELEASE=36
CONFIG_FW_VER_SUBRELEASE=0
CONFIG_FW_VER_YEAR=26
CONFIG_FW_VER_MONTH=9
CONFIG_FW_VER_REGION="RU"
```

### 2. `sdkconfig.defaults.esp32p4` (ESP32-P4-ETH overrides)
Only the values that differ from the shared defaults are set here — currently
the **device code**:
```
CONFIG_FW_VER_DEVICE=3
```

### 3. `platformio.ini` (PlatformIO menuconfig overrides — classic ESP32)
Each build environment (`[env:esp32dev]`, `[env:esp32dev-debug]`) has:
```
board_build.menuconfig.FW_VER_GLOBAL = 1
board_build.menuconfig.FW_VER_DEVICE = 2
board_build.menuconfig.FW_VER_RELEASE = 36
board_build.menuconfig.FW_VER_SUBRELEASE = 0
board_build.menuconfig.FW_VER_YEAR = 26
board_build.menuconfig.FW_VER_MONTH = 9
board_build.menuconfig.FW_VER_REGION = "RU"
```

### 4. `src/core/Version.cpp` (fallback defines, if config not found)
```cpp
#ifndef CONFIG_FW_VER_RELEASE
#define CONFIG_FW_VER_RELEASE 36
#endif
```

### 5. `src/Kconfig.projbuild` (menuconfig UI defaults)
Used when running `idf.py menuconfig` or on first build.

---

## Auto-Increment Script

### `scripts/inc_firmware_ver.py`

Increments the firmware version number in all configuration files at once.

**Usage:**

```bash
# Increment sub-release (cc) — default, use for each reflash
python scripts/inc_firmware_ver.py

# Increment release number (xxx) — for major changes
python scripts/inc_firmware_ver.py --rel

# Increment sub-release explicitly
python scripts/inc_firmware_ver.py --sub
```

**What the script does:**

1. Reads current version from `sdkconfig.defaults`
2. Increments the chosen field by 1
3. Updates `sdkconfig.defaults` — `CONFIG_FW_VER_SUBRELEASE` or `CONFIG_FW_VER_RELEASE`
4. Updates `src/core/Version.cpp` — fallback `#define`
5. Updates `platformio.ini` — both `[env:esp32dev]` and `[env:esp32dev-debug]`
6. Deletes all cached `sdkconfig.*` files (esp32dev, esp32dev-debug, etc.)
7. Prints the new version — ready for rebuild

**After running the script:**
```bash
# 1. Clean и rebuild (обязательно для перекомпиляции Version.cpp)
pio run -e esp32dev-debug --target clean
python -m platformio run -e esp32dev-debug

# 2. Прошивка — команды выполнять РАЗДЕЛЬНО!
python -m platformio run -e esp32dev-debug --target upload    # firmware
python -m platformio run -e esp32dev-debug --target uploadfs  # SPIFFS
```

> **Важно:** `--target upload --target uploadfs` в одной команде прошивает SPIFFS дважды, пропуская firmware. Всегда выполняйте их отдельно.

---

## Set Date Script

### `scripts/set_firmware_date.py`

Sets the firmware **year (YY)** and **month (MM)** in all configuration files at once.

**Usage:**

```bash
# Both year and month (2-4 digits for year; last two are used)
python scripts/set_firmware_date.py --year 26 --month 7

# Positional form
python scripts/set_firmware_date.py 26 7

# Only one field (the other keeps its current value)
python scripts/set_firmware_date.py --year 26
python scripts/set_firmware_date.py --month 8
```

**What the script does:**

1. Validates the values (year → last 2 digits, month → 01–12)
2. Updates `sdkconfig.defaults` — `CONFIG_FW_VER_YEAR` / `CONFIG_FW_VER_MONTH`
3. Updates `src/core/Version.cpp` — fallback `#define`
4. Updates `platformio.ini` — both `[env:esp32dev]` and `[env:esp32dev-debug]`
5. Deletes all cached `sdkconfig.*` files (esp32dev, esp32dev-debug, etc.)
6. Prints the applied changes — ready for rebuild

**After running the script:** rebuild and flash as described for the auto-increment script above.

---

## REST API

```
GET /api/version
```

**Response:**
```json
{
  "firmware_version": "01.02.003.00.26.07.RU"
}
```
