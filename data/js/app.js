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

    // Close any open dropdown when clicking elsewhere
    document.addEventListener('click', (e) => {
        if (!e.target.closest('.nav-dropdown')) {
            document.querySelectorAll('.nav-dropdown.open')
                .forEach(d => d.classList.remove('open'));
        }
    });

    // Update status on index page
    if (document.getElementById('wifi-status')) {
        updateStatus();
        // Auto-refresh every 5 s — live bars like Task Manager. Polling too
        // often (2 s) opens many TCP connections and exhausts the httpd
        // socket pool ("httpd_accept_conn: error in accept").
        setInterval(updateStatus, 5000);
    }
});

function setMeter(id, pct) {
    const bar = document.getElementById(id);
    if (!bar) return;
    const v = Math.max(0, Math.min(100, Math.round(pct)));
    bar.style.width = v + '%';
}

async function updateStatus() {
    try {
        const data = await fetchJSON('/api/status');
        document.getElementById('wifi-status').textContent =
            data.wifi_connected ? tr('status.connected') : tr('status.disconnected');
        document.getElementById('wifi-status').className = data.wifi_connected ? 'status-ok' : 'status-err';
        const dhcpEl = document.getElementById('dhcp-status');
        if (dhcpEl) {
            dhcpEl.textContent = data.dhcp_running ? tr('status.running') : tr('status.stopped');
            dhcpEl.className = data.dhcp_running ? 'status-ok' : 'status-err';
        }
        const dnsEl = document.getElementById('dns-status');
        if (dnsEl) {
            dnsEl.textContent = data.dns_running ? tr('status.running') : tr('status.stopped');
            dnsEl.className = data.dns_running ? 'status-ok' : 'status-err';
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
        if (ramTotalEl) ramTotalEl.textContent = (ramTotal / 1024).toFixed(0) + ' KB';
        const freeRamEl = document.getElementById('free-ram');
        if (freeRamEl) freeRamEl.textContent = (ramFree / 1024).toFixed(0) + ' KB free';
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
                if (psramTotalEl) psramTotalEl.textContent = (psramTotal / 1024).toFixed(0) + ' KB';
                const freePsramEl = document.getElementById('free-psram');
                if (freePsramEl) freePsramEl.textContent = (psramFree / 1024).toFixed(0) + ' KB free';
                if (document.getElementById('psram-bar')) {
                    setMeter('psram-bar', (1 - psramFree / psramTotal) * 100);
                }
            } else {
                psramEl.style.display = 'none';
            }
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
    } catch (e) {
        console.warn('Status update failed:', e);
    }
}
