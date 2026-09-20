#ifndef DHCP_SECURITY_CERTERRORTEXT_H
#define DHCP_SECURITY_CERTERRORTEXT_H

#include <cstddef>
#include <string>

#include "mbedtls/error.h"

namespace dhcp {
namespace security {

/** @brief Room for the mbedTLS error text (the library documents 100 bytes as enough). */
constexpr size_t kMbedErrorTextBytes = 100;

/**
 * @brief mbedTLS error number as text.
 *
 * Deliberately not in the class headers: they are compiled by the host tests,
 * which have no mbedTLS, while both implementations need the same rendering
 * (rule 2 — one implementation of "an error number the operator can read").
 */
inline std::string mbedErrorText(int err)
{
    char buffer[kMbedErrorTextBytes];
    buffer[0] = '\0';
    mbedtls_strerror(err, buffer, sizeof buffer);
    std::string text(buffer);
    if (text.empty()) text = "mbedTLS error " + std::to_string(err);
    return text;
}

} // namespace security
} // namespace dhcp

#endif // DHCP_SECURITY_CERTERRORTEXT_H
