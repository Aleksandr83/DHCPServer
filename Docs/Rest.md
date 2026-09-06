# REST API Documentation

All REST endpoints require **HTTP Basic Authentication**.
Default credentials: `admin` / `admin` (configurable via Security page).

---

## GET /api/status

Get overall system status.

**Response `200 OK`:**
```json
{
  "wifi_connected": true,
  "wifi_ssid": "MyNetwork",
  "ip4": "192.168.1.201",
  "ip6": "fd12:3456:789a:0001:021b:21ff:fe6b:8c4d",
  "dhcp_running": true,
  "dns_running": true,
  "firmware_version": "01.02.001.00.26.07.RU"
}
```

> The response also carries many runtime metrics: CPU load, heap/PSRAM,
> static-bindings/local-hosts usage and the built-in DNS cache block
> (`internal_cache_available/size_mb/entries/capacity/used_bytes/free_bytes`,
> `internal_cache_hits`, `internal_cache_avg_hit_us` — average µs of a
> successful PSRAM-cache lookup, `internal_forward_count`).

---

## GET /api/version

Get firmware version string.

**Response `200 OK`:**
```json
{
  "firmware_version": "01.02.001.00.26.07.RU"
}
```

---

## GET /api/dhcp/settings

Get current DHCP server configuration.

**Response `200 OK`:**
```json
{
  "enabled": true,
  "start_ip": "192.168.1.100",
  "end_ip": "192.168.1.200",
  "subnet": "255.255.255.0",
  "gateway": "192.168.1.1",
  "lease_time": 86400,
  "log_rest": false,
  "log_url": "",
  "log_auth": false,
  "log_auth_user": "",
  "log_auth_password": ""
}
```

> `log_rest` — send DHCP events (OFFER/ACK/NAK/RELEASE/DECLINE) to the
> external REST URL `log_url`; `log_auth*` are the HTTP Basic credentials.

---

## POST /api/dhcp/settings

Update DHCP server configuration.

**Request body:**
```json
{
  "enabled": true,
  "start_ip": "192.168.1.100",
  "end_ip": "192.168.1.200",
  "subnet": "255.255.255.0",
  "gateway": "192.168.1.1",
  "lease_time": 86400,
  "log_rest": false,
  "log_url": "http://example.com/api/v1/dhcp/log",
  "log_auth": false,
  "log_auth_user": "",
  "log_auth_password": ""
}
```

**Response `200 OK`:**
```json
{
  "status": "ok"
}
```

---

## GET /api/dhcp/static-bindings

Get static MAC→IP binding list.

**Response `200 OK`:**
```json
{
  "bindings": [
    {
      "mac": "24:0A:C4:01:23:45",
      "ip": "192.168.1.50",
      "name": "Printer"
    },
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "ip": "192.168.1.60",
      "name": "Camera"
    }
  ]
}
```

> **MAC address format:** `XX:XX:XX:XX:XX:XX` where each segment is a hexadecimal byte (uppercase or lowercase).

---

## POST /api/dhcp/static-bindings

Update static MAC→IP bindings (max 512 bytes total).

**Request body:**
```json
{
  "bindings": [
    {
      "mac": "24:0A:C4:01:23:45",
      "ip": "192.168.1.50",
      "name": "Printer"
    }
  ]
}
```

**Response `200 OK`:**
```json
{
  "status": "ok"
}
```

---

## GET /api/dhcp/leases

Get active DHCP leases.

**Response `200 OK`:**
```json
{
  "leases": [
    {
      "mac": "24:0a:c4:01:23:45",
      "ip": "192.168.1.100",
      "expiry": 12345678
    }
  ]
}
```

> `expiry` is the absolute timestamp (seconds since boot) when the lease expires.

---

## GET /api/dns/settings

Get DNS server configuration.

**Response `200 OK`:**
```json
{
  "external_dns": "192.168.1.1",
  "log_terminal": false,
  "log_rest": false,
  "log_rest_sent": false,
  "log_url": "",
  "log_auth": false,
  "log_auth_user": "",
  "log_auth_password": "",
  "cache_url": "",
  "cache_rest": false,
  "cache_rest_read": true,
  "cache_rest_write": true,
  "cache_auth": false,
  "cache_auth_user": "",
  "cache_auth_password": "",
  "cache_internal": false,
  "cache_internal_size_mb": 20,
  "cache_internal_ignore_ttl": false,
  "cache_internal_available": true
}
```

> `log_auth` / `cache_auth` — send HTTP Basic auth to the REST log / cache
> endpoint. `log_auth_user` / `log_auth_password` and
> `cache_auth_user` / `cache_auth_password` hold the credentials.
> `cache_rest` — master switch for the external DNS cache; `cache_rest_read`
> enables lookups (reading from the cache) and `cache_rest_write` enables
> storing forwarded answers into the cache (they default to on).
> `cache_internal` — master switch for the **built-in** (on-device) DNS cache
> (hash table in PSRAM, ESP32-P4); `cache_internal_size_mb` is its max size
> in MB (1..20, default 20); `cache_internal_ignore_ttl` — when on, stored
> TTLs are kept but never expire entries (actualization comes later).
> `cache_internal_available` (read-only) is true when PSRAM is present and
> the cache can actually be enabled.
>
> The 20 MB cap matches the FAT partition size (~21 MB) so the cache contents
> can later be persisted to `cache.dat` on `/fat`.

---

## POST /api/dns/settings

Update DNS server configuration.

**Request body:**
```json
{
  "external_dns": "192.168.1.1",
  "log_terminal": true,
  "log_rest": false,
  "log_rest_sent": false,
  "log_url": "http://example.com/api/dns-log",
  "log_auth": false,
  "log_auth_user": "",
  "log_auth_password": "",
  "cache_url": "http://example.com/api/dns-cache",
  "cache_rest": true,
  "cache_rest_read": true,
  "cache_rest_write": true,
  "cache_auth": false,
  "cache_auth_user": "",
  "cache_auth_password": "",
  "cache_internal": false,
  "cache_internal_size_mb": 20,
  "cache_internal_ignore_ttl": false
}
```

**Response `200 OK`:**
```json
{
  "status": "ok"
}
```

> The POST applies the built-in cache settings live: enabling/disabling it,
> resizing the PSRAM hash table when `cache_internal_size_mb` changed, and
> updating the ignore-TTL flag — no reboot required.

---

## Built-in (internal) DNS cache (PSRAM)

On the ESP32-P4-ETH (32 MB PSRAM) the device keeps an on-board DNS answer
cache — a hash table stored in external PSRAM (no HTTP involved). Lookup is
**synchronous and in-memory**, so it is consulted before the external REST
cache. Effective query pipeline:

```
local hosts → internal (PSRAM) cache → external (REST) cache → forward
```

- Internal hits are answered immediately with the stored TTL.
- On a forward, the upstream reply's A/AAAA records are stored in the internal
  cache together with the minimum TTL of the answers (TTL 0 → treated as 300 s).
- When `cache_internal_ignore_ttl` is **off** an entry older than its TTL is
  treated as a miss and purged. When **on** the TTL is kept but never expires
  an entry (a dedicated actualization mechanism will be added later).
- On a full table the oldest entry is evicted. Without PSRAM the internal cache
  is unavailable (`cache_internal_available=false`); every call is a safe no-op.

### Persistence file — `cache.dat` on FAT

The internal cache can be persisted to a file on the **FAT** partition
(`/fat/cache.dat`, only present on the ESP32-P4 layout) so it survives a
reboot. The DNS server **auto-loads** the file at start() (in a background
job) when the cache is enabled; Save/Load can also be triggered manually via
REST. Save/Load run in a **background low-priority task** — the HTTP handlers
return immediately (`started`) and the UI polls the progress endpoint until
`busy=false`. At most one operation runs at a time.

#### `GET /api/dns/internal-cache/file`

Info about the cache file (auth required).

**Response `200 OK`:**
```json
{
  "exists": true,
  "path": "/fat/cache.dat",
  "size_bytes": 4096,
  "entries": 512,
  "version": 1
}
```

#### `GET /api/dns/internal-cache/progress`

Progress of the running background save/load job (auth required).

**Response `200 OK`:**
```json
{
  "busy": true,
  "save": true,
  "done": 256,
  "total": 512,
  "percent": 50
}
```

> `busy=false` means no job is running; `done`/`total` keep the last
> finished job's values so the UI can show "finished at N entries".

#### `POST /api/dns/internal-cache/save`

Start a background save of the whole PSRAM cache to `/fat/cache.dat` (auth
required). Returns immediately; poll `GET .../progress`.

**Response `200 OK`:**
```json
{
  "status": "started",
  "op": "save"
}
```

**Errors:** `409 Conflict` — internal cache disabled **or** another operation
is already running; `500` — the background job could not start.

#### `POST /api/dns/internal-cache/load`

Start a background restore of the cache from `/fat/cache.dat` (auth
required). Entries are re-inserted with their **remaining TTL**, so they
expire after the remaining time once reloaded. Returns immediately; poll
`GET .../progress`.

**Response `200 OK`:**
```json
{
  "status": "started",
  "op": "load"
}
```

**Errors:** `409 Conflict` — internal cache disabled or another operation is
already running; `404 Not Found` — no cache file yet; `500` — the background
job could not start.

**File format:** binary, little-endian — 16-byte header (`"DCC1"` magic,
u32 version=1, u32 entryCount, u32 reserved), then per entry: nameLen u8 +
name, qtype u16, nA u8, nAAAA u8, remaining-ttl u32, then nA×4 B IPv4 and
nAAAA×16 B IPv6 raw bytes.

---

## External DNS cache (client)

The ESP32 acts as a client of an external DNS cache service. Both requests
send HTTP Basic auth if `cache_auth` is on, and an `Accept: application/json`
header (so an unauthenticated Laravel API returns 401 JSON instead of a
302 login redirect). The cache is only used when `cache_rest` is on **and**
`cache_url` is non-empty. `cache_url` is the **base** URL of the cache
resource, e.g. `https://dhcpserverweb.lo/api/v1/dns/cache` — the domain is
appended as a path segment (trailing slashes in `cache_url` are stripped).

### Lookup (read) — `GET {cache_url}/{domain}`

The cache is consulted for a query that is not in the local hosts list.
Lookup is **asynchronous** — the DNS server task never blocks on HTTP. The
cache lookup runs in a dedicated worker task; the DNS server waits for the
result (without blocking other queries) and only sends the query to the
**external DNS when the cache misses** (or does not answer within ~2 s). On a
cache hit the client is answered from the cache and no upstream query is
sent. `domain` is lower-cased, any trailing dot is stripped, then
URL-encoded.

**Response `200 OK`** (cache hit):
```json
{
  "domain": "example.com",
  "ips": ["93.184.216.34"],
  "type": 1,
  "expires_at": "2026-08-07 12:00:00",
  "updated_at": "2026-08-07 11:00:00"
}
```

**Response `404`** — not found / expired (normal miss).

> Only the `ips` array is parsed. The result is filtered by the queried type
> (A → IPv4, AAAA → IPv6) because the server keys by domain only; a record
> of the wrong type is treated as a miss (falls through to forwarding).

### Store (upsert) — `PUT {cache_url}/{domain}`

Whenever a query is resolved via the external DNS forwarder, the A/AAAA
answer is pushed to the external cache. This is **fire-and-forget**: records
go into a bounded ring buffer (depth 32, oldest dropped on overflow) and a
dedicated sender task PUTs them, so a slow cache never blocks DNS.

**Request body** (the domain comes from the URL path):
```json
{
  "ips": ["93.184.216.34"],
  "type": 1
}
```

**Response:** `200` (any 2xx is accepted; failures are logged as WARN).
TTL is left to the server default (`cache.default_ttl`).

---

## GET /api/security/settings

Get security/authentication configuration.

> Password is NOT returned in the response for security reasons.

**Response `200 OK`:**
```json
{
  "username": "admin",
  "max_attempts": 5,
  "lockout_period": 300
}
```

---

## POST /api/security/settings

Update security/authentication configuration.

**Request body:**
```json
{
  "username": "admin",
  "password": "newpassword",
  "max_attempts": 5,
  "lockout_period": 300
}
```

> If `password` is empty or omitted, the existing password is kept unchanged.

**Response `200 OK`:**
```json
{
  "status": "ok"
}
```

---

## POST /api/ota/upload

Upload and install a new firmware binary (OTA update).

**Request:** `multipart/form-data` with field name `firmware`.

**Response `200 OK`:**
```json
{
  "status": "ok",
  "message": "Update successful. Rebooting..."
}
```

**Response `500 Internal Server Error`:**
```json
{
  "status": "error",
  "message": "OTA update failed"
}
```

> On success, the device reboots automatically after a 500ms delay.
> The device uses dual OTA partitions (ota_0 / ota_1) for safe updates.

---

## POST /api/test-connection

Validate a REST endpoint from the device before relying on it. Used by the
"Test connection" buttons on the DNS and DHCP pages. The device performs a
**GET** to the given URL (harmless — a GET writes nothing) using the same
HTTP client settings as the REST log/cache senders: preemptive Basic auth,
`disable_auto_redirect`, `max_authorization_retries=-1`, 5 s timeout.

**Request body:**
```json
{
  "url": "https://dhcpserverweb.lo/api/v1/dns/cache",
  "auth": true,
  "user": "esp32",
  "pass": "esp32"
}
```

**Response `200 OK`:**
```json
{
  "ok": true,
  "http": 404,
  "elapsed_ms": 123,
  "error": ""
}
```

> `ok` is `true` when the HTTP round-trip succeeded (any status code).
> `http` is the response status (0 on transport failure); `error` holds an
> `esp_err_to_name` string on failure (empty on success). The web UI treats a
> 401/403 as an authentication problem and any other code as reachable.

---

## GET /api/settings/export

Full backup of all persisted settings as a single JSON document. Passwords are
**not** exported (web password, REST-log/cache auth passwords) — the matching
`*_auth` booleans are kept so an import knows whether auth is enabled.

**Response `200 OK`:**
```json
{
  "format": "dhcpserver-settings",
  "schema": 1,
  "firmware_version": "01.02.028.00.26.08.RU",
  "dhcp": {
    "enabled": true, "server_ip": "192.168.1.201",
    "start_ip": "192.168.1.100", "end_ip": "192.168.1.200",
    "subnet": "255.255.255.0", "gateway": "192.168.1.1",
    "lease_time": 86400, "log_terminal": false, "log_rest": false,
    "log_url": "", "log_auth": false, "log_auth_user": "",
    "dns_mode": "auto", "dns_address": ""
  },
  "static_bindings": [
    { "mac": "24:0A:C4:01:23:45", "ip": "192.168.1.50", "name": "",
      "gateway": "", "use_gateway": true, "enabled": true, "use_dns": true }
  ],
  "dns": {
    "enabled": true, "external_dns": "192.168.1.1",
    "log_terminal": false, "log_forwarded": true, "log_local": true,
    "log_cache": true, "log_rest_sent": false, "log_rest": false,
    "log_url": "", "log_auth": false, "log_auth_user": "",
    "cache_rest": false, "cache_rest_read": true, "cache_rest_write": true,
    "cache_url": "", "cache_auth": false, "cache_auth_user": ""
  },
  "local_hosts": [
    { "name": "mydevice.local", "ip4": "192.168.1.60", "ip6": "", "enabled": true }
  ],
  "security": { "username": "admin", "max_attempts": 5, "lockout_period": 300 }
}
```

---

## POST /api/settings/import

Restore settings from a JSON document produced by `GET /api/settings/export`.

**Request body:** the export JSON (full or partial document). Content-Length may
be up to ~16 KB.

Import logic:

1. **Format marker** — `"format":"dhcpserver-settings"` is required; otherwise
   `400`/error response.
2. **Version** — `firmware_version` is compared **by release number** (`xxx` of
   `aa.bb.xxx.cc.YY.MM.RR`); sub-release / date / region are ignored.
3. **Recognized fields only** — each section is applied field-by-field from the
   known schema; passwords are never imported (current ones are kept).
4. **Unknown fields** (e.g. a file exported by a **newer** firmware) are not
   applied and are listed in `skipped_fields`.
5. **Apply** — sections are saved to NVS; the DHCP/DNS servers are started or
   stopped to match the imported `enabled` flags. A change of the network
   parameters (`server_ip` / `subnet` / `gateway`) is **not** applied on the fly —
   the response flags `reboot_required`, so a reboot picks up the new static IP.

**Response `200 OK`:**
```json
{
  "status": "ok",
  "firmware_version": "01.02.028.00.26.08.RU",
  "file_version": "01.02.027.00.26.08.RU",
  "version_mismatch": true,
  "file_newer": false,
  "reboot_required": false,
  "imported": {
    "dhcp": true, "static_bindings": true, "dns": true,
    "local_hosts": true, "security": true
  },
  "skipped_fields": ["some_future_field"]
}
```

| Field | Meaning |
|-------|---------|
| `version_mismatch` | imported release ≠ current release |
| `file_newer` | imported file was produced by a newer release |
| `reboot_required` | network params changed — reboot to apply static IP |
| `imported` | which sections were actually found and applied |
| `skipped_fields` | fields present but unknown to this firmware (not imported) |

---

## POST /api/settings/reset

Factory reset: erases the **whole** settings NVS namespace (`dhcp`) and reboots.
On the next boot every setting falls back to its compile-time default —
network, DHCP, DNS, static bindings, local hosts, DNS cache and the web login
(`admin/admin`). The command is authenticated.

**Response `200 OK`** (sent before the device reboots, ~0.7 s later):
```json
{ "status": "ok", "message": "Settings reset to factory defaults. Rebooting...", "reboot": true }
```

**Response `500 Internal Server Error`** if the NVS erase failed:
```json
{ "status": "error", "message": "NVS erase failed" }
```

---

## POST /api/device/reboot

Reboots the device **without** touching any settings. The command is
authenticated. The device restarts ~0.5 s after the response is sent.

**Response `200 OK`:**
```json
{ "status": "ok", "message": "Device is rebooting...", "reboot": true }
```
