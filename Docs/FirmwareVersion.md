# Firmware Versioning

## Version Format

```
aa.bb.xxx.cc.YY.MM.RR
```

| Segment | Name | Range | Current | Description |
|---------|------|-------|---------|-------------|
| `aa` | Global version | 00–99 | `01` | Global firmware version |
| `bb` | Device/Product code | 00–99 | `02`/`03` | `02` = ESP32 + ENC28J60 (legacy), `03` = ESP32-P4-ETH |
| `xxx` | Release number | 000–999 | `041` | Major release |
| `cc` | Sub-release | 00–99 | `00` | Incremented on each reflash |
| `YY` | Year | 00–99 | `26` | Last 2 digits of year (2026) |
| `MM` | Month | 01–12 | `09` | Month |
| `RR` | Region | 2 chars | `RU` | Region code |

**Example (ESP32-P4-ETH):** `01.03.041.00.26.09.RU`

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
CONFIG_FW_VER_RELEASE=41
CONFIG_FW_VER_SUBRELEASE=0
CONFIG_FW_VER_YEAR=26
CONFIG_FW_VER_MONTH=9
CONFIG_FW_VER_REGION="RU"
CONFIG_FW_MIN_DATETIME="2026-09-11 22:00:00"
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
board_build.menuconfig.FW_VER_RELEASE = 41
board_build.menuconfig.FW_VER_SUBRELEASE = 0
board_build.menuconfig.FW_VER_YEAR = 26
board_build.menuconfig.FW_VER_MONTH = 9
board_build.menuconfig.FW_VER_REGION = "RU"
```

### 4. `src/core/Version.cpp` (fallback defines, if config not found)
```cpp
#ifndef CONFIG_FW_VER_RELEASE
#define CONFIG_FW_VER_RELEASE 41
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

# Only refresh the build date (CONFIG_FW_MIN_DATETIME), no version change
python scripts/inc_firmware_ver.py --min-only
```

**What the script does:**

1. Reads current version from `sdkconfig.defaults`
2. Increments the chosen field by 1
3. Updates `sdkconfig.defaults` — `CONFIG_FW_VER_SUBRELEASE` or `CONFIG_FW_VER_RELEASE`
4. Updates `src/core/Version.cpp` — fallback `#define`
5. Updates `platformio.ini` — both `[env:esp32dev]` and `[env:esp32dev-debug]`
6. Propagates the new value to the **live root `sdkconfig`** and any generated
   `sdkconfig.*` files that already contain the key (the ESP32-P4 `idf.py`
   build reads the live root `sdkconfig`, so it must not stay stale). Files are
   **updated, not deleted** — several are tracked in git.
7. Sets `CONFIG_FW_MIN_DATETIME` (the lowest date/time the Time Server page
   accepts for a manual clock setting, see [Minimum date/time](#minimum-datetime))
   to the current date **and time** — the build moment — in `sdkconfig.defaults`,
   every other `sdkconfig*` file (appended if a build has not created the key
   yet), the `Version.cpp` fallback and `platformio.ini`
8. Prints the new version — ready for rebuild

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

# Day of the minimum date (default 1) — for a minimum-only change
python scripts/set_firmware_date.py --day 15

# Hour of the minimum time (0-23, default 0)
python scripts/set_firmware_date.py --hour 12
```

**What the script does:**

1. Validates the values (year → last 2 digits, month → 01–12, day → 01–31, hour → 00–23)
2. Updates `sdkconfig.defaults` — `CONFIG_FW_VER_YEAR` / `CONFIG_FW_VER_MONTH`
3. Updates `src/core/Version.cpp` — fallback `#define`
4. Updates `platformio.ini` — both `[env:esp32dev]` and `[env:esp32dev-debug]`
5. Propagates the new value to the **live root `sdkconfig`** and any generated
   `sdkconfig.*` files that already contain the key (updated, not deleted)
6. Sets `CONFIG_FW_MIN_DATETIME` to `"20YY-MM-DD HH:00:00"` (the effective
   year/month plus the `--day` value, default 01, and the `--hour` value,
   default 00) — the release **date**, hour granularity
7. Prints the applied changes — ready for rebuild

**After running the script:** rebuild and flash as described for the auto-increment script above.

---

## Minimum date/time

`CONFIG_FW_MIN_DATETIME` (Kconfig string, `src/Kconfig.projbuild` → menu
**Time server**) is the lowest date/time the Time Server → **General** page
accepts for a manual clock setting — so the clock can never be set to a
meaningless value such as 1970. It is a build constant in the
`"YYYY-MM-DD HH:00:00"` form: **hour granularity**, the minutes and seconds are
always zero. It is reported read-only as `min_datetime` by
`GET /api/time/settings`; the **web page** enforces it (the firmware itself
does not reject an earlier value).

Both version scripts keep it current — `inc_firmware_ver.py` at the build
moment (current hour, minutes/seconds zeroed), `set_firmware_date.py` at the
release date (`--day`/`--hour`, default 01/00) — and propagate it to
`sdkconfig.defaults`, every other `sdkconfig*` file, the `Version.cpp` fallback
and `platformio.ini`.
The shared helpers live in `scripts/kconfig_tools.py`.

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
