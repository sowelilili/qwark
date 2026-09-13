/*
 * Socket compatibility for the qwark core.
 *
 * The core writes plain BSD calls (socket/bind/listen/accept/recv/send/sendto/
 * setsockopt); this header pulls in whichever set of system headers the platform
 * needs and papers over the two differences that matter: how a socket is closed
 * and where the error number lives.
 */
#ifndef QWARK_PLAT_NET_H
#define QWARK_PLAT_NET_H

#include "plat.h"

#ifdef QWARK_HOST

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t_compat;
#define PLAT_SHUT_RDWR SD_BOTH
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#define PLAT_SHUT_RDWR SHUT_RDWR
#endif

#else /* PS3 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netex/net.h>
#include <netex/errno.h>
#include <netex/sockinfo.h>
#define PLAT_SHUT_RDWR SHUT_RDWR

#endif

/* Closes a socket descriptor. Returns 0 on success. */
int plat_socket_close(int s);

/* Shuts a socket down in both directions so a blocked call returns. */
void plat_socket_shutdown(int s);

/* Connected sockets: nonblocking I/O and bounded readiness waits. */
int plat_socket_nonblocking(int s);
int plat_socket_wait(int s, int writing, u32 ms);

/* The last socket error for this thread. */
int plat_net_errno(void);

/* Non-zero when the error means "retry", not "this socket is done". */
int plat_net_would_retry(int err);

#endif /* QWARK_PLAT_NET_H */
