#ifndef DHCP_TEST_STUB_LWIP_INET_H
#define DHCP_TEST_STUB_LWIP_INET_H

/**
 * @file inet.h
 * @brief Host stand-in for lwIP's inet.h.
 *
 * The code under test uses `inet_pton` / `inet_ntop` and the address family
 * constants. Windows provides exactly those in `ws2tcpip.h`, so the shim only
 * has to bring them in (link with `-lws2_32`).
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#endif // DHCP_TEST_STUB_LWIP_INET_H
