#ifndef DHCP_CORE_TIMEDEFAULTS_H
#define DHCP_CORE_TIMEDEFAULTS_H

#include "core/Config.h"

namespace dhcp {
namespace core {

/**
 * @brief The factory values of the settings the Time Server page owns (stage 194).
 *
 * The page has a "Reset to default" button at the bottom, and it appears only
 * while the settings behind it differ from the factory ones. Both halves of that
 * sentence need one answer, not two: the factory values are the ones the device
 * boots with, which are written down once, as the initialisers of @ref TimeConfig
 * — so these two functions read them from a default-constructed config instead of
 * repeating them. A copy here would be the usual kind of copy: right until
 * somebody changes a default, after which the button resets a value the device
 * never had.
 *
 * Not every setting of the time section belongs to that page: the switches and
 * the fields of the "External NTP server" block do, while the logging switches
 * live on the Logging sub-page, have their own page and are left alone by this
 * reset. That boundary is the part of this file most worth keeping honest, which
 * is why it is tested from both sides in `test/test_timedefaults.cpp`.
 */

/**
 * @brief Are all the settings of the Time Server page still at their factory values?
 *
 * Change a logging setting and this stays true: that setting is not on this page.
 */
inline bool timePageIsDefault(const TimeConfig& cfg)
{
    const TimeConfig factory;
    return cfg.enabled == factory.enabled &&
           cfg.syncEnabled == factory.syncEnabled &&
           cfg.externalNtp == factory.externalNtp &&
           cfg.timezone == factory.timezone &&
           cfg.utcOffsetHours == factory.utcOffsetHours &&
           cfg.syncIntervalSec == factory.syncIntervalSec &&
           cfg.allowOwnSubnet == factory.allowOwnSubnet &&
           cfg.onlyAllowedComputers == factory.onlyAllowedComputers &&
           cfg.rateLimitPerSec == factory.rateLimitPerSec;
}

/**
 * @brief Put those settings back to their factory values.
 *
 * Every other field of `cfg` — the logging settings — is left exactly as it was,
 * so a reset of this page cannot silently switch the file log off.
 */
inline void resetTimePageToDefaults(TimeConfig& cfg)
{
    const TimeConfig factory;
    cfg.enabled = factory.enabled;
    cfg.syncEnabled = factory.syncEnabled;
    cfg.externalNtp = factory.externalNtp;
    cfg.timezone = factory.timezone;
    cfg.utcOffsetHours = factory.utcOffsetHours;
    cfg.syncIntervalSec = factory.syncIntervalSec;
    cfg.allowOwnSubnet = factory.allowOwnSubnet;
    cfg.onlyAllowedComputers = factory.onlyAllowedComputers;
    cfg.rateLimitPerSec = factory.rateLimitPerSec;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_TIMEDEFAULTS_H
