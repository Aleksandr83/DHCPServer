/**
 * DHCPServer — Web Interface SPA
 * Handles: i18n, floating emojis, auth, REST API calls
 */

/* ─── Configuration ─────────────────────────────────── */

const CONFIG = {
    BASE_URL: window.location.origin,
    AUTH: null, // set after login
};

/* ─── Numbers the interface works with (rule 39) ────── */

// How often the status page refreshes. The 500 ms of the restart flow is a
// different timer and lives with the code that uses it.
const kStatusPollMs = 5000;
const kPercentScale = 100;
const kBytesPerKib = 1024;
const kBytesPerMib = 1024 * 1024;
const kSmallValueRatio = 0.01;   // below this many MB a value is shown in KB
const kUsPerMs = 1000;

// The modal input is shared by every prompt of the interface. The firmware
// truncates a client name at 32 bytes (DhcpClientName::kMaxLen) and stores 20
// in the allow-list, so a longer text typed here is cut by the device — the
// limit is kept at 128 on purpose, decided on 19.09.2026, not overlooked.
const kNameInputMaxLen = 128;

// Used only when the device reports no memory total at all. The firmware does
// report one (768 KiB of internal SRAM on the P4, CpuMonitor::kSramWindowKib);
// this older, smaller figure is kept as it is on purpose, same decision.
const kFallbackRamTotalBytes = 320 * 1024;

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
        // Session expired or not logged in — redirect to login. Absolute path:
        // the pages under /pages/ would otherwise resolve 'login.html' into
        // /pages/login.html, which does not exist — the operator sees a 404
        // instead of the login form (the same trap the auth gate below
        // already documents).
        sessionStorage.removeItem('dhcp_auth');
        window.location.href = '/login.html';
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

/* Wait for something without blocking the page — used by the dialogs and
   by the restart preparation, which must let a message be read before the
   next one replaces it. */
const sleep = ms => new Promise(r => setTimeout(r, ms));

/* ─── In-page modal dialog ─────────────────────────── */

/* Native window.prompt()/confirm() are blocked in some embedded browsers, and a
   browser dialog cannot be styled, translated or screenshotted — so the UI asks
   its questions here. This used to live in the file explorer only; it now serves
   every page that needs an answer, which is why it took the explorer's exact
   semantics (and its callers were left untouched).

   Resolves with the typed value (input mode), `true` (confirm mode), 'extra'
   (the optional third button) or null when the operator cancels or closes it.

   A question and a message are not the same thing: a message has exactly one
   answer and no way to say "no", so `hideCancel` leaves the dialog with a
   single button. That is what replaced the browser's own `alert()`, which
   cannot be translated, styled or captured in a screenshot. */
let uiDialogResolve = null;

function uiDialog() {
    let modal = document.getElementById('ui-modal');
    if (modal) return modal;

    modal = document.createElement('div');
    modal.className = 'modal-backdrop';
    modal.id = 'ui-modal';
    modal.style.display = 'none';
    modal.innerHTML =
        '<div class="modal-card" role="dialog" aria-modal="true">' +
        '<h3 id="ui-modal-title"></h3>' +
        '<p class="hint" id="ui-modal-hint" style="display:none;"></p>' +
        '<input type="text" id="ui-modal-input" style="display:none;" maxlength="' +
        kNameInputMaxLen + '">' +
        '<div class="modal-actions">' +
        '<button class="btn" id="ui-modal-cancel"></button>' +
        '<button class="btn" id="ui-modal-extra" style="display:none;"></button>' +
        '<button class="btn btn-primary" id="ui-modal-ok"></button>' +
        '</div></div>';
    document.body.appendChild(modal);

    document.getElementById('ui-modal-ok').onclick = () => {
        const input = document.getElementById('ui-modal-input');
        closeDialog(input.style.display === 'none' ? true : input.value.trim());
    };
    document.getElementById('ui-modal-extra').onclick = () => closeDialog('extra');
    document.getElementById('ui-modal-cancel').onclick = () => closeDialog(null);
    document.getElementById('ui-modal-input').onkeydown = (e) => {
        if (e.key === 'Enter') document.getElementById('ui-modal-ok').click();
        if (e.key === 'Escape') closeDialog(null);
    };
    document.addEventListener('keydown', (e) => {
        // Escape anywhere in the dialog closes it, not just in the input field.
        if (e.key === 'Escape' && modal.style.display !== 'none') closeDialog(null);
    });
    return modal;
}

function closeDialog(result) {
    const modal = document.getElementById('ui-modal');
    if (modal) modal.style.display = 'none';
    const resolve = uiDialogResolve;
    uiDialogResolve = null;
    if (resolve) resolve(result);
}

function showDialog({ title, hint, value, okLabel, cancelLabel, danger,
                      withInput, extraLabel, hideCancel }) {
    const modal = uiDialog();
    const input = document.getElementById('ui-modal-input');
    const hintEl = document.getElementById('ui-modal-hint');
    const ok = document.getElementById('ui-modal-ok');
    const extra = document.getElementById('ui-modal-extra');

    document.getElementById('ui-modal-title').textContent = title;
    hintEl.textContent = hint || '';
    hintEl.style.display = hint ? '' : 'none';

    input.style.display = withInput ? '' : 'none';
    input.value = withInput ? (value || '') : '';

    ok.textContent = okLabel || tr('common.ok');
    ok.className = 'btn ' + (danger ? 'btn-danger' : 'btn-primary');
    const cancel = document.getElementById('ui-modal-cancel');
    cancel.textContent = cancelLabel || tr('common.cancel');
    // Not every dialog is a question: a message must not offer a choice it does
    // not have, so its Cancel button is simply not drawn.
    cancel.style.display = hideCancel ? 'none' : '';
    extra.textContent = extraLabel || '';
    extra.style.display = extraLabel ? '' : 'none';

    modal.style.display = 'flex';
    if (withInput) {
        input.focus();
        input.select();
    } else {
        ok.focus();
    }
    return new Promise(resolve => { uiDialogResolve = resolve; });
}

/* ─── Preparing a planned restart ─────────────────── */

/* A restart also writes what the "before reboot" switches ask for — the
   statistics file and the cache. That must not happen silently: the cache is
   megabytes, and a restart that *looks* instant while the device is still
   writing is exactly what the operator cannot see. So the page asks the device
   what has to be done (`POST /api/device/reboot/prepare`), shows every step,
   waits for the cache job through the very endpoint the Internal Cache page
   uses, and asks before losing the cache.

   `onStep(text, cls)` draws one line; the flow guarantees that a message has
   been on screen long enough to be read before the next one replaces it. It
   resolves with

     'ready'     — everything the switches asked for is written, so a restart may
                   be told `{ "saved": true }`;
     'doubt'     — no verdict (a device that does not report one, or a job that
                   stopped moving): the device writes the file itself during the
                   restart, which is the older and slower path;
     'failed'    — the write failed and the operator chose to restart anyway;
     'cancelled' — the write failed and the operator cancelled: nothing was done.

   The Device Management page and the Version page (before a firmware upload)
   both run exactly this. They are not the same action, though, so the flow takes
   the action as a parameter (`'reboot'` by default, `'update'` on the Version
   page): the question about a failed write has to offer "update anyway" there and
   "reboot anyway" here, and the operator who is updating firmware must not be
   asked whether to reboot — he is not rebooting, the update is. */
async function prepareRestartFlow(onStep, action) {
    const isUpdate = action === 'update';
    const kStepMinMs = 900;       // no message may flash by unread
    const kProgressMinMs = 300;   // ...and a progress line that ends at once
    const kPollMs = 500;
    const kStallLimitMs = 10000;  // no progress for this long -> carry on anyway

    let stepShownAt = 0;
    const step = (text, cls) => {
        onStep(text, cls);
        stepShownAt = Date.now();
    };

    // A step that finishes in a millisecond is a step nobody can read — the
    // cache of a freshly started device is empty, for instance. So a message
    // stays on screen for at least minMs before the next one replaces it.
    async function lingerStep(minMs) {
        const left = (minMs || kStepMinMs) - (Date.now() - stepShownAt);
        if (left > 0) await sleep(left);
    }

    // Wait for the background cache save to end. Returns the last progress
    // report, or null when it stopped moving — then the device's own guard
    // (kPersistStallMs) takes over during the restart, so nothing is lost, only
    // unshown.
    async function waitForCacheSave() {
        let lastDone = -1, stalledMs = 0, shownProgress = false;
        for (;;) {
            let p = null;
            try { p = await fetchJSON('/api/dns/internal-cache/progress'); } catch (e) { p = null; }
            if (p && !p.busy) {
                // Nothing to announce when the job was over before the first
                // question; but a progress line that did appear must not blink.
                if (shownProgress) await lingerStep(kProgressMinMs);
                return p;
            }

            const done = p ? (p.done || 0) : 0;
            const total = p ? (p.total || 0) : 0;
            const pct = total > 0 ? Math.round(done * kPercentScale / total) : null;
            step(tr('restart.cache_saving') +
                 (pct === null ? '' : ' ' + pct + '%') +
                 (total > 0 ? ' (' + done + ' / ' + total + ')' : ''));
            shownProgress = true;

            if (done !== lastDone) { lastDone = done; stalledMs = 0; }
            else { stalledMs += kPollMs; }
            if (stalledMs >= kStallLimitMs) {
                step(tr('restart.cache_stalled'), 'status-warn');
                return null;
            }
            await sleep(kPollMs);
        }
    }

    // Wait for the background statistics write the device started (stage 122:
    // it runs as a job of its own, like the cache). No percentage — 92 bytes
    // have none, and inventing one would be worse than the plain word. Returns
    // `{result, detail}` ('ok' | 'skipped' | 'failed' | '' when none arrived,
    // plus the device's own words about a failure), or null when the job stopped
    // answering.
    async function waitForStatsSave() {
        let stalledMs = 0;
        for (;;) {
            let p = null;
            try { p = await fetchJSON('/api/dns/stats/progress'); } catch (e) { p = null; }
            if (p && !p.busy) {
                return { result: p.last_result || '', detail: p.last_detail || '' };
            }
            stalledMs += kPollMs;
            if (stalledMs >= kStallLimitMs) {
                step(tr('restart.stats_stalled'), 'status-warn');
                return null;
            }
            await sleep(kPollMs);
        }
    }

    // One question for every way the files can fail to reach the card: the
    // statistics, the cache, a cache save that never moved. The operator asked
    // for exactly this rule — "if the save failed, ask whether to reboot" — and
    // it applies to both files, not only to the cache. Asking costs one click;
    // carrying on on its own costs him the data he was told would be kept.
    //
    // The **action** is the other half of the same rule: the question must name
    // what the operator is about to do. On the Version page that is an update, and
    // asking him whether to "reboot anyway" there is simply wrong.
    //
    // @param detail the device's own words about the failure, when it gave any:
    //        the operator has no serial console, so this dialog is the only place
    //        he can read why ("cannot publish the file").
    async function askBeforeLosingData(what, detail) {
        const cache = what === 'cache';
        const hintKey = cache ? (isUpdate ? 'restart.cache_failed_hint_update'
                                          : 'restart.cache_failed_hint')
                              : (isUpdate ? 'restart.stats_failed_hint_update'
                                          : 'restart.stats_failed_hint');
        let hint = tr(hintKey);
        if (detail) hint += '\n\n' + tr('restart.device_detail') + ': ' + detail;
        const anyway = await showDialog({
            title: tr(cache ? 'restart.cache_failed' : 'restart.stats_failed'),
            hint: hint,
            okLabel: tr(isUpdate ? 'restart.update_anyway' : 'restart.reboot_anyway'),
            cancelLabel: tr(isUpdate ? 'restart.cancel_update' : 'restart.cancel_reboot'),
            danger: true,
        });
        if (!anyway) {
            const cancelKey = cache ? (isUpdate ? 'restart.cancelled_update'
                                                : 'restart.cancelled')
                                    : (isUpdate ? 'restart.cancelled_stats_update'
                                                : 'restart.cancelled_stats');
            step(tr(cancelKey), 'status-warn');
            await lingerStep();
            return 'cancelled';
        }
        return 'failed';
    }

    // True when the statistics file was asked for but no verdict about it ever
    // arrived. It is not a failure, but it must not be reported to the device as
    // "already saved" either: `saved: true` tells it to skip **both** files, and
    // the statistics would then be lost for a reason the page itself called
    // unknown. So the flag waits for a verdict on everything the switches asked
    // for, and anything unknown keeps the device's own write alive (the older,
    // slower path — never a lost file).
    let statsUnknown = false;

    let p;
    step(tr('restart.stats_saving'));
    try {
        p = await postJSON('/api/device/reboot/prepare', {});
    } catch (e) {
        // The device did not answer at all — a firmware without
        // `/api/device/reboot/prepare` answers exactly like this. Nothing about
        // the two files can be shown then, so this line is the last message
        // before the reboot takes over: it must be readable. It used to be held
        // for 300 ms, which is why an old firmware looked like a broken page.
        await lingerStep(kProgressMinMs);
        step(tr('restart.prep_unavailable'), 'status-warn');
        await lingerStep();
        return 'doubt';
    }
    // The statistics file is 92 bytes, so this step is over before it can be
    // read — hold its announcement for a moment anyway.
    await lingerStep(kProgressMinMs);

    if (p.stats === 'saved') {
        // An older device writes the file inside the request itself: by the time
        // this answer is here, the file is on the card.
        step(tr('restart.stats_saved'));
        await lingerStep();
    } else if (p.stats === 'started' || p.stats === 'busy') {
        // The write runs as a background job now — wait for its verdict instead
        // of assuming it. ('busy' = one was already running, e.g. a second press
        // of the button; either way it is that job's verdict we report.)
        const statsOutcome = await waitForStatsSave();
        if (statsOutcome === null) {
            // It stopped moving: the file's state is unknown, so the operator
            // decides, not the page.
            await lingerStep();
            return askBeforeLosingData('stats');
        }
        const verdict = statsOutcome.result;
        if (verdict === 'ok') {
            step(tr('restart.stats_saved'), 'status-ok');
            await lingerStep();
        } else if (verdict === 'skipped') {
            step(tr('restart.stats_not_saved'), 'status-warn');
            await lingerStep();
        } else if (verdict === 'failed') {
            step(tr('restart.stats_failed'), 'status-err');
            await lingerStep();
            return askBeforeLosingData('stats', statsOutcome.detail);
        } else {
            // No verdict at all: the job never reported one (the device restarted
            // under it, or the endpoint is not there). A missing answer is not a
            // failure and not a success — say exactly that, and send no `saved`
            // flag, so the device writes the file itself during the restart.
            step(tr('restart.stats_no_verdict'), 'status-warn');
            statsUnknown = true;
            await lingerStep();
        }
    } else if (p.stats === 'skipped') {
        // The switch is off. Say so rather than stay silent: an absent step is
        // indistinguishable from a broken feature (stage 120).
        step(tr('restart.stats_not_saved'), 'status-warn');
        await lingerStep();
    } else if (p.stats === 'failed') {
        // The statistics file could not be written *now* — and this page is the
        // only place that knows it. It used to print the line and let the
        // caller reboot, which is exactly what the operator objected to: a
        // failure message followed by a restart nobody agreed to.
        step(tr('restart.stats_failed'), 'status-err');
        await lingerStep();     // readable before the question opens
        return askBeforeLosingData('stats');
    }

    if (p.cache === 'skipped') {
        // The device refuses to write the cache: "save the cache before a
        // reboot" is off, or the internal cache itself is disabled. Say it
        // rather than stay silent.
        step(tr('restart.cache_not_saved'), 'status-warn');
        await lingerStep();
        return statsUnknown ? 'doubt' : 'ready';
    }
    if (p.cache === 'empty') {
        // Nothing live to write — a freshly started device, or one whose entries
        // have all expired. Normal, and the restart has nothing to do either.
        step(tr('restart.cache_empty'));
        await lingerStep();
        return statsUnknown ? 'doubt' : 'ready';
    }
    if (p.cache === 'failed') {
        // The save could not even be started, so this restart will not write the
        // file either: the cache is lost unless the operator says otherwise.
        step(tr('restart.cache_failed'), 'status-err');
        await lingerStep();
        return askBeforeLosingData('cache');
    }

    // 'started' (we just started it) or 'busy' (a save was already running — a
    // manual one, or the boot restore): either way wait for it.
    const last = await waitForCacheSave();
    if (last === null) {
        // The job stopped moving and waitForCacheSave() has already said so. The
        // file's state is unknown, so the page has no business deciding to
        // restart anyway: the operator does. (It used to print "rebooting
        // anyway" and carry on by itself — the same mistake as the silent
        // branches, just louder.)
        await lingerStep();
        return askBeforeLosingData('cache');
    }
    const result = last.last_result || '';
    if (result === 'ok') {
        step(tr('restart.cache_saved'), 'status-ok');
        await lingerStep();
        return statsUnknown ? 'doubt' : 'ready';
    }
    if (result === 'empty') {
        step(tr('restart.cache_empty'));
        await lingerStep();
        return statsUnknown ? 'doubt' : 'ready';
    }
    if (result === 'failed') {
        // The device said the cache could not be written, and carrying on now
        // would lose it. That is the operator's call, not the page's: asking
        // costs one click, losing a warm cache costs him the working set he has
        // just built.
        return askBeforeLosingData('cache');
    }
    // No verdict at all: the device does not report one (a firmware without
    // `last_result` answers `busy=false` and nothing more). Guessing "failed"
    // from a missing field is exactly what made this flow lie once — but going
    // silent is the other half of the same mistake: the operator cannot tell a
    // cache that was written from one that was never touched, and all he sees
    // is a restart with no cache step in it. So say what is known — the answer
    // never arrived, the device writes the file itself during the restart —
    // and send no `saved` flag.
    step(tr('restart.cache_no_verdict'), 'status-warn');
    await lingerStep();
    return 'doubt';
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
        .forEach(d => { d.classList.remove('open'); closeSubmenus(d); });
    if (!wasOpen) dd.classList.add('open');
}

/* Toggle a group inside an open dropdown (Settings → Security). It deliberately
   leaves the surrounding dropdown alone: the menu it lives in has to stay open,
   and a click on the group is not a click "outside" the menu. */
function toggleSubmenu(event) {
    event.preventDefault();
    event.stopPropagation();
    const group = event.currentTarget.closest('.nav-submenu');
    if (group) group.classList.toggle('open');
}

/* Collapse the groups of a dropdown that is being closed: reopening a menu shows
   its plain list instead of remembering an expanded group from last time. */
function closeSubmenus(root) {
    root.querySelectorAll('.nav-submenu.open')
        .forEach(g => g.classList.remove('open'));
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
                .forEach(d => { d.classList.remove('open'); closeSubmenus(d); });
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
        setInterval(poll, kStatusPollMs);
    }
});

function setMeter(id, pct) {
    const bar = document.getElementById(id);
    if (!bar) return;
    const v = Math.max(0, Math.min(kPercentScale, Math.round(pct)));
    bar.style.width = v + '%';
}

/* Byte sizes on the status page: kilobytes for the on-chip memories, megabytes
   or gigabytes for the FAT volumes (21 MB partition / multi-GB cards), so a
   single helper covers both RAM and storage rows. */
function formatBytes(bytes) {
    const b = Number(bytes) || 0;
    if (b < kBytesPerKib) return b + ' B';
    const kb = b / kBytesPerKib;
    if (kb < kBytesPerKib) return kb.toFixed(0) + ' KB';
    const mb = kb / kBytesPerKib;
    if (mb < kBytesPerKib) return mb.toFixed(mb < 10 ? 1 : 0) + ' MB';
    return (mb / kBytesPerKib).toFixed(2) + ' GB';
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
            // Rule 39: the scale the uptime is broken down into.
            const SEC_PER_DAY = 86400;
            const SEC_PER_HOUR = 3600;
            const SEC_PER_MIN = 60;
            const days = Math.floor(sec / SEC_PER_DAY);
            const hours = Math.floor(sec / SEC_PER_HOUR);
            const mins = Math.floor(sec / SEC_PER_MIN);
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
            : (data.heap_total != null ? data.heap_total : kFallbackRamTotalBytes);
        const ramFree = data.heap_free != null ? data.heap_free : 0;
        const ramTotalEl = document.getElementById('ram-total');
        if (ramTotalEl) ramTotalEl.textContent = formatBytes(ramTotal);
        const freeRamEl = document.getElementById('free-ram');
        if (freeRamEl) freeRamEl.textContent = formatBytes(ramFree) + ' ' + tr('app.free_short');
        if (document.getElementById('ram-bar')) {
            setMeter('ram-bar',
                     ramTotal > 0 ? (1 - ramFree / ramTotal) * kPercentScale : 0);
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
                    setMeter('psram-bar', (1 - psramFree / psramTotal) * kPercentScale);
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
                setMeter(barId,
                         mounted ? (1 - vol.free_bytes / vol.total_bytes) * kPercentScale : 0);
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
                const MB = kBytesPerMib;
                const used = data.internal_cache_used_bytes != null ? data.internal_cache_used_bytes : 0;
                const free = data.internal_cache_free_bytes != null ? data.internal_cache_free_bytes : 0;
                const entries = data.internal_cache_entries != null ? data.internal_cache_entries : 0;
                const capacity = data.internal_cache_capacity != null ? data.internal_cache_capacity : 0;
                const hits = data.internal_cache_hits != null ? data.internal_cache_hits : 0;
                const fwd = data.internal_forward_count != null ? data.internal_forward_count : 0;
                const avgUs = data.internal_cache_avg_hit_us != null ? data.internal_cache_avg_hit_us : 0;
                const mbFmt = (bytes) => {
                    const m = bytes / MB;
                    if (m > 0 && m < kSmallValueRatio) {
                        return (bytes / kBytesPerKib).toFixed(0) + ' KB';
                    }
                    return m.toFixed(2) + ' MB';
                };
                const usFmt = (us) => {
                    if (us < kUsPerMs) return us + 'µs';
                    return (us / kUsPerMs).toFixed(1) + 'ms';
                };
                const usedEl = document.getElementById('internal-cache-used');
                if (usedEl) usedEl.textContent = mbFmt(used);
                const freeEl = document.getElementById('internal-cache-free');
                if (freeEl) freeEl.textContent = mbFmt(free) + ' free';
                const pct = (used + free) > 0
                    ? (used / (used + free)) * kPercentScale : 0;
                setMeter('internal-cache-bar', pct);
                const statsEl = document.getElementById('internal-cache-stats');
                const diagEl = document.getElementById('internal-cache-diag');
                // Stage 127: the average is the whole lookup, and a lookup takes
                // the arena mutex first — so the average and its parts (waiting
                // for somebody else, our own work, the chain a hit walked, what
                // a full-pool eviction costs) go on a line of their own, under
                // the counters. A device that does not send them keeps the
                // average where it always was.
                const avgWait = data.internal_cache_avg_wait_us != null ? data.internal_cache_avg_wait_us : null;
                const avgWork = data.internal_cache_avg_work_us != null ? data.internal_cache_avg_work_us : null;
                const walkX100 = data.internal_cache_walk_x100 != null ? data.internal_cache_walk_x100 : null;
                const scans = data.internal_cache_evict_scans != null ? data.internal_cache_evict_scans : 0;
                const scanMs = data.internal_cache_evict_scan_ms != null ? data.internal_cache_evict_scan_ms : 0;
                const hasDiag = avgWait != null && avgWork != null;
                if (statsEl) {
                    let s =
                        tr('app.ic_entries') + ' ' + entries + ' / ' + capacity +
                        '   ·   ' + tr('app.ic_hits') + ' ' + hits +
                        '   ·   ' + tr('app.ic_forward') + ' ' + fwd;
                    if (avgUs > 0 && !hasDiag) {
                        s += '   ·   ' + tr('app.ic_avg') + ' ' + usFmt(avgUs);
                    }
                    statsEl.textContent = s;
                }
                if (diagEl) {
                    if (!hasDiag) {
                        diagEl.textContent = '';
                        diagEl.style.display = 'none';
                    } else {
                        let d = tr('app.ic_avg') + ' ' + usFmt(avgUs) +
                                '   ·   ' + tr('app.ic_wait') + ' ' + usFmt(avgWait) +
                                '   ·   ' + tr('app.ic_work') + ' ' + usFmt(avgWork);
                        if (walkX100 != null) {
                            d += '   ·   ' + tr('app.ic_walk') + ' ' + (walkX100 / 100).toFixed(1);
                        }
                        if (scans > 0) {
                            d += '   ·   ' + tr('app.ic_evict') + ' ' + scans + ' × ' + usFmt(scanMs * 1000);
                        }
                        diagEl.textContent = d;
                        diagEl.style.display = '';
                    }
                }
            }
        }
    } catch (e) {
        console.warn('Status update failed:', e);
    }
}
