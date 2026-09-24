# DHCPServer

**DHCPv4 + Caching DNS Proxy + NTP Server for Waveshare ESP32-P4-ETH**

---

![DHCPServer web interface](Docs/images/web_interface.png)

---

## 📖 Description

DHCP server and caching DNS proxy built on the **Waveshare ESP32-P4-ETH** (dual-core RISC-V **ESP32-P4**). The device connects to the local network **over the onboard 10/100 Ethernet** (internal EMAC + **IP101GRI** PHY) — **WiFi/Bluetooth are not available on the ESP32-P4**. It assigns IP addresses through DHCP, proxies DNS queries with caching and logging, serves time over NTP (and syncs its own clock from an upstream NTP server), is managed through a web interface (dark theme, RU/EN localization) that also offers a **file explorer** for the internal FAT data partition and a microSD card, or through a UART terminal menu.

---

## ✨ Features

- **DHCPv4 Server** — configurable IP range, subnet, gateway, lease time, static MAC→IP bindings (enable + per-host DNS override)
- **Allowed Computers (DHCP allow-list)** — "assign addresses only to allowed computers" (DHCP ▾ General): with the switch **off** the list is ignored completely and addresses are handed out as before; with it **on** a client is served only when its MAC is in the list (≤ 25 entries, per-entry Enable checkbox, 1024-byte NVS budget) or when it has a static binding whose own Enable checkbox is on — every other machine gets nothing at all, so it loses its address at the next renewal instead of being refused politely. The list is mirrored into a PSRAM MAC hash table (built at boot, rebuilt on every save, lock-free swap, listens nothing) and the policy **fails open** if that table cannot be built, so a memory failure never takes the LAN off the air. Part of the settings export/import, and the page says which state the list is in. The page itself lives under **Settings ▾ → Security → Allowed computers** (it moved there in stage 140, together with a proper name for the file: `security_allowed.html`)

Messages on that page (the list is full, a row has a name but no address, the device refused the save, the list was saved) are shown by the page's own dialog, not by the browser: a browser alert cannot be translated, styled or captured in a screenshot, and it blocks the whole page. A message has exactly one answer, so its dialog has a single button.

Every page of the interface works that way — a device refusal, a saved message or a question is drawn by the page itself, in the language the operator picked, and there is no browser `alert()` or `confirm()` left anywhere (stage 142). A refusal is shown with `common.error` as the title and the device's own words underneath. The same dialog also carries **input**: where an action needs more than a yes/no, it opens with a text field, a drop-down or both (`showFieldDialog()` in `data/js/app.js`), so a question is asked where the button is, not by a row of controls that the operator has to notice on the page before pressing it (stage 163 — the name and the validity of a new certificate).

The line under the table counts the entries and, when some of them are switched off, how many of them are enabled; it follows every edit that can change the number, the Enable checkboxes included.

On the page itself a row without a MAC is **not an entry**: its Enable checkbox is off (it goes off by itself if the address is erased), it is not counted, never saved, and a half-filled row (a name with no address) stops the save with a reason instead of making the device refuse the whole list
- **DNS Proxy** — pipeline: logging → local hosts → **internal (PSRAM) cache** → external cache (REST) → forwarding to external DNS
- **Block non-A/AAAA forwarding** — optional toggle on DNS Setup: queries of any type other than A/AAAA that are not answered from local hosts get an immediate NODATA reply and are never sent to the external cache/upstream (the client falls back to A/AAAA)
- **Internal DNS Cache** — on-device A/AAAA hash table in PSRAM (up to 20 MB, configurable; TTL-aware, ignore-TTL option), served before the external cache. Every record carries a **usage counter**: it grows by one for each answer served from the cache and for each answer stored, the **least used** record is evicted when the pool fills up, and the status reports the total and the most-used name (the cache file keeps the counters, format version 3; version 1 and 2 files are still read). The status line prints the average hit time next to the parts it is made of — waiting for the arena lock, the cache's own work, and the number of records a hit walked (stage 127; those totals survive a reboot with the statistics file)
- **Cache Persistence** — the built-in cache can be saved to/loaded from `cache.dat` on the FAT partition (background job with live progress; auto-restored on boot), and a planned restart no longer throws the working set away: with the "Save cache before reboot" switch on (**default**, next to "Save statistics before reboot" on the Internal Cache page) the device writes the whole table before restarting — from the Reboot button, a firmware update or the console's `reboot` — and loads it again at boot; every save also stores an **MD5 of the file** in NVS, and a file whose checksum does not match is refused (the boot restore logs it, the load button asks before overriding). A restart does not take the write on trust either (stage 169): the statistics file is read back and compared byte for byte, the cache file is parsed and its record count compared with the one the save reported, and a file that does not match is written **once more**. If it still does not match, the page says so with the device's own words and asks the operator — "cancel the reboot/update" or "go ahead anyway" — while a path without a page (a script's OTA upload, the terminal's `reboot`) writes the reason to `Errors.log` and carries on. The page also says **which of the two things the device is doing, and which file**: the write and the read-back are separate phases of the progress it polls (`checking`), the file is named by its path (`path`), and a finished read-back that matched is reported as such (`checked`, which is not the same as a successful save — the DNS page's own button writes without checking). So the status line reads "Saving the cache: /fat/cache.dat… 30 %", then "Checking the cache file: /fat/cache.dat… 70 % (700 / 1000)", then "The cache file was checked successfully.", then "Cache saved."; the statistics file gets the same four lines (`/fat/Statistica.dat`), with its "checking" line rarely visible because 92 bytes are read in microseconds — the step line names both halves while the job runs. Every one of those lines stays on screen for **at least two seconds** before the next one takes its place, including the last one of the sequence (the wait happens before a replacement, not after it), so none of them flashes by unread
- **Cache Reset and Autosave** — the Reset button of the Internal Cache page
  drops every cached answer held in PSRAM (the file on the card is left
  alone); the cache can also save itself every N minutes, N hours or N days,
  the interval being bounded by its unit (60 minutes, 24 hours, the length of
  the running month in days) and applied by the Save button next to it. The
  autosave is suspended while the clock is not set, and a manual save shifts
  the countdown by a whole period.
- **Time Server (NTP)** — the device syncs its clock from an external NTP server (SNTP) and serves UTC time to LAN clients over NTP (UDP 123; configurable external NTP server, re-sync interval, named timezone selection with a custom-offset fallback; clock sync and serving can be switched on/off independently; optional terminal/REST logging of served requests). Until the clock has been synchronised (or set by hand) the server answers with LI=3/stratum 16 (RFC 5905) instead of a wrong time. The date/time can also be **set manually** from the web interface — typed in the selected timezone or taken from the computer's clock, and values earlier than the firmware build time are refused (the board has no battery-backed RTC)
- **LAN-only hardening** — the built-in DNS and NTP servers answer only clients from the device's own subnet (**on by default**; a query from outside is dropped without any reply, so the device cannot be used as an open resolver or a reflection amplifier). The **file explorer applies the same rule** to `/api/files/*` (a foreign client gets `403`). NTP additionally rate-limits replies per client address (1..100/s, default 5), and the DHCP lease/offer table has a hard cap (`0` = auto = 2× the pool size, 8..512, configurable) so a DISCOVER flood with random MACs cannot grow it without limit
- **File Explorer** — browse, download, upload, rename, delete and format the **internal FAT data partition** (~21 MB, `/fat`) and a **microSD card** in the board's slot (`/sdcard`; no card is fitted by default, so that volume reports “not mounted”). A card is picked up **and noticed again** while the device runs: a missing one is retried (at most once every few seconds), and a mounted one is asked on the bus whether it is still there (no card-detect line exists on the board), so pulling it out flips the volume to “not mounted” within a poll instead of reporting yesterday's capacities until the next reboot. One “⋯” button on the right holds the file actions (**New file**, **New folder**, **Select all**, **Upload file**, **Upload folder**, **Move ...**, **Delete** for the ticked rows, **Refresh**, **Format card**, **Check for errors**) and a **read-only volume check** walks the whole volume, reading every file to the end — a broken cluster chain is only visible that way — then offers the two actions that can make a card usable again (delete the unreadable entries, or format it). Formatting also works on a card that cannot be mounted (an interrupted format leaves one without a filesystem, and the format is the only thing that can give it back), and a card that stopped answering mid-format can be brought back by stopping the operation: its supply is cut for five seconds, which breaks the stuck driver call. Rows have their own menu (download/open/rename/delete). Uploads are atomic (`<name>.part` + rename) and **resumable**: the progress row carries pause and cancel buttons, and a paused (or interrupted) transfer continues from the byte the device reached instead of starting over — the temporary `*.part` names stay hidden from the explorer and cannot be created through the API. Text files can be edited in the browser, and the home page shows both volumes’ used/free space next to the RAM bars
- **Onboard Ethernet 10/100** — internal EMAC + IP101GRI PHY over RMII (no WiFi — ESP32-P4 has no radio)
- **Task Scheduler** — every long-running operation of the device in one list (Settings ▾ Task Scheduler): volume check, file uploads, DNS cache writes, formatting, connection tests. Each operation gets its own block with its state, progress (an indeterminate bar while the total is unknown), the file or volume it is working on and the elapsed time, and **any unfinished one can be stopped from there** — the list only holds operations that are still running or waiting, so there is no "stoppable" flag to consult and no dead button (a paused upload drops its `.part`, a check ends at its next entry, a **format even on a card that stopped answering**: the operation ends at once and the card's supply is cut for five seconds, which breaks the stuck call). The firmware keeps the list (`core::JobRegistry`), so a subsystem only has to announce itself — a finished one-off operation leaves the list at once, and one scheduled to repeat stays with its interval. **Long operations run outside the web server's task**, so a card that takes minutes to erase, a multi-gigabyte upload, or a large download does not freeze the interface for anyone else (in a second browser the pages and the `/api/status` polling keep working)
- **Web Interface** — dark theme, RU/EN localization, DHCP/DNS/Time/Security sub-pages
- **HTTPS and Certificates** — the interface can be served over **TLS (port 443)** as well as plain HTTP (80), switched live from **Settings ▾ Security** and refused with a reason when no usable pair is stored. While TLS is up **and** the stored pair has not expired, the plain port stops serving altogether and answers **every** request — pages, files, `/api` and even an unknown path — with a **permanent redirect** to the same address over `https://` (`301` for GET/HEAD, `308` for anything else, so a POST keeps its method and body); the name in the answer is the one the client asked for (`Host`), so `http://dns.lo/pages/files.html` lands on `https://dns.lo/pages/files.html`. It switches itself **off** with the pair: the moment the certificate expires, and whenever TLS is off or no pair is usable, the plain port serves again — and the device's own IP stays reachable both ways, since the redirect is the same for every name The pair itself lives on **Settings ▾ Security → Certificates**: the volume it sits on (internal FAT or microSD, with the offer to **copy** it when you switch), a self-signed **ECDSA P-256** certificate generated on the device (name typed by the operator and **remembered on the device**, default `dhcpserver.local`, plus the device's own IP as SAN; validity chosen from **1, 2, 3, 5 or 10 years**, 5 by default) — both asked for **in the dialog of the Create button**, which pre-fills the remembered name and period, follows every keystroke with the sentence that is about to be issued and warns that an existing pair will be replaced — the state of the pair in words (ready, no certificate, volume not mounted, unreadable, expired, not valid yet), the days left (warned about under 30) and three actions: create, **download the certificate** and delete. The **private key is never downloadable** — no route hands it out. A switch the boot could not honour yet is **not lost**: the certificate cannot be judged before the clock is set, so a stored "on" with a pair that reads as *not valid yet* is retried by the device itself as soon as SNTP — or a manual clock setting — provides a date, and `https_enabled` reports what is **listening**, not what is remembered
- **REST API** — full device management over HTTP with Basic auth + rate limiting
- **OTA Updates** — firmware update via web interface, dual OTA partitions for safe upgrades
- **Terminal Menu** — UART console with `lan status`, `passwd reset`, `settings reset` (factory reset), `version` and `reboot`
- **Link Status LED** — link status indication on boards with a user LED; compiled out on the ESP32-P4-ETH (it has no user LED)
- **UART Console & Flashing** — via onboard USB-C / CH343P

---

## 🔧 Hardware — Waveshare ESP32-P4-ETH

The project targets the **Waveshare ESP32-P4-ETH** development board. Ethernet is
**onboard** (10/100 RJ45) — no external Ethernet module or wiring is required.

![Waveshare ESP32-P4-ETH board](Docs/images/esp32-p4-eth_board.svg)

| Component | Specification |
|-----------|--------------|
| Board | Waveshare ESP32-P4-ETH |
| MCU | ESP32-P4 NRW32 — ESP32-P4 module, dual-core RISC-V (max **360 MHz** on this silicon revision) |
| Ethernet | Onboard 10/100 Mbps RJ45 — internal EMAC + **IP101GRI** PHY (RMII) |
| Flash | GigaDevice 25Q256EY1G — SPI NOR, 256 Mbit (**32 MB**) |
| PSRAM | 32 MB (stacked in the ESP32-P4 module) |
| USB-UART | **CH343P** — USB-C → UART/TTL (console + flashing) |
| Audio | **ES8311** codec + **NS4150B** 3 W power amplifier (mic / speaker) |
| Wi-Fi / BT | — (not available on ESP32-P4) |
| LED | only a power indicator on board — no user LED; the link-status LED feature is compiled out (GPIO `-1`) |

### Onboard Chips

| Chip | Manufacturer | Function |
|------|--------------|----------|
| **ESP32-P4 NRW32** (FEFO FMDD297) | Espressif Systems | Dual-core RISC-V microcontroller, 32 MB PSRAM |
| **IP101GRI** | IC Plus Corp | 10/100 Ethernet PHY transceiver (RMII) |
| **25Q256EY1G** | GigaDevice | SPI NOR flash, 256 Mbit (32 MB) |
| **CH343P** | WCH | USB → high-speed UART/TTL bridge (console) |
| **ES8311** | Everest Semiconductor | Low-power mono audio codec (DAC/ADC) |
| **NS4150B** | — | Audio power amplifier, 3 W × 1 |

### Flashing & Console

Connect the board to the PC over **USB-C**. The onboard **CH343P** exposes the
ESP32-P4 UART0 console and the ROM bootloader. Hold **BOOT** while resetting to
enter download mode.

---

## 🚀 Quick Start

> The firmware is **target-conditional** (`CONFIG_IDF_TARGET_ESP32P4`). The
> ESP32-P4 path drives the internal RMII EMAC + **IP101GRI** PHY; the classic
> ESP32 path drives an **ENC28J60** over SPI. Both share the same
> DHCP/DNS/time/web application code.

### Option A — ESP32 + ENC28J60 (PlatformIO)

Reference/legacy target — the ENC28J60 code path is preserved and fully
buildable with PlatformIO.

**Prerequisites:** [PlatformIO](https://platformio.com/), Python 3.8+, an
ESP32-WROOM-32 board + ENC28J60 module (wiring in `Docs/ENC28J60.md`).

```bash
# Build firmware
pio run -e esp32dev

# Flash firmware + SPIFFS web content (run separately!)
pio run -e esp32dev --target upload
pio run -e esp32dev --target uploadfs

# Monitor serial output
pio run -e esp32dev --target monitor
```

> ⚠️ Never combine `--target upload --target uploadfs` in one command — SPIFFS
> would be flashed twice and the firmware skipped. Run them separately, then
> reboot the device.

### Option B — ESP32-P4-ETH (native ESP-IDF)

> ⚠️ PlatformIO's `espressif32` platform does **not** ship an ESP32-P4 MCU
> or board definition, so the ESP32-P4 target is built with the **native
> ESP-IDF** toolchain. No `[env:esp32-p4-eth]` exists in `platformio.ini` on
> purpose.

**Prerequisites:** ESP-IDF **v6.1** — the version the ESP32-P4 target is built
and tested with — with the RISC-V toolchain (`riscv32-esp-elf`) and the
Waveshare ESP32-P4-ETH board (onboard Ethernet — no extra module).

```powershell
# 1. Select the ESP32-P4 target (applies sdkconfig.defaults.esp32p4)
idf.py set-target esp32p4

# 2. Build
idf.py build

# 3. Flash firmware, then monitor the console
idf.py -p COMx flash
idf.py -p COMx monitor

# 4. Upload the web UI (SPIFFS) — native ESP-IDF has no "uploadfs"
.\scripts\upload_web_p4.ps1 -Port COMx
```

> ⚠️ PlatformIO's `uploadfs` does not exist for the ESP32-P4 — the web content
> (`data/`) lives in a separate SPIFFS partition and is uploaded with
> [`scripts/upload_web_p4.ps1`](scripts/upload_web_p4.ps1) (flash the firmware
> first, then the web UI).

> ℹ️ After this first cable flash, both parts can be updated **through the
> browser**: the Version page uploads `build/DHCPServer.bin` over the air (OTA)
> and replaces the SPIFFS content from a locally picked `data` folder — see
> [Docs/ESP32-P4-ETH.md](Docs/ESP32-P4-ETH.md#ota-updates). The picker accepts a
> folder only if it holds `index.html` (a wrong folder is refused with the reason
> instead of being uploaded), and once every file has arrived the device is told
> which files the folder holds and **removes the rest** (`POST /api/web/sync`),
> after showing the operator exactly what will go — so the tree on the device
> becomes the folder's tree and the interface is updatable remotely, without a
> cable and without flashing the SPIFFS image. The same two calls are available
> from a command line: `scripts\sync_web_p4.ps1 -Device <host>` is a dry run that
> lists the extra files, `-Delete` removes them, and `-Upload` sends the folder
> first (the prune is skipped when any file failed to arrive).

> ℹ️ `idf.py set-target` already performs a full reconfigure and `idf.py build`
> re-runs CMake when `CMakeLists.txt`/`sdkconfig*` change, so no separate step
> is needed. Run `idf.py reconfigure` after **adding or removing a source
> file**: ESP-IDF expands `SRC_DIRS` with a plain `file(GLOB ...)` (no
> `CONFIGURE_DEPENDS`), so a brand-new file would otherwise be ignored.

Target configuration files for this build:

- [`sdkconfig.defaults.esp32p4`](sdkconfig.defaults.esp32p4) — EMAC instead of
  SPI Ethernet, 32 MB flash, silicon rev v1.3 support, partition table override.
- [`partitions/dhcp_partitions_p4.csv`](partitions/dhcp_partitions_p4.csv) —
  32 MB partition table (2× OTA + 1 MB SPIFFS + ~21 MB FAT).

Full build/flash/web-UI/OTA/troubleshooting walkthrough:
[`Docs/ESP32-P4-ETH.md`](Docs/ESP32-P4-ETH.md). Partition layout details are in
[`Docs/PartitionTable.md`](Docs/PartitionTable.md).

---

## ⚙️ Configuration

### Static IP Addresses

| Protocol | Address |
|----------|---------|
| IPv4 | `192.168.1.201` |
| IPv6 | `fd12:3456:789a:0001:021b:21ff:fe6b:8c4d` |
| External DNS | `192.168.1.1` |

Default addresses come from the `EthManager` constructor (`src/eth/EthManager.h`, classic ESP32: `src/wifi/WiFiManager.h`); the address the DHCP server hands out as its own (`server_ip`, default `192.168.1.201`) is configurable in the web UI and stored in NVS (`src/core/Config.cpp`).

### Firmware Version

Defined in menuconfig (`Kconfig.projbuild`) or `sdkconfig.defaults`:

| Field | Format | Default | Description |
|-------|--------|---------|-------------|
| `aa` | 00-99 | `01` | Global version |
| `bb` | 00-99 | `02` / `03` | Device code — `02` = ESP32 + ENC28J60, `03` = ESP32-P4-ETH |
| `xxx` | 000-999 | `043` | Release number |
| `cc` | 00-99 | `00` | Sub-release |
| `YY` | 00-99 | `26` | Year (2026) |
| `MM` | 01-12 | `09` | Month |
| `RR` | 2 chars | `RU` | Region |

Example: `01.03.043.00.26.09.RU` — see [Docs/FirmwareVersion.md](Docs/FirmwareVersion.md).
The minimum manual date/time (`CONFIG_FW_MIN_DATETIME`, hour granularity) is a build constant refreshed by the version scripts together with the release.

### Partition Table

The repo ships two tables: the legacy 4 MB table for the ESP32
([`partitions/dhcp_partitions.csv`](partitions/dhcp_partitions.csv)) and a
32 MB table for the ESP32-P4-ETH
([`partitions/dhcp_partitions_p4.csv`](partitions/dhcp_partitions_p4.csv)).
The ESP32-P4 uses the internal 32 MB flash with the `phy_init`/RF-calibration
partition removed (no 2.4 GHz radio) and much larger OTA slots.

**ESP32-P4-ETH layout (32 MB, `dhcp_partitions_p4.csv`):**

| Partition | Offset | Size | Usage |
|-----------|--------|------|-------|
| nvs | 0x9000 | 24 KB | Configuration storage |
| otadata | 0x10000 | 8 KB | OTA boot selection |
| ota_0 | 0x20000 | 5 MB | OTA app slot 0 |
| ota_1 | 0x520000 | 5 MB | OTA app slot 1 |
| spiffs | 0xA20000 | 1 MB | Web interface files |
| fat | 0xB20000 | ~21 MB | FAT data partition (RW, mounted `/fat`): holds `cache.dat` and, when the internal volume is chosen, the HTTPS pair in `certs/` |

**Legacy ESP32 layout (4 MB, `dhcp_partitions.csv`):**

| Partition | Offset | Size | Usage |
|-----------|--------|------|-------|
| nvs | 0x9000 | 24 KB | Configuration storage |
| phy_init | 0xF000 | 4 KB | RF calibration |
| otadata | 0x10000 | 8 KB | OTA boot selection |
| ota_0 | 0x20000 | 1.5 MB | OTA app slot 0 |
| ota_1 | 0x1A0000 | 1.5 MB | OTA app slot 1 |
| spiffs | 0x320000 | 896 KB | Web interface files |

---

## 🖥️ Web Interface

Access: `http://192.168.1.201` (default static IP)

**Default login:** `admin` / `admin`

### Pages

| Page | Route | Description |
|------|-------|-------------|
| Home | `/index.html` | System status: device date/time, DHCP/DNS/NTP state, CPU/RAM, storage (internal FAT + microSD), cache stats |
| DHCP ▾ Setup | `/pages/dhcp_setup.html` | Server status, IP, address range, lease table cap |
| DHCP ▾ DNS | `/pages/dhcp_dns.html` | Built-in DNS status, mode/address |
| DHCP ▾ Static Bindings | `/pages/dhcp_static.html` | Static MAC→IP bindings (enable/DNS) |
| DHCP ▾ Logging | `/pages/dhcp_logging.html` | DHCP REST logging (URL/auth/Test) |
| DNS ▾ Setup | `/pages/dns_setup.html` | DNS forwarding, mode/address, query filters (LAN-only, non-A/AAAA blocking) |
| DNS ▾ Internal Cache | `/pages/dns_internal.html` | Built-in PSRAM DNS cache: on/off, ignore TTL, "withstand a reboot" switches for the statistics (`Statistica.dat`) and the cache (`cache.dat`), usage frequency (names used, most-used record), save/load `cache.dat` with progress |
| DNS ▾ External Cache | `/pages/dns_cache.html` | External REST cache URL, cache stats |
| DNS ▾ Local Hosts | `/pages/dns_local_hosts.html` | Local domain→IP mappings |
| DNS ▾ Logging | `/pages/dns_logging.html` | DNS REST logging |
| Time ▾ General | `/pages/ntp_setup.html` | NTP server: on/off, LAN-only filter + reply rate limit, clock sync on/off, external NTP, timezone, sync interval, manual date/time setting (typed or taken from the computer) |
| Time ▾ Logging | `/pages/ntp_logging.html` | NTP request logging (terminal + external REST URL/auth/Test) |
| Settings ▾ Files | `/pages/files.html` | File explorer: internal FAT + microSD, download/upload/rename/delete, actions menu (new file, new folder, select all, upload file, upload folder, copy/move the selection — including **between the two volumes**, with progress and cancel — delete, refresh, format card), text editor |
| Settings ▾ Security | `/pages/security.html` | Web login (username/password), max attempts, lockout period, HTTPS switch (with the certificate state) |
| Settings ▾ Security | `/pages/certs.html` | Certificate pair for HTTPS: volume (FAT/microSD, with copying when switching), state of the pair in words, days left, and create / download the certificate / delete (the private key is never downloadable); **Create** asks in a dialog for the name (pre-filled with the one remembered on the device, cleared with the × to take the default) and the validity (1/2/3/5/10 years, the remembered one pre-selected), shows the resulting sentence live and warns that an existing pair will be replaced |
| Settings ▾ Import | `/pages/settings_import.html` | Restore settings from an exported JSON file |
| Settings ▾ Export | `/pages/settings_export.html` | Download the current settings as a JSON file |
| Settings ▾ Device | `/pages/settings_device.html` | Reboot — which first writes and *shows* what the two "before reboot" switches ask for (statistics, then the cache with its percentage) and only then restarts — and factory reset (erases all settings) |
| Settings ▾ Task Scheduler | `/pages/jobs.html` | Long-running operations of the whole device: state, progress, current step, elapsed time, a stop button for every unfinished one (polls `GET /api/jobs` every 2 s) |
| Help ▾ Version | `/pages/version.html` | Firmware version info, OTA firmware upload, web-file (SPIFFS) upload |

### Language

Toggle between Russian and English using the RU/EN buttons in the navigation bar.

---

## 📡 REST API

All endpoints require HTTP Basic Authentication.

### Endpoints

| Method | Route | Description |
|--------|-------|-------------|
| GET | `/api/status` | System status (network, DHCP, DNS, CPU, RAM, uptime, storage volumes) |
| GET | `/api/version` | Firmware version string |
| GET | `/api/dhcp/settings` | DHCP configuration |
| POST | `/api/dhcp/settings` | Update DHCP configuration |
| GET | `/api/dhcp/static-bindings` | Static MAC→IP bindings |
| POST | `/api/dhcp/static-bindings` | Update static bindings |
| GET | `/api/dhcp/leases` | Active DHCP leases |
| GET | `/api/dns/settings` | DNS configuration |
| POST | `/api/dns/settings` | Update DNS configuration |
| GET | `/api/dns/local-hosts` | Local DNS host mappings |
| POST | `/api/dns/local-hosts` | Update local DNS host mappings |
| GET | `/api/security/settings` | Security settings (no password) + whether the TLS listener is up, and whether a certificate can serve it |
| POST | `/api/security/settings` | Update security settings (the HTTPS server on port 443 is switched live, or the request is refused with the reason) |
| GET | `/api/security/certificates` | Certificate pair: state of the two files, volume, subject/SAN, validity and days left, sizes, the list of validity choices and the default name (`default_name`) the Create dialog pre-fills |
| POST | `/api/security/certificates` | Generate a pair (`generate`), delete it (`delete`), move/store it on another volume (`storage`); an accepted action re-applies the HTTPS listener when the switch was already on (`https_restart`, decided by the pair that is left) |
| GET | `/api/security/certificates/download` | Download the certificate as PEM (the private key has no route at all) |
| GET | `/api/settings/export` | Export all settings as JSON (passwords excluded) |
| POST | `/api/settings/import` | Import settings from JSON |
| POST | `/api/settings/reset` | Factory reset all settings and reboot |
| POST | `/api/device/reboot` | Reboot the device (optional `{"saved": true}` = the restart files are already written) |
| POST | `/api/device/reboot/prepare` | Start the files a planned restart wants (statistics and cache, each as a background job) and report what each step decided |
| POST | `/api/ota/upload` | Upload firmware (raw body; `multipart/form-data` also accepted; `?saved=1` = the restart files are already written, skip them) |
| POST | `/api/web/file?path=<rel>` | Upload a web (SPIFFS) file |
| POST | `/api/web/sync` | Make the device's web tree equal to an uploaded folder: dry run lists the extra files, `"delete":true` removes them |
| GET | `/api/files/volumes` | Explorer volumes (id, mount point, mounted/present, capacity, last mount error) |
| GET | `/api/files/list?volume=<id>&path=<dir>` | Directory listing + free space |
| GET | `/api/files/download?volume=<id>&path=<file>` | Download a file (chunked; runs outside the server task, so a large download does not block other clients) |
| POST | `/api/files/upload?volume=<id>&path=<file>` | Upload a file (raw body, atomic, resumable with `total`/`offset`; read outside the server task, so a folder upload does not block other clients) |
| GET | `/api/files/upload/offset?volume=<id>&path=<file>` | How much of a paused upload is already on the device |
| POST | `/api/files/upload/cancel?volume=<id>&path=<file>` | Throw away the temporary file of an upload |
| POST | `/api/files/mkdir` | Create a directory |
| POST | `/api/files/rename` | Rename / move (inside one volume) |
| POST | `/api/files/transfer` | Copy or move files/directories **between volumes** (`op`, `src_volume`, `paths[]`, `dst_volume`, `dst_path`, `conflict`); runs outside the server task, answers `409` + the taken names before anything is copied |
| GET | `/api/files/transfer` | Snapshot of the transfer job (phase, bytes, counters, first error), polled while it runs |
| POST | `/api/files/transfer/cancel` | Ask the running transfer to stop (what was already copied stays) |
| POST | `/api/files/delete` | Delete a file or directory (`recursive` for a non-empty one) |
| POST | `/api/files/format` | Format the microSD card (`confirm: true`); runs outside the server task, so a long erase does not block the UI and can be stopped from the scheduler |
| GET | `/api/files/text?volume=<id>&path=<file>` | Read a text file for the editor |
| POST | `/api/files/text` | Save a text file (`mtime` guards against overwriting a newer version) |
| GET/POST | `/api/files/settings` | Explorer access policy (LAN-only filter on/off + state) |
| POST | `/api/files/check` | Start a read-only volume check (“Check for errors”) |
| GET | `/api/files/check` | Report/progress of that check, polled while it walks |
| POST | `/api/files/check/cancel` | Ask the running check to stop |
| GET | `/api/jobs` | Long-running operations of the device (the scheduler page) |
| POST | `/api/jobs/cancel` | Ask an unfinished operation to stop (`{"id": …}`) |
| POST | `/api/test-connection` | Test a REST endpoint from the device |
| GET | `/api/dns/internal-cache/file` | `cache.dat` info (exists/size/entries) |
| GET | `/api/dns/internal-cache/progress` | Background save/load job progress (`busy`, `save`, `checking`, `done`, `total`, `percent`, `last_result`, `last_detail`) |
| GET | `/api/dns/stats/progress` | Background statistics-write state and verdict (`busy`, `checking`, `last_result`, `last_detail`; `last_result` is `ok`/`skipped`/`mismatch`/`failed`) |
| POST | `/api/dns/internal-cache/save` | Save the PSRAM cache to `cache.dat` (async) |
| POST | `/api/dns/internal-cache/load` | Restore the cache from `cache.dat` (async) |
| POST | `/api/dns/internal-cache/reset` | Drop every cached answer held in PSRAM (auth) |
| GET | `/api/time/settings` | NTP server configuration + status |
| POST | `/api/time/settings` | Update NTP server configuration |
| GET | `/api/time/now` | Current device time (UTC/local, unix) |
| POST | `/api/time/set` | Set the device clock manually (local date/time + UTC offset) |

Full documentation: [Docs/Rest.md](Docs/Rest.md)

---

## ⌨️ Terminal Menu

Connect via serial at 115200 baud.

```
dhcp> help
Available commands:
  lan status                  — Show LAN connection status and IP
  passwd reset                — Reset web password to default (admin)
  settings reset              — Factory reset ALL settings to defaults and reboot
  version                     — Show firmware version
  help                        — Show this help
  reboot                      — Reboot the device
```

> The `lan status` command reports the wired Ethernet link through the `IWiFiManager` interface (implemented by `EthWifiAdapter`).

---

## 🏗️ Project Structure

```
DHCPServer/
├── CMakeLists.txt           # Component CMakeLists (used by ESP-IDF)
├── platformio.ini           # PlatformIO configuration (ESP32 + ENC28J60)
├── sdkconfig.defaults       # Shared ESP-IDF defaults (ESP32)
├── sdkconfig.defaults.esp32p4 # ESP32-P4-ETH overrides (EMAC, 32 MB flash)
├── 3DModel/                 # Blender model of the device enclosure (.blend)
├── components/
│   └── enc28j60/           # ENC28J60 SPI Ethernet driver (classic ESP32)
├── partitions/
│   ├── dhcp_partitions.csv      # 4 MB table (ESP32 + ENC28J60)
│   └── dhcp_partitions_p4.csv   # 32 MB table (ESP32-P4-ETH)
├── scripts/
│   ├── inc_firmware_ver.py     # Version bump (--sub / --rel / --min-only)
│   ├── set_firmware_date.py    # Release date + minimum manual date/time
│   ├── kconfig_tools.py        # Shared helpers used by both scripts above
│   ├── upload_web_p4.ps1       # Upload data/ to the SPIFFS partition (ESP32-P4)
│   └── upload_spiffs.py        # SPIFFS image helper (PlatformIO / legacy)
├── src/
│   ├── main.cpp            # Application entry point
│   ├── CMakeLists.txt      # Component sources (wifi/ excluded on ESP32-P4)
│   ├── Kconfig.projbuild   # Project configuration options
│   ├── core/               # Version, Config, Subnet helper, CPU monitor,
│   │                       # error log (Errors.log on FAT, written by its own
│   │                       # task through a queue in PSRAM)
│   ├── eth/                # Ethernet manager (ENC28J60 SPI on ESP32 /
│   │                       #   internal EMAC + IP101GRI on ESP32-P4-ETH)
│   ├── dhcp/               # DHCP server
│   ├── dns/                # DNS proxy, cache, logger
│   ├── files/              # File explorer facade (volumes, list/upload/rename/delete)
│   ├── time/               # NTP server + SNTP client + logger + date/time math
│   ├── web/                # HTTP/HTTPS server, auth, REST API, JSON writers, multipart extractor
│   ├── led/                # LED controller (no-op on ESP32-P4-ETH)
│   ├── menu/               # Terminal menu
│   ├── security/           # Certificate builder + certificate store (HTTPS pair)
│   ├── storage/            # SPIFFS wrapper + FAT/microSD volumes + path policy
│   └── wifi/               # WiFiManager (ESP32 only — not built on ESP32-P4)
├── data/                   # SPIFFS web content
│   ├── index.html
│   ├── login.html          # Auth gate (served at /)
│   ├── header.html / footer.html
│   ├── css/style.css
│   ├── js/app.js
│   ├── i18n/{ru,en}.json
│   └── pages/              # DHCP / DNS / Time / Settings (incl. files.html, jobs.html, certs.html) / Help sub-pages
├── test/                   # Unit tests (host-style, no board needed)
├── Docs/                   # Documentation
│   ├── images/              # Board photos and screenshots
│   ├── ENC28J60.md
│   ├── ESP32-P4-ETH.md      # P4 build/flash/web-UI guide
│   ├── ESP-Prog.md
│   ├── FirmwareVersion.md
│   ├── History.md
│   ├── Dialog.md
│   ├── Rest.md
│   └── PartitionTable.md
└── Plan/                   # Project planning
    ├── Task.md
    ├── Structure.md
    ├── Stages.md
    └── Hardware.md
```

---

## 🧪 Testing

Tests live in `test/test_*.cpp`; each file defines its own entry point — usually
`app_main`, a few define `main()` themselves.

Modules with no ESP-IDF dependency are compiled and run **on the PC** (fast,
no board needed) — this is how `Subnet`, `TimeMath`, `PathUtil`,
`MultipartExtractor` and the file-explorer JSON writers (`FileJson` /
`JsonWriter`) are verified. Code that does touch ESP-IDF is covered the same way
when the run needs a controllable environment: `test/test_internalcache.cpp`
builds against the stand-ins in `test/stubs` (capability allocator, a clock the
test moves, a FreeRTOS mutex that does nothing, lwIP's `inet_pton`/`inet_ntop`)
and asks for `-DDHCP_TEST_HOST`.

**One command runs the whole suite** — `scripts/run_host_tests.py`:

```powershell
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" scripts\run_host_tests.py
```

It reads each test's own "Build (MinGW…)" recipe, adds the translation units
that recipe forgets (`EXTRA_UNITS` in the script, and section 9 of
`Plan/Memory.md`), picks the entry-point shim the file needs — a two-line file
that calls `esp_test_app_main()`, whose symbol differs for `void`/`int` and for
C or C++ linkage — compiles with `-fsyntax-only` first, so "does not compile" is
told apart from "does not link", runs every test from the repository root and
writes one log per test into `%TEMP%\dhcpserver_host_tests\logs`. Six tests are
listed in the script as not buildable on a PC (they need ESP-IDF headers, or the
board); anything else that fails to build makes the script exit non-zero. The
compiler is MinGW `g++` (`C:\Qt\Tools\mingw1310_64\bin`, overridable with
`--mingw <dir>` or `DHCPSERVER_MINGW`).

A single test by hand looks like this (the shim is the one the script writes):

```bash
g++ -std=c++17 -Wall -Wextra -Dapp_main=esp_test_app_main -I. \
    test/test_subnet.cpp src/core/Subnet.cpp host_main.cpp -o test_subnet
```

The same files are also built and run **on the board** through PlatformIO
(the shared logic classes are target-independent, so the board runs the same
code the ESP32-P4 build compiles):

```bash
pio test -e esp32dev
```

Test files:
- `test/test_subnet.cpp` — IPv4 subnet arithmetic for the DNS/NTP LAN filters (host-runnable)
- `test/test_time.cpp` — date/time parsing and Unix conversion (host-runnable)
- `test/test_version.cpp` — Version formatting and components
- `test/test_config.cpp` — Config read/write roundtrip
- `test/test_auth.cpp` — Auth manager with lockout
- `test/test_dhcp.cpp` — DHCP server lifecycle
- `test/test_dns.cpp` — DNS cache stub
- `test/test_wifi.cpp` — WiFiManager (needs hardware) and LedController
- `test/test_pathutil.cpp` — file-explorer path policy (escape attempts, illegal names) — host-runnable
- `test/test_uploadrange.cpp` — chunk arithmetic of a resumable upload (continue, complete, mismatched offset) — host-runnable
- `test/test_transferengine.cpp` — copy/move between volumes against the real host filesystem: counters, tree copy, `move` deleting the source only after a complete copy, conflicts/skip/merge, cancel, a directory with more entries than one listing holds, refusing a destination that is too small — host-runnable
- `test/test_filesink.cpp` — the temporary file of an upload: keep, continue, publish, discard (needs `-I test/stubs` for the logging stub) — host-runnable
- `test/test_jobregistry.cpp` — rules of the job list behind the scheduler page (one-off removal, repeats, paused operations, cancellation) — host-runnable
- `test/test_multipart.cpp` — `MultipartExtractor` for OTA bodies (any chunk size) — host-runnable
- `test/test_filejson.cpp` — file-explorer/storage JSON payloads + structural comma checks — host-runnable
- `test/test_internalcache.cpp` — the PSRAM DNS cache: usage counter (hit, store, refresh, recycled node), least-used eviction, TTL expiry, save/load roundtrip (format v2), a version 1 file, the counter saturating at the maximum instead of wrapping through zero and a file value above `UINT32_MAX` coming back clamped, plus the two cases that pin the 32-bit millisecond clock's turnover down (the age of a record and the save/load of live entries stay exact across it, which only works because the age is a modular subtraction — a naive "the clock went backwards" guard fails them) — host-only (`-DDHCP_TEST_HOST -I test/stubs`, link `-lws2_32`): the stub clock makes TTL expiry testable without sleeping, and the test fills the whole pool, which is where it found the uninitialised bucket heads
- `test/test_dnsstatstore.cpp` — the file that keeps the DNS cache statistics next to `cache.dat`: a fixed record size and a magic, a `u64` counter that survives a round trip at its maximum, the average staying an average **across a reboot** ((sum + new) / (hits + new)), damaged records refused with a reason instead of being read as numbers (empty, truncated, trailing bytes, a foreign file, a version from the future, one flipped bit, a wrong size), save and load, a refused save leaving the previous file intact with no `.tmp` behind, a missing file as the normal case, removal, and the retry that moves a file it cannot read out of the way rather than overwriting it — host-runnable
- `test/test_md5.cpp` — MD5 against the RFC 1321 vectors and the lengths that break a wrong implementation (55/56/57, 63/64/65, 119/120/121 bytes, with the expected values taken from an independent implementation), a digest that does not depend on the chunk size (1…4096 bytes), `hex()` being idempotent, the file helper reading in 4 KB blocks, and a missing file being a different answer from an empty one — host-runnable
- `test/test_restartsavejobstate.cpp` — the state both "write a file before the restart" jobs use: single-flight (a second request is refused, not started), the verdict a finished job stores, no inherited verdict when a new run starts (the page would otherwise read the previous run's luck), a switch turned off mid-run not faking the outcome, and the poll sequence a page sees across two runs — host-only (`g++ -DDHCP_TEST_HOST`), which is the point: every bug this project had in this area lived in that logic rather than in the file I/O
- `test/test_allowedlist.cpp` — the DHCP allow-list module: MAC parsing in every shape a human types, the NVS codec round trip (including the two-field line an older firmware wrote), the per-entry Enable flag (a switched-off entry is kept but never allows), the 12-entry/20-character limits, duplicate and junk dropping, the A/B table swap, the fail-open case when PSRAM is missing, and the policy with the switch off, with an enabled/disabled static binding and with a stranger — host-runnable
- `test/test_clientname.cpp` — the name a DHCP client reports about itself (option 12, option 81 in both encodings, option 12 winning over 81), the bounded option walk (a length past the buffer, a missing length byte, PAD between options), the text filter (control characters and `|` dropped, valid UTF-8 kept, invalid bytes dropped) and the truncation on a character boundary — host-runnable
- `test/test_ptrprobe.cpp` — the reverse-DNS probe: the query names the address backwards, and the answer parser follows compression pointers with a jump budget, accepting a value that points back into the question or is written out in full, and refusing a wrong id, a QR-clear packet, an error rcode, an A record where a PTR belongs, a datagram cut in the middle, a forward pointer and a pointer loop — host-runnable
- `test/test_nbstatprobe.cpp` — the NetBIOS node-status probe: the wildcard query byte for byte (first-level encoding, QTYPE 0x0021), a name table where the workstation entry wins over the server entry of the same computer, group entries (`WORKGROUP`, `__MSBROWSE__`) never winning, an empty table, a wrong id / a QR-clear packet / an error rcode / no answers, a truncated table, an `RDLENGTH` past the data, an `RDLENGTH` of zero and a full 15-character name — host-runnable
- `test/test_routetable.cpp` — the route table as a table: a repeated URI+method pair is still found when it carries another handler, the same URI under another method is not a repeat (GET and POST of one resource are two legal routes), the comparison is exact (`/api/status` and `/api/status2` are strangers), an empty URI or a null handler is refused, two "empty" rows are not a duplicate, `slotsFor(n)` returns exactly `n` slots — which is the defect of stage 137 in one line, a hard-coded limit of 81 rows for 82 routes dropping the last one — and, the case that keeps the web UI honest, every file under `data/` has a route parsed out of `src/web/WebServer.cpp`, with a `SKIP` instead of a false verdict when the project tree is not next to the test; it also reads `CONFIG_SPIFFS_OBJ_NAME_LEN` out of the `sdkconfig` files and refuses a path that SPIFFS could not open at runtime, which is how a page that builds, flashes and uploads fine still answers 404 (`/pages/dhcp_static_bindings.html` in 2026-08-09, `32 > 31`) — host-runnable
- `test/test_restsenderlimits.cpp` — the send limits that used to be four names of the same timeout now live in one place, and this test is what keeps them there: `static_assert`s on all five values (`6000`, `500`, `10`, `5000`, `1024` ms/bytes), the order `10 < 500 < 6000` so the stop path fits inside the drain deadline, and buffers a power of two and large enough not to truncate a log record — host-runnable
- `test/test_autosaveperiod.cpp` — the period of the cache autosave (minute, hour, day): the seconds of each unit and the stored indices (hour `0` and day `1` keep the meaning stage 153 gave them, minutes take the index the month left, and an index nobody ever wrote falls back to hours), the bound an interval is clamped into (60 minutes, 24 hours, and for days the length of the month that is running — 28 in February, 30 in September, `0`/40 meaning "not a month" gives 31), clamping `0 → 1` rather than refusing a settings file an older firmware wrote, seconds = interval × unit, one unit worth exactly its own number of the smaller ones, and a worst case that still fits the 32-bit countdown — every number pinned by a `static_assert` so that changing one stops the build — host-runnable
- `test/test_errorlogcore.cpp` — the part of the error log that is plain C++: the stamp (a device whose clock is not set gets `t+152s`, never a 1970 date that looks like a fact), the truncation marker on a message too long for one queue item, the drain into a target, and the drop counter — a full queue or a target that refuses a line must be **reported inside the log**, because a log with silent holes is worse than none
- `test/test_certstore.cpp` — the rules the certificates page and the API share, checked where they are defined: the mount point of the pair (`/fat` — the internal FAT partition, **not** the SPIFFS volume the web files live on, which a data re-flash would wipe), the file, folder and volume names a round trip through the page has to survive (a name that parsed back as the other volume would move the certificate silently), the ASN.1 time text (a date the calendar cannot hold, or a buffer one byte short, is refused rather than clamped), five years day for day, the ends of the list of periods and the values just outside it, the status of the pair (a file that does not parse is never reported as expired, a missing volume never as a missing certificate — on the page those send the operator to different places), how many days are left (the last partial day counts as one, an expired pair is 0 and never negative, and the count, the warning and the expiry itself agree), and the filter both the default name and a typed-in one have to pass — host-runnable, and most of its numbers are `static_assert`s so that changing one stops the build instead of silently changing what the device answers
- `test/test_redirectpolicy.cpp` — the rules of the http→https redirect (stage 167): the condition is "a TLS listener is serving **and** the pair's deadline is in the future" (a listener without a judged pair, a deadline of `0`/`-1` and the very second of expiry are all refusals), `GET` and `HEAD` get `301` while every other method gets `308` so a POST arrives as a POST, and the name in `Location` is built from the request's own `Host` — the port the client used is replaced by the TLS one (`:443` is left implicit), an IPv6 literal keeps its brackets, and a name is taken whole or not at all (`isUsableHost()` refuses junk instead of cleaning it, which is what turned `Host: dns.lo\r\nX-Ignored: 1` into a redirect nobody asked for) — host-runnable
- `test/test_restartsaveverify.cpp` — the policy both "write a file before the restart" jobs now follow (stage 169): one attempt writes the file *and* reads it back, `Ok` means the volume holds what was written, `Mismatch` that it does not and `Failed` that the write did not happen; a **mismatch is written exactly once more** and never twice, the verdict describes the volume as it is now (a mismatch followed by a failed write is `Failed`, the reverse is `Mismatch`), a missing attempt counts as a failure instead of a crash or a silence, and every attempt reaches the report callback — which is what puts both attempts and both reasons into `Errors.log` — host-runnable
- `test/test_cachefilereader.cpp` — the reader of `cache.dat` and the read-back check a planned restart runs (stage 169), against files assembled **by hand** byte by byte (a builder shared with the reader would agree with it about a wrong layout): a whole file is read to the end with its records, indexes, remaining TTLs, usage counters and raw addresses; a file cut inside a record, a header promising one record while the file holds two, a foreign file, a file shorter than the header, a version from the future, an impossible record count, a zero-length name, a query type that is neither A nor AAAA and a record without an address are each refused with a reason a page can show; version 1 (no counter) and version 2 (an eight-byte one) still read; and `check()` fails on a file that holds fewer records than the save reported or no file at all — host-runnable

> No `[env:esp32-p4-eth]` test target exists (the `espressif32` PIO platform
> has no ESP32-P4 support) — test the ESP32 build, then flash the ESP32-P4
> build via native `idf.py` (see [Quick Start](#-quick-start)).

---

##  License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

