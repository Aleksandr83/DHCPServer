#include "AuthManager.h"
#include "../core/Config.h"

#include <cstring>
#include <vector>
#include <algorithm>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

static const char* TAG = "AuthManager";

namespace dhcp {
namespace web {

AuthManager::AuthManager()
    : username_("admin")
    , password_("admin")
    , maxAttempts_(5)
    , lockoutPeriodSec_(300)
{
    /* Don't call reloadConfig() here — it needs Config which opens NVS,
       and we may be constructed during static init (before app_main).
       Config will be loaded on first use via WebServer::start(). */
}

void AuthManager::ensureConfigLoaded()
{
    if (configLoaded_) return;
    reloadConfig();
}

void AuthManager::reloadConfig()
{
    auto sec = core::Config::instance().getSecurity();
    username_ = sec.username;
    password_ = sec.password;
    maxAttempts_ = sec.maxAttempts;
    lockoutPeriodSec_ = sec.lockoutPeriodSec;
    configLoaded_ = true;
    ESP_LOGI(TAG, "Auth config loaded: user=%s max_attempts=%lu lockout=%lus",
             username_.c_str(),
             static_cast<unsigned long>(maxAttempts_),
             static_cast<unsigned long>(lockoutPeriodSec_));
}

bool AuthManager::authenticate(const std::string& authHeader,
                                const std::string& clientIp)
{
    cleanupExpired();

    // Check lockout
    if (isLockedOut(clientIp)) {
        ESP_LOGW(TAG, "Client %s is locked out", clientIp.c_str());
        return false;
    }

    // No auth header → fail
    if (authHeader.empty()) {
        recordFailure(clientIp);
        return false;
    }

    // Parse "Basic <base64>"
    const std::string prefix = "Basic ";
    if (authHeader.compare(0, prefix.length(), prefix) != 0) {
        recordFailure(clientIp);
        return false;
    }

    std::string base64Credentials = authHeader.substr(prefix.length());

    // Decode base64 (simple implementation for ESP-IDF)
    // We use the mbedTLS base64 decoder
    size_t decodedLen = 0;
    uint8_t decoded[256];
    mbedtls_base64_decode(decoded, sizeof(decoded), &decodedLen,
                          reinterpret_cast<const uint8_t*>(base64Credentials.c_str()),
                          base64Credentials.length());

    if (decodedLen == 0) {
        recordFailure(clientIp);
        return false;
    }

    std::string credentials(reinterpret_cast<char*>(decoded), decodedLen);

    // Format: "username:password"
    auto colonPos = credentials.find(':');
    if (colonPos == std::string::npos) {
        recordFailure(clientIp);
        return false;
    }

    std::string user = credentials.substr(0, colonPos);
    std::string pass = credentials.substr(colonPos + 1);

    if (user == username_ && pass == password_) {
        // Success — clear failed attempts for this IP
        failedAttempts_.erase(clientIp);
        return true;
    }

    recordFailure(clientIp);
    return false;
}

bool AuthManager::isLockedOut(const std::string& clientIp) const
{
    auto it = failedAttempts_.find(clientIp);
    if (it == failedAttempts_.end()) return false;

    uint32_t now = getCurrentTimeSec();
    if (now - it->second.firstAttemptTime > lockoutPeriodSec_) {
        // Lockout period expired
        failedAttempts_.erase(it);
        return false;
    }

    return it->second.count >= maxAttempts_;
}

std::string AuthManager::wwwAuthenticateHeader() const
{
    return "Basic realm=\"DHCPServer\"";
}

void AuthManager::recordFailure(const std::string& clientIp)
{
    uint32_t now = getCurrentTimeSec();
    auto& entry = failedAttempts_[clientIp];
    if (entry.count == 0) {
        entry.firstAttemptTime = now;
    }
    entry.count++;

    ESP_LOGW(TAG, "Auth failed for %s (%lu/%lu attempts)",
             clientIp.c_str(),
             static_cast<unsigned long>(entry.count),
             static_cast<unsigned long>(maxAttempts_));

    if (entry.count >= maxAttempts_) {
        ESP_LOGW(TAG, "Client %s locked out for %lus",
                 clientIp.c_str(),
                 static_cast<unsigned long>(lockoutPeriodSec_));
    }
}

void AuthManager::cleanupExpired()
{
    uint32_t now = getCurrentTimeSec();
    for (auto it = failedAttempts_.begin(); it != failedAttempts_.end(); ) {
        if (now - it->second.firstAttemptTime > lockoutPeriodSec_) {
            it = failedAttempts_.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t AuthManager::getCurrentTimeSec() const
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

} // namespace web
} // namespace dhcp
