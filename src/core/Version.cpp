#include "Version.h"
#include <cstdio>
#include <cstring>

// Menuconfig definitions
#ifndef CONFIG_FW_VER_GLOBAL
#define CONFIG_FW_VER_GLOBAL 1
#endif
#ifndef CONFIG_FW_VER_DEVICE
#define CONFIG_FW_VER_DEVICE 2
#endif
#ifndef CONFIG_FW_VER_RELEASE
#define CONFIG_FW_VER_RELEASE 36
#endif
#ifndef CONFIG_FW_VER_SUBRELEASE
#define CONFIG_FW_VER_SUBRELEASE 0
#endif
#ifndef CONFIG_FW_VER_YEAR
#define CONFIG_FW_VER_YEAR 26
#endif
#ifndef CONFIG_FW_VER_MONTH
#define CONFIG_FW_VER_MONTH 9
#endif
#ifndef CONFIG_FW_VER_REGION
#define CONFIG_FW_VER_REGION "RU"
#endif

namespace dhcp {
namespace core {

Version& Version::instance()
{
    static Version inst;
    return inst;
}

Version::Version()
    : global_(CONFIG_FW_VER_GLOBAL)
    , device_(CONFIG_FW_VER_DEVICE)
    , release_(CONFIG_FW_VER_RELEASE)
    , subRelease_(CONFIG_FW_VER_SUBRELEASE)
    , year_(CONFIG_FW_VER_YEAR)
    , month_(CONFIG_FW_VER_MONTH)
    , region_(CONFIG_FW_VER_REGION)
{
    // Trim region to exactly 2 chars
    if (region_.length() > 2) {
        region_ = region_.substr(0, 2);
    } else if (region_.length() < 2) {
        if (region_.empty()) {
            region_ = "??";
        } else {
            region_ += "?";
        }
    }
}

const std::string& Version::toString() const
{
    if (!cached_valid_) {
        char buf[32];
        std::snprintf(buf, sizeof(buf),
                      "%02u.%02u.%03u.%02u.%02u.%02u.%s",
                      static_cast<unsigned>(global_),
                      static_cast<unsigned>(device_),
                      static_cast<unsigned>(release_),
                      static_cast<unsigned>(subRelease_),
                      static_cast<unsigned>(year_),
                      static_cast<unsigned>(month_),
                      region_.c_str());
        cached_ = buf;
        cached_valid_ = true;
    }
    return cached_;
}

} // namespace core
} // namespace dhcp
