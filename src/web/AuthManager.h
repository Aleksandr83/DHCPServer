#ifndef DHCP_WEB_AUTHMANAGER_H
#define DHCP_WEB_AUTHMANAGER_H

#include <string>
#include <map>
#include <cstdint>

namespace dhcp {
namespace web {

/**
 * @brief HTTP Basic Authentication manager with rate limiting.
 *
 * Tracks failed attempts per client IP.
 * Locks out clients that exceed max_attempts within lockout_period.
 */
class AuthManager {
public:
    AuthManager();
    ~AuthManager() = default;

    /**
     * @brief Authenticate a request using the Authorization header.
     *
     * @param authHeader  Value of the Authorization header (or empty).
     * @param clientIp    Client IP string for rate limiting.
     * @return true if authenticated, false otherwise.
     */
    bool authenticate(const std::string& authHeader, const std::string& clientIp);

    /**
     * @brief Reload settings from Config.
     */
    void reloadConfig();

    /**
     * @brief Generate WWW-Authenticate header value.
     */
    std::string wwwAuthenticateHeader() const;

    /**
     * @brief Check if a client is currently locked out.
     */
    bool isLockedOut(const std::string& clientIp) const;

    /**
     * @brief Load settings from Config (NVS must be initialized).
     * Called explicitly from WebServer::start().
     */
    void ensureConfigLoaded();

private:
    struct FailedEntry {
        uint32_t count = 0;
        uint32_t firstAttemptTime = 0; // seconds since boot
    };

    std::string username_ = "admin";
    std::string password_ = "admin";
    uint32_t maxAttempts_ = 5;
    uint32_t lockoutPeriodSec_ = 300;

    /** True after reloadConfig() successfully loaded from NVS */
    bool configLoaded_ = false;

    // Failed attempts per IP
    mutable std::map<std::string, FailedEntry> failedAttempts_;

    void recordFailure(const std::string& clientIp);
    uint32_t getCurrentTimeSec() const;
    void cleanupExpired();
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_AUTHMANAGER_H
