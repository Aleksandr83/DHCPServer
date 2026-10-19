#include "SshStage.h"

#include "esp_attr.h"

namespace dhcp {
namespace net {

namespace {

/// @brief Tells a written stage from the junk RTC memory holds after a power-on.
constexpr uint32_t kStageMagic = 0x53534831;   // "SSH1"

RTC_NOINIT_ATTR uint32_t s_magic;
RTC_NOINIT_ATTR uint32_t s_stage;

} // namespace

void sshStageSet(SshStage stage)
{
    s_magic = kStageMagic;
    s_stage = static_cast<uint32_t>(stage);
}

SshStage sshStageLast()
{
    if (s_magic != kStageMagic) return SshStage::None;
    const uint32_t value = s_stage;
    if (value > static_cast<uint32_t>(SshStage::Shell)) return SshStage::None;
    return static_cast<SshStage>(value);
}

} // namespace net
} // namespace dhcp
