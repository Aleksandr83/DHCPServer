#ifndef DHCP_CORE_CREDENTIALS_H
#define DHCP_CORE_CREDENTIALS_H

#include <string>

namespace dhcp {
namespace core {

/**
 * @brief Compare a typed login with the stored pair (stage 222).
 *
 * The SSH server accepts the web interface's credentials, so the comparison has
 * to answer the same question the HTTP path answers — but it is a separate
 * function because it is reached from a different place and must not be written
 * three more times by hand.
 *
 * The comparison walks the **whole** string of each field regardless of where a
 * difference appears, so the time it takes does not depend on how much of the
 * password was guessed right. That is a small thing on a LAN appliance and a
 * free one here: the loop is the same length either way.
 *
 * An empty stored user or password matches nothing: a device whose NVS was never
 * written must not accept a blank login.
 */
inline bool credentialsMatch(const std::string& user, const std::string& password,
                             const std::string& expectedUser,
                             const std::string& expectedPassword)
{
    if (expectedUser.empty() || expectedPassword.empty()) return false;

    unsigned diff = 0;
    const size_t userLen = user.size();
    diff |= static_cast<unsigned>(userLen ^ expectedUser.size());
    for (size_t i = 0; i < expectedUser.size(); i++) {
        const char given = (i < userLen) ? user[i] : '\0';
        diff |= static_cast<unsigned>(given ^ expectedUser[i]);
    }

    const size_t passLen = password.size();
    diff |= static_cast<unsigned>(passLen ^ expectedPassword.size());
    for (size_t i = 0; i < expectedPassword.size(); i++) {
        const char given = (i < passLen) ? password[i] : '\0';
        diff |= static_cast<unsigned>(given ^ expectedPassword[i]);
    }

    return diff == 0;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_CREDENTIALS_H
