/**
 * DHCPServer — Web Interface SPA
 * Handles: i18n, floating emojis, auth, REST API calls
 */

/* ─── Configuration ─────────────────────────────────── */

const CONFIG = {
    BASE_URL: window.location.origin,
    AUTH: null, // set after login
};

/* ─── Session Auth ──────────────────────────────────── */

// Restore auth from session storage
(function() {
    const saved = sessionStorage.getItem('dhcp_auth');
    if (saved) {
        try {
            const { user, pass } = JSON.parse(saved);
            CONFIG.AUTH = btoa(user + ':' + pass);
        } catch(e) {}
    }
})();

// Redirect to login if not authenticated (skip for login page itself)
(function() {
    const isLoginPage = window.location.pathname === '/login.html' ||
                        window.location.pathname === '/';
    if (!CONFIG.AUTH && !isLoginPage) {
        // Absolute path — relative 'login.html' on a subpage resolves to
        // /pages/login.html and 404s.
        window.location.href = '/login.html';
    }
})();

/* ─── Floating Emojis ──────────────────────────────── */

const EMOJIS = ['🌐', '📡', '🔒', '⚡', '🖥️', '🔧', '📶', '🌍', '🚀', '💻'];

function createFloatingEmojis() {
    const container = document.createElement('div');
    container.className = 'emoji-container';
    document.body.prepend(container);

    for (let i = 0; i < 20; i++) {
        const emoji = document.createElement('span');
        emoji.className = 'emoji';
        emoji.textContent = EMOJIS[i % EMOJIS.length];
        emoji.style.left = Math.random() * 100 + '%';
        emoji.style.fontSize = (1.2 + Math.random() * 1.8) + 'rem';
        emoji.style.animationDuration = (15 + Math.random() * 25) + 's';
        emoji.style.animationDelay = (Math.random() * 20) + 's';
        container.appendChild(emoji);
    }
}

/* ─── i18n ──────────────────────────────────────────── */

let currentLang = localStorage.getItem('lang') || 'ru';
let translations = {};

async function loadTranslations(lang) {
    try {
        const resp = await fetch(`/i18n/${lang}.json`);
        translations = await resp.json();
        applyTranslations();
    } catch (e) {
        console.warn('Failed to load translations:', e);
    }
}

function applyTranslations() {
    document.querySelectorAll('[data-i18n]').forEach(el => {
        const key = el.dataset.i18n;
        const val = key.split('.').reduce((o, k) => (o && o[k] !== undefined) ? o[k] : null, translations);
        if (val) {
            if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
                el.placeholder = val;
            } else {
                el.textContent = val;
            }
        }
    });
    document.documentElement.lang = currentLang;
    // Let pages re-render JS-generated (non data-i18n) text on language change
    // (e.g. <option> labels built at runtime).
    document.dispatchEvent(new CustomEvent('i18n-applied'));
}

function setLanguage(lang) {
    currentLang = lang;
    localStorage.setItem('lang', lang);
    loadTranslations(lang);
}

/* Look up a translation key ("dns.test_ok") in the loaded dictionary.
   Falls back to the key itself if missing. Used by JS-generated text. */
function tr(key) {
    const val = key.split('.').reduce((o, k) => (o && o[k] !== undefined) ? o[k] : null, translations);
    return val !== null && val !== undefined ? val : key;
}

/* ─── DNS partial-save helper ────────────────────────
   The DNS settings are ONE object on the server, but the DNS section is split
   into several sub-pages, each owning a subset of the fields. Saving must send
   the FULL object (the backend replaces it), so fetch the current persisted
   settings and override with the page-owned fields. */
async function saveDnsPartial(localFields) {
    const server = await fetchJSON('/api/dns/settings');
    return postJSON('/api/dns/settings', { ...server, ...localFields });
}

/* Same helper for the DHCP settings — one object on the server, but the DHCP
   section is split into sub-pages, each owning a subset of the fields. */
async function saveDhcpPartial(localFields) {
    const server = await fetchJSON('/api/dhcp/settings');
    return postJSON('/api/dhcp/settings', { ...server, ...localFields });
}

/* Same helper for the Time (NTP) settings — one object on the server, but the
   Time section is split into two sub-pages (General / Logging), each owning a
   subset of the fields. */
async function saveTimePartial(localFields) {
    const server = await fetchJSON('/api/time/settings');
    return postJSON('/api/time/settings', { ...server, ...localFields });
}

/* Run a "Test connection" for a DNS/DHCP REST block. The test runs ENTIRELY on
   the MCU (POST /api/test-connection): the MCU reads the real NVS settings,
   performs the request and returns {ok, http, elapsed_ms, error}. The browser
   only sends a "target" and shows the result; if the current form differs from
   the saved (NVS) snapshot, a warning is shown. */
async function testConnection(resultId, target, saved, form, ns) {
    const el = document.getElementById(resultId);
    if (!el) return;

    // Unsaved edits in the form? Warn (the test still uses the NVS values the
    // MCU reads).
    const dirty = saved && (
        form.url !== saved.url ||
        !!form.auth !== !!saved.auth ||
        form.user !== saved.user ||
        form.pass !== saved.pass);

    el.textContent = dirty ? tr(ns + '.test_not_saved') : tr(ns + '.test_checking');
    el.className = 'test-conn-result ' + (dirty ? 'test-conn-warn' : '');
    try {
        const r = await postJSON('/api/test-connection', { target });
        const prefix = dirty ? tr(ns + '.test_not_saved') + ' — ' : '';
        if (r.ok) {
            el.textContent = prefix + tr(ns + '.test_ok') + ' (HTTP ' + r.http + ', ' + r.elapsed_ms + ' ms)';
            el.className = 'test-conn-result ' + (dirty ? 'test-conn-warn' : 'test-conn-ok');
        } else {
            el.textContent = prefix + tr(ns + '.test_fail') + ' (HTTP ' + r.http + (r.error ? ': ' + r.error : '') + ')';
            el.className = 'test-conn-result ' + (dirty ? 'test-conn-warn' : 'test-conn-fail');
        }
    } catch (e) {
        el.textContent = (dirty ? tr(ns + '.test_not_saved') + ' — ' : '') +
                         tr(ns + '.test_fail') + ': ' + e;
        el.className = 'test-conn-result ' + (dirty ? 'test-conn-warn' : 'test-conn-fail');
    }
}

/* ─── HTTP Basic Auth ──────────────────────────────── */

function setAuth(username, password) {
    CONFIG.AUTH = btoa(username + ':' + password);
}

function getAuthHeaders() {
    return CONFIG.AUTH ? { 'Authorization': 'Basic ' + CONFIG.AUTH } : {};
}

async function apiFetch(url, options = {}) {
    const headers = { ...getAuthHeaders(), ...options.headers };
    const resp = await fetch(url, { ...options, headers });
    if (resp.status === 401) {
        // Session expired or not logged in — redirect to login
        sessionStorage.removeItem('dhcp_auth');
        window.location.href = 'login.html';
    }
    return resp;
}

/* ─── Helpers ──────────────────────────────────────── */

async function fetchJSON(url) {
    const resp = await apiFetch(url);
    return resp.json();
}

async function postJSON(url, data) {
    const resp = await apiFetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data),
    });
    return resp.json();
}

/* Show/hide the "Files" nav entry according to the build's capability.
   Called once per page load (the flag is a build property, not a setting). */
async function applyFilesNavVisibility() {
    const item = document.getElementById('nav-files');
    if (!item) return;

    try {
        const status = await fetchJSON('/api/status');
        item.style.display = status.files_enabled ? 'block' : 'none';
    } catch (e) {
        console.warn('files_enabled check failed:', e);
    }
}

/* Toggle the mobile hamburger menu */
function toggleNav() {
    const navbar = document.querySelector('.navbar');
    if (navbar) navbar.classList.toggle('nav-open');
}

/* Toggle a navbar dropdown (e.g. Help menu) */
function toggleDropdown(event) {
    event.preventDefault();
    event.stopPropagation();
    const dd = event.currentTarget.closest('.nav-dropdown');
    if (!dd) return;
    const wasOpen = dd.classList.contains('open');
    document.querySelectorAll('.nav-dropdown.open')
        .forEach(d => d.classList.remove('open'));
    if (!wasOpen) dd.classList.add('open');
}

/* ─── Page Load Handler ────────────────────────────── */

document.addEventListener('DOMContentLoaded', async () => {
    createFloatingEmojis();
    await loadTranslations(currentLang);

    // Load header & footer
    const headerResp = await fetch('/header.html');
    const headerHtml = await headerResp.text();
    document.getElementById('header-placeholder')?.insertAdjacentHTML('afterbegin', headerHtml);

    const footerResp = await fetch('/footer.html');
    const footerHtml = await footerResp.text();
    document.getElementById('footer-placeholder')?.insertAdjacentHTML('afterbegin', footerHtml);

    // Re-apply translations after dynamic content
    applyTranslations();

    // The Files explorer needs FAT volumes — present only on the ESP32-P4
    // build (flash data partition + microSD). /api/status reports that as
    // `files_enabled`, and the nav entry stays hidden until we know, so a
    // classic ESP32 never shows a dead menu item.
    await applyFilesNavVisibility();

    // Close any open dropdown when clicking elsewhere
    document.addEventListener('click', (e) => {
        if (!e.target.closest('.nav-dropdown')) {
            document.querySelectorAll('.nav-dropdown.open')
                .forEach(d => d.classList.remove('open'));
        }
    });

    // Update status on index page
    if (document.getElementById('dhcp-status')) {
        // Two requests per poll (status + time), but issued SEQUENTIALLY —
        // polling too often or in parallel opens many TCP connections and
        // exhausts the httpd socket pool ("httpd_accept_conn: error in accept").
        const poll = async () => {
            await updateStatus();
            await updateDeviceTime();
        };
        poll();
        // Auto-refresh every 5 s — live bars like Task Manager.
        setInterval(poll, 5000);
    }
});

function setMeter(id, pct) {
    const bar = document.getElementById(id);
    if (!bar) return;
    const v = Math.max(0, Math.min(100, Math.round(pct)));
    bar.style.width = v + '%';
}

/* Byte sizes on the status page: kilobytes for the on-chip memories, megabytes
   or gigabytes for the FAT volumes (21 MB partition / multi-GB cards), so a
   single helper covers both RAM and storage rows. */
function formatBytes(bytes) {
    const b = Number(bytes) || 0;
    if (b < 1024) return b + ' B';
    const kb = b / 1024;
    if (kb < 1024) return kb.toFixed(0) + ' KB';
    const mb = kb / 1024;
    if (mb < 1024) return mb.toFixed(mb < 10 ? 1 : 0) + ' MB';
    return (mb / 1024).toFixed(2) + ' GB';
}

/* Device time for DISPLAY: the API/browser fields use the ISO-ish wire format
   "YYYY-MM-DD HH:MM:SS", but the clock readouts are shown day-first and without
   seconds — "DD-MM-YYYY HH:MM" (the pages poll every 5 s, so seconds would only
   tick in jumps). Anything that does not parse as the wire format is returned
   untouched, so an unset/unsynchronised stamp never renders as garbage. */
function formatDeviceTimeDisplay(stamp) {
    if (!stamp) return '';
    const m = /^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})/.exec(stamp);
    if (!m) return stamp;
    return m[3] + '-' + m[2] + '-' + m[1] + ' ' + m[4] + ':' + m[5];
}

/* Device clock row on the main page. Local time (the timezone selected on the
   Time Server page) plus a "not synchronized" note on its own line below the
   value while the clock has never been synchronised (and was not set by hand
   either). */
async function updateDeviceTime() {
    const el = document.getElementById('device-time');
    if (!el) return;
    try {
        const data = await fetchJSON('/api/time/now');
        el.textContent = formatDeviceTimeDisplay(data.now_local || data.now_utc) || '--';
        el.className = 'status-value';
        if (!data.synced) {
            const note = document.createElement('span');
            note.className = 'unsynced-note';
            note.textContent = tr('app.time_unsynced');
            el.appendChild(note);
        }
    } catch (e) {
        /* ignore — the next poll retries */
    }
}

async function updateStatus() {
    try {
        const data = await fetchJSON('/api/status');

        // Uptime next to the page title: one unit, the largest that fits —
        // seconds, then minutes, hours and days — because a raw second count
        // would be noise in the header.
        const uptimeEl = document.getElementById('uptime');
        if (uptimeEl && data.uptime_sec != null) {
            const sec = Math.max(0, Math.floor(Number(data.uptime_sec) || 0));
            const days = Math.floor(sec / 86400);
            const hours = Math.floor(sec / 3600);
            const mins = Math.floor(sec / 60);
            const value = days >= 1 ? days + ' ' + tr('app.uptime_days')
                        : hours >= 1 ? hours + ' ' + tr('app.uptime_hours')
                        : mins >= 1 ? mins + ' ' + tr('app.uptime_min')
                        : sec + ' ' + tr('app.uptime_sec');
            uptimeEl.textContent = tr('app.uptime') + ' ' + value;
        }
        const dhcpEl = document.getElementById('dhcp-status');
        if (dhcpEl) {
            dhcpEl.textContent = data.dhcp_running ? tr('status.running') : tr('status.stopped');
            // Keep `status-value` (right alignment + weight) — assigning only the
            // colour class dropped it and made the value jump to the left after
            // the first poll.
            dhcpEl.className = 'status-value ' + (data.dhcp_running ? 'status-ok' : 'status-err');
        }
        const dnsEl = document.getElementById('dns-status');
        if (dnsEl) {
            dnsEl.textContent = data.dns_running ? tr('status.running') : tr('status.stopped');
            dnsEl.className = 'status-value ' + (data.dns_running ? 'status-ok' : 'status-err');
        }
        const ntpEl = document.getElementById('ntp-status');
        if (ntpEl) {
            ntpEl.textContent = data.ntp_running ? tr('status.running') : tr('status.stopped');
            ntpEl.className = 'status-value ' + (data.ntp_running ? 'status-ok' : 'status-err');
        }
        // CPU per-core bars
        const pct0 = data.cpu_load0 != null ? data.cpu_load0 : 0;
        const pct1 = data.cpu_load1 != null ? data.cpu_load1 : 0;
        setMeter('cpu-bar0', pct0);
        setMeter('cpu-bar1', pct1);
        const p0 = document.getElementById('cpu-pct0');
        const p1 = document.getElementById('cpu-pct1');
        if (p0) p0.textContent = Math.round(pct0) + '%';
        if (p1) p1.textContent = Math.round(pct1) + '%';
        // Memory meters — internal RAM always; external PSRAM when present.
        // For internal RAM, total = physical on-chip SRAM (ram_total, e.g.
        // 768 KB on ESP32-P4); free comes from the managed heap, so the fill
        // bar reflects true chip utilisation (used = total - free heap).
        const ramTotal = data.ram_total != null ? data.ram_total
            : (data.heap_total != null ? data.heap_total : 320 * 1024);
        const ramFree = data.heap_free != null ? data.heap_free : 0;
        const ramTotalEl = document.getElementById('ram-total');
        if (ramTotalEl) ramTotalEl.textContent = formatBytes(ramTotal);
        const freeRamEl = document.getElementById('free-ram');
        if (freeRamEl) freeRamEl.textContent = formatBytes(ramFree) + ' ' + tr('app.free_short');
        if (document.getElementById('ram-bar')) {
            setMeter('ram-bar', ramTotal > 0 ? (1 - ramFree / ramTotal) * 100 : 0);
        }
        const psramEl = document.getElementById('psram-meter');
        if (psramEl) {
            const psramTotal = data.psram_total != null ? data.psram_total : 0;
            const psramFree = data.psram_free != null ? data.psram_free : 0;
            if (psramTotal > 0) {
                psramEl.style.display = '';
                const psramTotalEl = document.getElementById('psram-total');
                if (psramTotalEl) psramTotalEl.textContent = formatBytes(psramTotal);
                const freePsramEl = document.getElementById('free-psram');
                if (freePsramEl) freePsramEl.textContent = formatBytes(psramFree) + ' ' + tr('app.free_short');
                if (document.getElementById('psram-bar')) {
                    setMeter('psram-bar', (1 - psramFree / psramTotal) * 100);
                }
            } else {
                psramEl.style.display = 'none';
            }
        }
        // Storage meters — the internal FAT partition and the microSD card.
        // The data comes from `/api/status` (`volumes`), which is the same
        // array the file explorer serves; an older firmware has no such field,
        // so the row simply stays hidden until one arrives.
        const storageRow = document.getElementById('storage-row');
        if (storageRow) {
            const vols = Array.isArray(data.volumes) ? data.volumes : [];
            const fat = vols.find(v => v.id === 'fat');
            const sd = vols.find(v => v.id === 'sd');
            storageRow.style.display = (fat || sd) ? '' : 'none';

            const renderVolume = (vol, labelId, barId, totalId, freeId) => {
                const labelEl = document.getElementById(labelId);
                const totalEl = document.getElementById(totalId);
                const freeEl = document.getElementById(freeId);
                // The label column is narrow, so the mount point lives in the
                // tooltip rather than in the text.
                if (labelEl) labelEl.title = vol && vol.mount_point ? vol.mount_point : '';
                if (!vol) {
                    if (totalEl) totalEl.textContent = '--';
                    if (freeEl) freeEl.textContent = '';
                    setMeter(barId, 0);
                    return;
                }
                // Nothing to measure on an unusable volume: say so instead of
                // drawing a zero-capacity bar. "Not mounted" is deliberate —
                // with no card-detect line the firmware cannot tell an empty
                // slot from a card it failed to mount. The driver's own error
                // stays in the tooltip only: a line like "mount failed (4-bit):
                // ESP_ERR_TIMEOUT; mount failed (1-bit): …" is a diagnostic, not
                // something to read on the home page (the Files page still
                // shows it next to the volume).
                const mounted = !!vol.mounted && vol.total_bytes > 0;
                if (totalEl) {
                    totalEl.textContent = mounted ? formatBytes(vol.total_bytes) : tr('app.not_mounted');
                    totalEl.title = vol.error || '';
                }
                if (freeEl) {
                    freeEl.textContent = mounted
                        ? formatBytes(vol.free_bytes) + ' ' + tr('app.free_short') : '';
                }
                setMeter(barId, mounted ? (1 - vol.free_bytes / vol.total_bytes) * 100 : 0);
            };

            renderVolume(fat, 'fat-label', 'fat-bar', 'fat-total', 'fat-free');
            renderVolume(sd, 'sd-label', 'sd-bar', 'sd-total', 'sd-free');
        }
        // Static bindings NVS storage usage
        const sbEl = document.getElementById('static-bindings-usage');
        if (sbEl) {
            if (data.static_bindings_max) {
                sbEl.textContent = data.static_bindings_used + ' / ' +
                                   data.static_bindings_max + ' B';
            } else {
                sbEl.textContent = '--';
            }
        }
        // Local DNS hosts NVS storage usage
        const lhEl = document.getElementById('local-hosts-usage');
        if (lhEl) {
            if (data.local_hosts_max) {
                lhEl.textContent = data.local_hosts_used + ' / ' +
                                   data.local_hosts_max + ' B';
            } else {
                lhEl.textContent = '--';
            }
        }
        // Built-in DNS cache (PSRAM) — used/free MB + entries + counters
        const icRow = document.getElementById('internal-cache-row');
        if (icRow) {
            if (!data.internal_cache_available) {
                icRow.style.display = 'none';
            } else {
                icRow.style.display = '';
                const MB = 1048576;
                const used = data.internal_cache_used_bytes != null ? data.internal_cache_used_bytes : 0;
                const free = data.internal_cache_free_bytes != null ? data.internal_cache_free_bytes : 0;
                const entries = data.internal_cache_entries != null ? data.internal_cache_entries : 0;
                const capacity = data.internal_cache_capacity != null ? data.internal_cache_capacity : 0;
                const hits = data.internal_cache_hits != null ? data.internal_cache_hits : 0;
                const fwd = data.internal_forward_count != null ? data.internal_forward_count : 0;
                const avgUs = data.internal_cache_avg_hit_us != null ? data.internal_cache_avg_hit_us : 0;
                const mbFmt = (bytes) => {
                    const m = bytes / MB;
                    if (m > 0 && m < 0.01) return (bytes / 1024).toFixed(0) + ' KB';
                    return m.toFixed(2) + ' MB';
                };
                const usFmt = (us) => {
                    if (us < 1000) return us + 'µs';
                    return (us / 1000).toFixed(1) + 'ms';
                };
                const usedEl = document.getElementById('internal-cache-used');
                if (usedEl) usedEl.textContent = mbFmt(used);
                const freeEl = document.getElementById('internal-cache-free');
                if (freeEl) freeEl.textContent = mbFmt(free) + ' free';
                const pct = (used + free) > 0 ? (used / (used + free)) * 100 : 0;
                setMeter('internal-cache-bar', pct);
                const statsEl = document.getElementById('internal-cache-stats');
                if (statsEl) {
                    let s =
                        tr('app.ic_entries') + ' ' + entries + ' / ' + capacity +
                        '   ·   ' + tr('app.ic_hits') + ' ' + hits +
                        '   ·   ' + tr('app.ic_forward') + ' ' + fwd;
                    if (avgUs > 0) {
                        s += '   ·   ' + tr('app.ic_avg') + ' ' + usFmt(avgUs);
                    }
                    statsEl.textContent = s;
                }
            }
        }
    } catch (e) {
        console.warn('Status update failed:', e);
    }
}
