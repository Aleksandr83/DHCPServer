#ifndef DHCP_MENU_ICONSOLEOUTPUT_H
#define DHCP_MENU_ICONSOLEOUTPUT_H

#include <string>

namespace dhcp {
namespace menu {

/**
 * @brief Where a console writes (stage 222).
 *
 * The serial menu writes to `printf`, the SSH shell writes into an SSH channel.
 * Both print the same text from the same code, so the sink is the only thing that
 * differs — that is the whole reason this interface exists.
 *
 * It lives in a header of its own so a user of the sink (the SSH file shell) does
 * not drag the whole command set — and, through it, the ESP-IDF — into a host test.
 */
class IConsoleOutput {
public:
    virtual ~IConsoleOutput() = default;
    virtual void write(const std::string& text) = 0;
};

} // namespace menu
} // namespace dhcp

#endif // DHCP_MENU_ICONSOLEOUTPUT_H
