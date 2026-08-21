#include "DnsCache.h"
#include <cstdio>
#include <cstring>
#include <cctype>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* TAG = "DnsCache";

// DNS query types (used to filter cached answers by the queried type).
#define DNS_TYPE_A    1
#define DNS_TYPE_AAAA 28

namespace dhcp {
namespace dns {

// The cache server reply has a fixed, simple shape:
//   {"domain":"example.com","ips":["1.2.3.4","1.2.3.5"],"type":1,"expires_at":"...","updated_at":"..."}
// cJSON is not part of ESP-IDF 6, so parse it with a small self-contained
// parser instead of pulling in a managed component.
namespace {
bool parseLookupIps(const std::string& body, std::vector<std::string>& ips)
{
    const std::string needle = "\"ips\"";
    const size_t k = body.find(needle);
    if (k == std::string::npos) return false;
    const size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    const size_t open = body.find('[', colon + 1);
    if (open == std::string::npos) return false;
    const size_t close = body.find(']', open + 1);
    if (close == std::string::npos) return false;

    const std::string arr = body.substr(open + 1, close - open - 1);
    size_t q = 0;
    while (q < arr.size()) {
        const size_t openQ = arr.find('"', q);
        if (openQ == std::string::npos) break;
        const size_t closeQ = arr.find('"', openQ + 1);
        if (closeQ == std::string::npos) break;
        const std::string ip = arr.substr(openQ + 1, closeQ - openQ - 1);
        if (!ip.empty()) ips.push_back(ip);
        q = closeQ + 1;
    }
    return !ips.empty();
}

struct CacheRespCapture {
    char buf[512] = {0};
    size_t len = 0;
    char location[256] = {0};
};

esp_err_t cacheRespHandler(esp_http_client_event_t* evt)
{
    auto* cap = static_cast<CacheRespCapture*>(evt->user_data);
    if (!cap) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        const size_t space = sizeof(cap->buf) - 1 - cap->len;
        const size_t n = (static_cast<size_t>(evt->data_len) < space)
                             ? static_cast<size_t>(evt->data_len) : space;
        if (n > 0) {
            memcpy(cap->buf + cap->len, evt->data, n);
            cap->len += n;
            cap->buf[cap->len] = '\0';
        }
    } else if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
               evt->header_value) {
        // Capture the redirect target for diagnostics (Location header).
        if (strncasecmp(evt->header_key, "Location", 8) == 0) {
            strncpy(cap->location, evt->header_value,
                    sizeof(cap->location) - 1);
        }
    }
    return ESP_OK;
}
} // namespace

DnsCache::DnsCache()
{
}

DnsCache::~DnsCache()
{
    stopCacheSender();
}

void DnsCache::setEnabled(bool enabled)
{
    enabled_ = enabled;
    updateCacheSenderState();
}

void DnsCache::setReadEnabled(bool enabled)
{
    readEnabled_ = enabled;
}

void DnsCache::setWriteEnabled(bool enabled)
{
    writeEnabled_ = enabled;
    updateCacheSenderState();
}

void DnsCache::setTerminalLogging(bool enabled)
{
    terminalLogging_ = enabled;
}

void DnsCache::setUrl(const std::string& url)
{
    url_ = url;
    // Normalize: strip trailing slashes so the resource path appends cleanly.
    while (url_.size() > 1 && url_.back() == '/') url_.pop_back();
    updateCacheSenderState();
    ESP_LOGI(TAG, "Cache URL set to: %s", url_.c_str());
}

void DnsCache::setAuth(bool enabled, const std::string& user,
                       const std::string& pass)
{
    authEnabled_ = enabled;
    authUser_ = user;
    authPassword_ = pass;
    ESP_LOGI(TAG, "Cache auth set (enabled=%d user=%s)",
             enabled ? 1 : 0, user.c_str());
}

// ─── Helpers ────────────────────────────────────────

std::string DnsCache::normalizeDomain(const std::string& domain)
{
    std::string d = domain;
    for (auto& c : d) c = static_cast<char>(tolower((unsigned char)c));
    if (!d.empty() && d.back() == '.') d.pop_back();
    return d;
}

std::string DnsCache::urlEncode(const std::string& s)
{
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '.' || c == '-' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// ─── Lookup (async worker) ──────────────────────────

// Performs one lookup: GET {url}/{domain} + parse + type filter.
// Runs in the lookup worker task, NEVER in the DNS server task (the DNS task
// must not block on HTTP — a blocking lookup there stalled every query for
// ~1.6 s and made sites time out).
bool DnsCache::isDegraded() const
{
    const int64_t now = esp_timer_get_time() / 1000;
    return degradedUntilMs_ != 0 && now < degradedUntilMs_;
}

// Circuit breaker: count slow/failed lookups; after kFailThreshold in a row
// bypass the cache for kDegradeCooldownMs so a slow cache can never degrade
// DNS. A fast lookup resets the counters and closes the breaker.
void DnsCache::notifyLookupOutcome(int64_t elapsedMs)
{
    if (elapsedMs >= kSlowLookupMs) {
        lookupBadCount_ = lookupBadCount_ + 1;
        if (lookupBadCount_ >= kFailThreshold) {
            degradedUntilMs_ = esp_timer_get_time() / 1000 + kDegradeCooldownMs;
            ESP_LOGW(TAG, "Cache degraded: %u slow/failed lookups in a row, bypassing for %lld ms",
                     static_cast<unsigned>(lookupBadCount_),
                     static_cast<long long>(kDegradeCooldownMs));
        }
    } else {
        if (lookupBadCount_ > 0 || degradedUntilMs_ != 0) {
            if (terminalLogging_) ESP_LOGI(TAG, "Cache lookup healthy again");
        }
        lookupBadCount_ = 0;
        degradedUntilMs_ = 0;
    }
}

bool DnsCache::doLookupAndParse(const std::string& domain, uint16_t type,
                                std::vector<std::string>& result)
{
    if (!enabled_ || url_.empty()) return false;

    const std::string d = normalizeDomain(domain);
    if (d.empty()) return false;

    // Resource-style URL: GET {url}/{domain}
    const std::string url = url_ + "/" + urlEncode(d);
    if (terminalLogging_) ESP_LOGI(TAG, "Cache lookup GET: %s type=%u", url.c_str(), type);

    std::string body;
    const int status = doLookup(url, body);
    if (status == 0) return false;              // transport failure (already WARN)
    if (status == 404) {                        // not found / expired — normal miss
        if (terminalLogging_) ESP_LOGI(TAG, "Cache miss: %s type=%u (404)", d.c_str(), type);
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Cache lookup HTTP %d: domain=%s (miss)", status, d.c_str());
        return false;
    }
    if (!parseLookupIps(body, result)) {
        if (terminalLogging_) ESP_LOGI(TAG, "Cache miss: %s type=%u (empty/bad body)", d.c_str(), type);
        return false;
    }

    // The server keys by domain only, so keep only answers matching the
    // queried type (A -> IPv4, AAAA -> IPv6) — answering e.g. an A record
    // to an AAAA query is rejected by strict resolvers.
    std::vector<std::string> filtered;
    for (const auto& ip : result) {
        const bool isV6 = ip.find(':') != std::string::npos;
        if ((type == DNS_TYPE_AAAA) ? isV6 : !isV6) filtered.push_back(ip);
    }
    result.swap(filtered);

    if (!result.empty()) {
        if (terminalLogging_) {
            ESP_LOGI(TAG, "Cache hit: %s type=%u -> %zu IP(s)", d.c_str(),
                     type, result.size());
        }
    } else {
        if (terminalLogging_) ESP_LOGI(TAG, "Cache miss: %s type=%u (wrong record type)", d.c_str(), type);
    }
    return !result.empty();
}

// Dedup helpers: track domains with an in-flight lookup so a burst of
// queries for the same domain fires only ONE GET to the cache server.
bool DnsCache::markPending(const std::string& d)
{
    portENTER_CRITICAL(&pendingLock_);
    for (int i = 0; i < pendingCount_; ++i) {
        if (strcmp(pendingDomains_[i], d.c_str()) == 0) {
            portEXIT_CRITICAL(&pendingLock_);
            return false;  // already pending → skip (dedup)
        }
    }
    if (pendingCount_ < kMaxPendingDomains) {
        strncpy(pendingDomains_[pendingCount_], d.c_str(),
                sizeof(pendingDomains_[0]) - 1);
        pendingDomains_[pendingCount_][sizeof(pendingDomains_[0]) - 1] = '\0';
        pendingCount_++;
    }
    portEXIT_CRITICAL(&pendingLock_);
    // Table full → can't dedup, allow the lookup (best-effort).
    return true;
}

void DnsCache::clearPending(const std::string& d)
{
    portENTER_CRITICAL(&pendingLock_);
    for (int i = 0; i < pendingCount_; ++i) {
        if (strcmp(pendingDomains_[i], d.c_str()) == 0) {
            for (int j = i; j < pendingCount_ - 1; ++j) {
                strcpy(pendingDomains_[j], pendingDomains_[j + 1]);
            }
            pendingCount_--;
            break;
        }
    }
    portEXIT_CRITICAL(&pendingLock_);
}

void DnsCache::submitLookup(const std::string& domain, uint16_t type,
                            uint8_t token)
{
    if (!enabled_ || !readEnabled_ || url_.empty()) {
        if (terminalLogging_) {
            ESP_LOGI(TAG, "Cache lookup SUBMIT SKIP: enabled=%d read=%d url=%s",
                     enabled_ ? 1 : 0, readEnabled_ ? 1 : 0,
                     url_.empty() ? "-" : url_.c_str());
        }
        return;
    }
    if (!lookupQueue_) {
        ESP_LOGW(TAG, "Cache lookup SUBMIT SKIP: worker queue not created");
        return;
    }

    const std::string d = normalizeDomain(domain);
    if (d.empty()) return;

    // Dedup: skip if a lookup for this domain is already in flight.
    if (!markPending(d)) {
        if (terminalLogging_) ESP_LOGI(TAG, "Cache lookup DEDUP: domain=%s type=%u token=%u (already pending)",
                                       d.c_str(), type, token);
        return;
    }

    LookupRequest req;
    req.token = token;
    req.type = type;
    strncpy(req.domain, d.c_str(), sizeof(req.domain) - 1);
    if (terminalLogging_) ESP_LOGI(TAG, "Cache lookup SUBMIT: domain=%s type=%u token=%u",
                                   d.c_str(), type, token);

    if (xQueueSendToBack(lookupQueue_, &req, 0) != pdTRUE) {
        // Queue full -> drop the oldest, then retry once.
        LookupRequest discard;
        if (xQueueReceive(lookupQueue_, &discard, 0) == pdTRUE) {
            xQueueSendToBack(lookupQueue_, &req, 0);
        }
    }
}

void DnsCache::startLookupWorker()
{
    ensureLookupWorker();
}

void DnsCache::ensureLookupWorker()
{
    // Wakeup socket: UDP bound on loopback. The worker sends result datagrams
    // to 127.0.0.1:<port>; the DNS task selects on lookupFd_ and recvfrom()s
    // them. lwIP delivers loopback UDP to the bound socket.
    if (lookupFd_ < 0) {
        lookupFd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (lookupFd_ < 0) {
            ESP_LOGE(TAG, "Failed to create cache lookup wakeup socket");
            return;
        }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;  // ephemeral
        if (bind(lookupFd_, (struct sockaddr*)&a, sizeof(a)) < 0) {
            ESP_LOGE(TAG, "Failed to bind cache lookup wakeup socket");
            close(lookupFd_);
            lookupFd_ = -1;
            return;
        }
        socklen_t alen = sizeof(a);
        getsockname(lookupFd_, (struct sockaddr*)&a, &alen);
        lookupPort_ = ntohs(a.sin_port);
    }

    if (lookupQueue_ == nullptr) {
        lookupQueue_ = xQueueCreate(kLookupQueueDepth, sizeof(LookupRequest));
    }
    if (!lookupQueue_) {
        ESP_LOGE(TAG, "Failed to create cache lookup queue");
        return;
    }
    if (lookupTask_) return;  // already running

    lookupStopRequested_ = false;
    if (xTaskCreate(&DnsCache::lookupWorkerTask, "dns_cach_lk",
                    kLookupWorkerStack, this, kLookupWorkerPriority,
                    &lookupTask_) != pdPASS) {
        lookupTask_ = nullptr;
        ESP_LOGE(TAG, "Failed to create cache lookup worker task");
    }
}

void DnsCache::stopLookupWorker()
{
    if (!lookupTask_) return;
    lookupStopRequested_ = true;
    // Wake the worker if it is blocked on the queue.
    LookupRequest wake;
    wake.token = 0xFF;
    if (lookupQueue_) {
        xQueueSendToBack(lookupQueue_, &wake, pdMS_TO_TICKS(10));
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    while (lookupTask_ != nullptr && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    lookupStopRequested_ = false;
}

void DnsCache::lookupWorkerTask(void* arg)
{
    auto* self = static_cast<DnsCache*>(arg);
    self->lookupWorkerLoop();
    vTaskDelete(nullptr);
}

void DnsCache::lookupWorkerLoop()
{
    LookupRequest req;
    while (!lookupStopRequested_) {
        if (xQueueReceive(lookupQueue_, &req, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        if (req.token == 0xFF) break;  // stop marker

        if (terminalLogging_) ESP_LOGI(TAG, "Cache lookup WORKER: domain=%s type=%u token=%u",
                                       req.domain, req.type, req.token);
        const int64_t t0 = esp_timer_get_time();
        std::vector<std::string> result;
        const bool hit = doLookupAndParse(req.domain, req.type, result);
        const int64_t elapsedMs = (esp_timer_get_time() - t0) / 1000;
        notifyLookupOutcome(elapsedMs);
        if (terminalLogging_) ESP_LOGI(TAG, "Cache lookup WORKER done: domain=%s hit=%d ips=%zu (%lld ms)",
                                       req.domain, hit ? 1 : 0, result.size(),
                                       static_cast<long long>(elapsedMs));
        // Release the dedup entry so a fresh lookup for this domain can start.
        clearPending(req.domain);

        // Deliver the result to the DNS task via the UDP loopback socket.
        if (lookupFd_ >= 0) {
            LookupResult res;
            res.token = req.token;
            res.hit = hit;
            std::string joined;
            for (size_t i = 0; i < result.size(); ++i) {
                if (i) joined += ',';
                joined += result[i];
            }
            strncpy(res.ips, joined.c_str(), sizeof(res.ips) - 1);

            struct sockaddr_in dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            dst.sin_port = htons(lookupPort_);
            sendto(lookupFd_, &res, sizeof(res), 0,
                   (struct sockaddr*)&dst, sizeof(dst));
        }
    }
    lookupTask_ = nullptr;
}

bool DnsCache::drainLookupResult(LookupResult& out)
{
    if (lookupFd_ < 0) return false;
    struct sockaddr_in from;
    socklen_t alen = sizeof(from);
    memset(&from, 0, sizeof(from));
    ssize_t n = recvfrom(lookupFd_, &out, sizeof(out), 0,
                         (struct sockaddr*)&from, &alen);
    return n == static_cast<ssize_t>(sizeof(out));
}

int DnsCache::doLookup(const std::string& url, std::string& respBody)
{
    CacheRespCapture respCap;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = kLookupTimeoutMs;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    // Do NOT follow 3xx redirects automatically: a redirect loop (e.g. a
    // server bouncing to /login) used to burn 10 requests (~2.4 s each) and
    // end in ESP_ERR_HTTP_MAX_REDIRECT. With auto-redirect off the 3xx status
    // is returned here and logged with the Location header for diagnosis.
    cfg.disable_auto_redirect = true;
    if (authEnabled_ && !authUser_.empty()) {
        cfg.username = authUser_.c_str();
        cfg.password = authPassword_.c_str();
        // Send Basic auth preemptively. Without auth_type the first request
        // carries NO Authorization header, the server answers 401, and
        // esp_http_client_add_auth() retries up to 10 times (each incrementing
        // redirect_counter) until ESP_ERR_HTTP_MAX_REDIRECT fires — the same
        // misleading error as a 3xx redirect loop. Set the type so the header
        // is sent on the first attempt.
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    // Disable the 401 retry loop entirely: a single 401 is returned as a
    // status code (logged below), instead of 10 auth retries ending in
    // ESP_ERR_HTTP_MAX_REDIRECT.
    cfg.max_authorization_retries = -1;
    // Self-signed certs are accepted via CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    // (sets MBEDTLS_SSL_VERIFY_NONE), so skip_cert_common_name_check must NOT
    // be set — that flag disables SNI and Apache then returns 421 for the
    // name-based vhost. SNI (the URL hostname) is sent automatically.
    cfg.event_handler = &cacheRespHandler;
    cfg.user_data = &respCap;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Cache lookup INIT FAILED (cannot allocate client)");
        return 0;
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = esp_http_client_perform(client);
    const int64_t elapsedMs = (esp_timer_get_time() - t0) / 1000;

    int status = 0;
    if (err != ESP_OK) {
        // Report the HTTP status too (e.g. 401) so an auth problem is visible
        // instead of a generic ESP_FAIL/MAX_REDIRECT.
        const int hstatus = esp_http_client_get_status_code(client);
        ESP_LOGW(TAG, "Cache lookup FAILED (%s): http=%d url=%s (%lld ms)",
                 esp_err_to_name(err), hstatus, url.c_str(),
                 static_cast<long long>(elapsedMs));
    } else {
        status = esp_http_client_get_status_code(client);
        respBody.assign(respCap.buf, respCap.len);
        if (status >= 300 && status < 400) {
            // Redirect to /login (or a loop) — surface it loudly.
            ESP_LOGW(TAG, "Cache lookup HTTP %d (redirect): url=%s location=%s (%lld ms)",
                     status, url.c_str(),
                     respCap.location[0] ? respCap.location : "-",
                     static_cast<long long>(elapsedMs));
        } else if (terminalLogging_) {
            ESP_LOGI(TAG, "Cache lookup %d: url=%s (%lld ms)",
                     status, url.c_str(),
                     static_cast<long long>(elapsedMs));
        }
    }
    esp_http_client_cleanup(client);
    return status;
}

// ─── Store (async, fire-and-forget) ─────────────────

void DnsCache::store(const std::string& domain, uint16_t type,
                     const std::vector<std::string>& ips)
{
    if (!enabled_ || !writeEnabled_ || url_.empty()) {
        if (terminalLogging_) {
            ESP_LOGI(TAG, "Cache store SKIP: enabled=%d write=%d url=%s",
                     enabled_ ? 1 : 0, writeEnabled_ ? 1 : 0,
                     url_.empty() ? "-" : url_.c_str());
        }
        return;
    }
    if (ips.empty()) {
        if (terminalLogging_) ESP_LOGI(TAG, "Cache store SKIP: empty ips");
        return;
    }
    if (!storeQueue_) {
        ESP_LOGW(TAG, "Cache store SKIP: sender queue not created");
        return;
    }

    const std::string d = normalizeDomain(domain);
    if (d.empty()) return;

    CacheStoreRecord rec;
    rec.type = type;
    strncpy(rec.domain, d.c_str(), sizeof(rec.domain) - 1);
    std::string joined;
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i) joined += ',';
        joined += ips[i];
    }
    strncpy(rec.ips, joined.c_str(), sizeof(rec.ips) - 1);
    if (terminalLogging_) ESP_LOGI(TAG, "Cache store QUEUED: domain=%s type=%u ips=%s",
                                   d.c_str(), type, joined.c_str());

    if (xQueueSendToBack(storeQueue_, &rec, 0) != pdTRUE) {
        // Ring buffer full -> drop the oldest, then retry once.
        CacheStoreRecord discard;
        if (xQueueReceive(storeQueue_, &discard, 0) == pdTRUE) {
            xQueueSendToBack(storeQueue_, &rec, 0);
        }
        ++storeDropped_;
        if ((storeDropped_ % 50) == 1) {
            ESP_LOGW(TAG, "Cache store queue full, oldest dropped (%u total)",
                     static_cast<unsigned>(storeDropped_));
        }
    }
}

// ─── Async store sender ─────────────────────────────

void DnsCache::updateCacheSenderState()
{
    const bool want = enabled_ && writeEnabled_ && !url_.empty();
    if (want && !storeTask_) {
        ensureCacheSender();
    } else if (!want && storeTask_) {
        stopCacheSender();
    }
}

void DnsCache::ensureCacheSender()
{
    if (storeQueue_ == nullptr) {
        storeQueue_ = xQueueCreate(kStoreQueueDepth, sizeof(CacheStoreRecord));
    }
    if (!storeQueue_) {
        ESP_LOGE(TAG, "Failed to create cache store queue");
        return;
    }
    storeStopRequested_ = false;
    if (xTaskCreate(&DnsCache::cacheSenderTask, "dns_cache",
                    kStoreSenderStack, this, kStoreSenderPriority,
                    &storeTask_) != pdPASS) {
        storeTask_ = nullptr;
        ESP_LOGE(TAG, "Failed to create cache store sender task");
    }
}

void DnsCache::stopCacheSender()
{
    if (!storeTask_) return;
    storeStopRequested_ = true;
    CacheStoreRecord marker;
    marker.stop = true;
    if (storeQueue_) {
        xQueueSendToBack(storeQueue_, &marker, pdMS_TO_TICKS(10));
    }
    // Wait for the sender to exit (an in-flight POST may take up to the
    // timeout). The queue is kept allocated to avoid a use-after-free.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    while (storeTask_ != nullptr && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    storeStopRequested_ = false;
}

void DnsCache::cacheSenderTask(void* arg)
{
    auto* self = static_cast<DnsCache*>(arg);
    CacheStoreRecord rec;
    while (!self->storeStopRequested_) {
        if (xQueueReceive(self->storeQueue_, &rec, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (rec.stop) break;
            self->sendStore(rec);
        }
    }
    self->storeTask_ = nullptr;
    vTaskDelete(nullptr);
}

void DnsCache::sendStore(const CacheStoreRecord& rec)
{
    if (url_.empty()) return;
    const std::string payload = buildStoreJson(rec);
    // Resource-style URL: PUT {url}/{domain}
    const std::string url = url_ + "/" + urlEncode(rec.domain);

    if (terminalLogging_) ESP_LOGI(TAG, "Cache store: PUT %s domain=%s type=%u auth=%s",
                                   url.c_str(), rec.domain, rec.type,
                                   (authEnabled_ && !authUser_.empty()) ? "on" : "off");

    CacheRespCapture respCap;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_PUT;
    cfg.timeout_ms = kStoreSendTimeoutMs;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    // See doLookup: never chase 3xx redirect loops automatically.
    cfg.disable_auto_redirect = true;
    if (authEnabled_ && !authUser_.empty()) {
        cfg.username = authUser_.c_str();
        cfg.password = authPassword_.c_str();
        // Send Basic auth preemptively (see doLookup for why).
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    // Disable the 401 retry loop (see doLookup for why).
    cfg.max_authorization_retries = -1;
    cfg.event_handler = &cacheRespHandler;
    cfg.user_data = &respCap;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Cache store INIT FAILED (cannot allocate client)");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // Tell Laravel the client expects JSON so an unauthenticated API request
    // returns 401 JSON instead of a 302 redirect to login (redirect loop).
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, payload.c_str(),
                                   static_cast<int>(payload.size()));

    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = esp_http_client_perform(client);
    const int64_t elapsedMs = (esp_timer_get_time() - t0) / 1000;

    if (err != ESP_OK) {
        // Report the HTTP status too (e.g. 401) so an auth problem is visible.
        const int hstatus = esp_http_client_get_status_code(client);
        ESP_LOGW(TAG, "Cache store FAILED (%s): http=%d domain=%s url=%s (%lld ms)",
                 esp_err_to_name(err), hstatus, rec.domain, url.c_str(),
                 static_cast<long long>(elapsedMs));
    } else {
        const int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
        if (terminalLogging_) {
            ESP_LOGI(TAG, "Cache store OK: domain=%s type=%u status=%d body=\"%s\" (%lld ms)",
                     rec.domain, rec.type, status, respCap.buf,
                     static_cast<long long>(elapsedMs));
        }
        } else {
            ESP_LOGW(TAG, "Cache store HTTP %d: domain=%s type=%u location=%s body=\"%s\" (%lld ms)",
                     status, rec.domain, rec.type,
                     respCap.location[0] ? respCap.location : "-",
                     respCap.buf,
                     static_cast<long long>(elapsedMs));
        }
    }
    esp_http_client_cleanup(client);
}

std::string DnsCache::buildStoreJson(const CacheStoreRecord& rec) const
{
    // The server takes the domain from the URL path, so the body carries
    // only the record data: {"ips":[...],"type":N}.
    std::string json = "{\"ips\":[";
    // ips is stored comma-separated; split back into a JSON array.
    const std::string list(rec.ips);
    size_t start = 0;
    bool first = true;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        const std::string ip = list.substr(start, comma - start);
        if (!ip.empty()) {
            if (!first) json += ',';
            json += '"';
            json += ip;
            json += '"';
            first = false;
        }
        if (comma == list.size()) break;
        start = comma + 1;
    }
    json += "],\"type\":";
    json += std::to_string(rec.type);
    json += "}";
    return json;
}

} // namespace dns
} // namespace dhcp
