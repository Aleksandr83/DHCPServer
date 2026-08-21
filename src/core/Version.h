#ifndef DHCP_CORE_VERSION_H
#define DHCP_CORE_VERSION_H

#include <string>
#include <cstdint>

namespace dhcp {
namespace core {

/**
 * @brief Firmware version string builder.
 *
 * Format: aa.bb.xxx.cc.YY.MM.RR
 *   aa  — global version (00–99)
 *   bb  — device/product code (00–99)
 *   xxx — release number (000–999)
 *   cc  — sub-release (00–99)
 *   YY  — year, last 2 digits
 *   MM  — month (01–12)
 *   RR  — region (2 chars, from CONFIG_FW_VER_REGION)
 *
 * All numeric fields come from menuconfig (CONFIG_FW_VER_*).
 */
class Version {
public:
    /**
     * @brief Get the singleton instance.
     */
    static Version& instance();

    /**
     * @brief Build and return the full version string.
     * The string is cached after first call.
     */
    const std::string& toString() const;

    /**
     * @brief Get individual components.
     */
    uint8_t global() const { return global_; }
    uint8_t device() const { return device_; }
    uint16_t release() const { return release_; }
    uint8_t subRelease() const { return subRelease_; }
    uint8_t year() const { return year_; }
    uint8_t month() const { return month_; }
    const std::string& region() const { return region_; }

private:
    Version();
    ~Version() = default;
    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    uint8_t global_;
    uint8_t device_;
    uint16_t release_;
    uint8_t subRelease_;
    uint8_t year_;
    uint8_t month_;
    std::string region_;
    mutable std::string cached_;
    mutable bool cached_valid_ = false;
};

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_VERSION_H
