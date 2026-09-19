/*
 * lwip/sockets.h shim — NuttX doesn't ship lwIP in the Metalio
 * configuration; the BSD socket API is provided by the NuttX network
 * stack via <sys/socket.h>, <sys/select.h>, <netinet/in.h>, <arpa/inet.h>,
 * <netdb.h> etc.  This header aggregates them under the "lwip/sockets.h"
 * path that media_lib_sal's media_lib_socket_reg.h expects.
 *
 * The only lwIP-specific item that media_lib_socket_reg.h reaches for is
 * a set of POSIX socket types and structs — all of which NuttX already
 * provides with the same shape (struct sockaddr, socklen_t, fd_set,
 * ssize_t, struct iovec, struct msghdr, struct timeval, ...).
 */
#ifndef __LWIP_SOCKETS_SHIM_H
#define __LWIP_SOCKETS_SHIM_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stddef.h>

/* lwIP traditionally typedef'd these to unsigned/signed compatible ints;
 * NuttX already exposes the same POSIX names in <sys/socket.h>.  If any
 * are missing fall back to standard definitions here. */

#ifndef socklen_t
typedef __socklen_t socklen_t;
#endif

#endif /* __LWIP_SOCKETS_SHIM_H */
