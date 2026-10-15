#include "core/SubnetFilter.h"

#include "core/Subnet.h"

using namespace std;

namespace dhcp {
namespace core {

void SubnetFilter::apply(const Settings& cfg)
{
    enabled_ = cfg.enabled;
    deviceIp_ = cfg.deviceIp;
    netmask_ = cfg.netmask;
    valid_ = false;
    network_ = 0;
    mask_ = 0;

    if (!enabled_) return;              // the operator switched this filter off

    uint32_t addr = 0;
    uint32_t mask = 0;
    // 0.0.0.0 is not an address this device has and 0.0.0.0 is not a netmask: both
    // mean "not configured", and a filter that cannot be computed must fail open.
    // `Subnet::isValidMask()` accepts a zero mask on purpose (it answers "are the
    // bits contiguous"), which is exactly how a zero mask used to reach the packet
    // path and drop every client.
    const bool ipOk = Subnet::parseIp4(deviceIp_, addr) && addr != 0;
    const bool maskOk = Subnet::parseIp4(netmask_, mask) && mask != 0 &&
                        Subnet::isValidMask(mask);
    if (!ipOk || !maskOk) return;       // reported through skippedText()

    network_ = Subnet::network(addr, mask);
    mask_ = mask;
    valid_ = true;
}

bool SubnetFilter::contains(uint32_t clientIpHostOrder) const
{
    // Deliberately not `if (!enabled_)`: a filter that cannot decide allows the
    // client, so the safe answer is the default one here and not an early "no".
    if (!active()) return true;
    return Subnet::contains(network_, mask_, clientIpHostOrder);
}

string SubnetFilter::enabledText() const
{
    return "Own-subnet filter enabled: " + Subnet::toString(network_) + "/" +
           to_string(Subnet::prefixLength(mask_));
}

string SubnetFilter::skippedText() const
{
    // One wording for the three subsystems that used to write their own, so the
    // same misconfiguration can no longer be described in three different ways.
    return "Own-subnet filter requested but the DHCP subnet is invalid (server_ip='" +
           deviceIp_ + "', subnet='" + netmask_ + "') — filter SKIPPED";
}

} // namespace core
} // namespace dhcp
