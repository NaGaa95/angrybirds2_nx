/* bsd_bridge.h -- real BSD sockets for AB2, bridging the game's bionic (Android)
 * socket ABI to libnx/newlib.
 *
 * Angry Birds 2 is always-online: libil2cpp (Rovio "Abba" HTTP / managed sockets)
 * AND libunity (UnityWebRequest -> Unity's bundled libcurl) both import the raw
 * Berkeley socket symbols (socket/connect/send/recv/getaddrinfo/...). The port
 * historically stubbed them all to fail so the engine stayed offline. To let the
 * Switch actually reach cloud.rovio.com and download the Skynest asset_map + the
 * ReferenceFiles/TypeRegistry bundles, we route those imports here to the real
 * libnx BSD stack (after socketInitialize), translating the ABI at the boundary.
 *
 * ABI translation is required because the game .so was compiled against bionic
 * (Linux) headers while libnx uses BSD/newlib values + layouts:
 *   - sockaddr_in: bionic {u16 family@0, port, addr, zero}; BSD {u8 len@0, u8 fam@1,...}
 *   - addrinfo:    ai_addr and ai_canonname are SWAPPED between the two
 *   - SOL_SOCKET (1 vs 0xffff), SO_x / MSG_x / O_NONBLOCK / FIONBIO all differ numerically
 */
#ifndef __BSD_BRIDGE_H__
#define __BSD_BRIDGE_H__

#include <stddef.h>

/* One-time init: nifm + socket service with enlarged buffers. Safe to call once
 * from main() before the engine boots. */
void nx_net_init(void);

/* Real socket API (bionic ABI in, libnx underneath). Signatures use void-ptr / unsigned
 * so they drop straight into the existing import-table slots without pulling bionic
 * struct definitions into every translation unit. */
int  nx_socket(int domain, int type, int protocol);
int  nx_connect(int fd, const void *addr, unsigned addrlen);
int  nx_bind(int fd, const void *addr, unsigned addrlen);
int  nx_listen(int fd, int backlog);
int  nx_accept(int fd, void *addr, void *addrlen);        /* unsigned* addrlen */
int  nx_shutdown(int fd, int how);
long nx_send(int fd, const void *buf, size_t len, int flags);
long nx_recv(int fd, void *buf, size_t len, int flags);
long nx_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen);
long nx_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, void *addrlen);
long nx_sendmsg(int fd, const void *msg, int flags);
long nx_recvmsg(int fd, void *msg, int flags);
int  nx_setsockopt(int fd, int level, int optname, const void *optval, unsigned optlen);
int  nx_getsockopt(int fd, int level, int optname, void *optval, void *optlen);
int  nx_getsockname(int fd, void *addr, void *addrlen);
int  nx_getpeername(int fd, void *addr, void *addrlen);
int  nx_getaddrinfo(const char *node, const char *service, const void *hints, void **res);
void nx_freeaddrinfo(void *res);
int  nx_getnameinfo(const void *addr, unsigned addrlen, char *host, unsigned hostlen,
                    char *serv, unsigned servlen, int flags);
void *nx_gethostbyname(const char *name);
int  nx_inet_pton(int af, const char *src, void *dst);
const char *nx_inet_ntop(int af, const void *src, char *dst, unsigned size);
unsigned nx_inet_addr(const char *cp);
int  nx_poll(void *fds, unsigned long nfds, int timeout);
int  nx_select(int nfds, void *rd, void *wr, void *ex, void *timeout);
int  nx_fcntl(int fd, int cmd, int arg);
int  nx_ioctl(int fd, unsigned long req, void *arg);

#endif /* __BSD_BRIDGE_H__ */
