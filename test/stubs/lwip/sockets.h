#ifndef DHCP_TEST_STUB_LWIP_SOCKETS_H
#define DHCP_TEST_STUB_LWIP_SOCKETS_H

/**
 * @file sockets.h
 * @brief Host stand-in for lwIP's sockets.h.
 *
 * Nothing in the code under test opens a socket here — the header is included
 * for the address-family constants and the address structures, which
 * `ws2tcpip.h` already provides on Windows.
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#endif // DHCP_TEST_STUB_LWIP_SOCKETS_H
