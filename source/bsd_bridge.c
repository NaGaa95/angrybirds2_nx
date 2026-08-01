/* bsd_bridge.c -- real BSD sockets for AB2 (bionic ABI -> libnx/newlib). See header. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <switch.h>

/* libnx / newlib socket API (these names resolve to the REAL implementations here,
 * because this TU is compiled against newlib headers, not the game's bionic ones). */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include "bsd_bridge.h"

int fakefd_is_fake(int fd);   /* android_native_unity.h */

/* ======================================================================== *
 *  bionic (Linux) numeric constants -- the values the game .so passes us.
 *  Targets are the real newlib macros (SOL_SOCKET, SO_*, MSG_*, O_NONBLOCK...).
 * ======================================================================== */
#define BIO_AF_UNSPEC     0
#define BIO_AF_INET       2
#define BIO_AF_INET6      10

#define BIO_SOCK_STREAM   1
#define BIO_SOCK_DGRAM    2
#define BIO_SOCK_NONBLOCK 0x800     /* O_NONBLOCK, ORed into socket() type   */
#define BIO_SOCK_CLOEXEC  0x80000   /* O_CLOEXEC,  ORed into socket() type   */

#define BIO_SOL_SOCKET    1
#define BIO_IPPROTO_TCP   6         /* same on both, level for TCP_NODELAY   */
#define BIO_IPPROTO_IP    0

#define BIO_SO_REUSEADDR  2
#define BIO_SO_ERROR      4
#define BIO_SO_BROADCAST  6
#define BIO_SO_SNDBUF     7
#define BIO_SO_RCVBUF     8
#define BIO_SO_KEEPALIVE  9
#define BIO_SO_LINGER     13
#define BIO_SO_REUSEPORT  15
#define BIO_SO_RCVTIMEO   20
#define BIO_SO_SNDTIMEO   21

#define BIO_MSG_OOB       0x01
#define BIO_MSG_PEEK      0x02
#define BIO_MSG_DONTWAIT  0x40
#define BIO_MSG_WAITALL   0x100
#define BIO_MSG_NOSIGNAL  0x4000

#define BIO_O_NONBLOCK    0x800
#define BIO_F_GETFL       3
#define BIO_F_SETFL       4
#define BIO_FIONBIO       0x5421

#define BIO_AI_PASSIVE     0x0001
#define BIO_AI_CANONNAME   0x0002
#define BIO_AI_NUMERICHOST 0x0004
#define BIO_AI_NUMERICSERV 0x0008

/* ---- family ---- */
static int fam_b2n(int f) {
  switch (f) { case BIO_AF_INET: return AF_INET; case BIO_AF_INET6: return AF_INET6;
               case BIO_AF_UNSPEC: return AF_UNSPEC; default: return f; }
}
static int fam_n2b(int f) {
  switch (f) { case AF_INET: return BIO_AF_INET; case AF_INET6: return BIO_AF_INET6;
               case AF_UNSPEC: return BIO_AF_UNSPEC; default: return f; }
}

/* ---- socket() type flags ---- */
static int socktype_b2n(int t) {
  int base = t & 0xff;                    /* SOCK_STREAM/DGRAM share values */
  int out = base;
  if (t & BIO_SOCK_NONBLOCK) out |= SOCK_NONBLOCK;
  if (t & BIO_SOCK_CLOEXEC)  out |= SOCK_CLOEXEC;
  return out;
}

/* ---- send/recv flags ---- */
static int msgflags_b2n(int f) {
  int out = 0;
  if (f & BIO_MSG_OOB)      out |= MSG_OOB;
  if (f & BIO_MSG_PEEK)     out |= MSG_PEEK;
  if (f & BIO_MSG_DONTWAIT) out |= MSG_DONTWAIT;
  if (f & BIO_MSG_WAITALL)  out |= MSG_WAITALL;
  if (f & BIO_MSG_NOSIGNAL) out |= MSG_NOSIGNAL;
  return out;
}

/* ---- setsockopt/getsockopt level+name ---- */
static int level_b2n(int level) {
  if (level == BIO_SOL_SOCKET) return SOL_SOCKET;   /* 1 -> 0xffff */
  return level;                                     /* IPPROTO_TCP/IP unchanged */
}
static int sockopt_b2n(int level, int name) {
  if (level != BIO_SOL_SOCKET) return name;         /* TCP_NODELAY etc. unchanged */
  switch (name) {
    case BIO_SO_REUSEADDR: return SO_REUSEADDR;
    case BIO_SO_ERROR:     return SO_ERROR;
    case BIO_SO_BROADCAST: return SO_BROADCAST;
    case BIO_SO_SNDBUF:    return SO_SNDBUF;
    case BIO_SO_RCVBUF:    return SO_RCVBUF;
    case BIO_SO_KEEPALIVE: return SO_KEEPALIVE;
    case BIO_SO_LINGER:    return SO_LINGER;
    case BIO_SO_RCVTIMEO:  return SO_RCVTIMEO;
    case BIO_SO_SNDTIMEO:  return SO_SNDTIMEO;
#ifdef SO_REUSEPORT
    case BIO_SO_REUSEPORT: return SO_REUSEPORT;
#endif
    default: return name;
  }
}

/* ======================================================================== *
 *  sockaddr translation. Only the leading family bytes + (v6) the flow/scope
 *  fields differ; port + address are network-order at identical offsets.
 * ======================================================================== */

/* bionic -> newlib. Returns newlib socklen, or 0 if unsupported/empty. */
static socklen_t sa_b2n(const void *bsa, unsigned blen, struct sockaddr_storage *out) {
  if (!bsa || blen < 2) return 0;
  const uint8_t *b = (const uint8_t *)bsa;
  int bfam = (int)(b[0] | (b[1] << 8));            /* bionic family = u16 @0 */
  memset(out, 0, sizeof *out);
  if (bfam == BIO_AF_INET) {
    struct sockaddr_in *s = (struct sockaddr_in *)out;
    s->sin_family = AF_INET;
    memcpy(&s->sin_port, b + 2, 2);               /* network order, copy raw */
    memcpy(&s->sin_addr, b + 4, 4);
    return sizeof(struct sockaddr_in);
  }
  if (bfam == BIO_AF_INET6) {
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)out;
    s->sin6_family = AF_INET6;
    memcpy(&s->sin6_port,     b + 2,  2);
    memcpy(&s->sin6_flowinfo, b + 4,  4);
    memcpy(&s->sin6_addr,     b + 8,  16);
    if (blen >= 28) memcpy(&s->sin6_scope_id, b + 24, 4);
    return sizeof(struct sockaddr_in6);
  }
  return 0;
}

/* newlib -> bionic. Writes up to *bcap bytes into bsa, sets *bcap to bytes written. */
static void sa_n2b(const struct sockaddr *nsa, void *bsa, unsigned *bcap) {
  if (!bsa || !bcap) { if (bcap) *bcap = 0; return; }
  uint8_t *b = (uint8_t *)bsa;
  unsigned cap = *bcap;
  if (!nsa) { *bcap = 0; return; }
  if (nsa->sa_family == AF_INET && cap >= sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *s = (const struct sockaddr_in *)nsa;
    memset(b, 0, sizeof(struct sockaddr_in));
    b[0] = BIO_AF_INET & 0xff; b[1] = (BIO_AF_INET >> 8) & 0xff;
    memcpy(b + 2, &s->sin_port, 2);
    memcpy(b + 4, &s->sin_addr, 4);
    *bcap = sizeof(struct sockaddr_in);
  } else if (nsa->sa_family == AF_INET6 && cap >= 28) {
    const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)nsa;
    memset(b, 0, 28);
    b[0] = BIO_AF_INET6 & 0xff; b[1] = (BIO_AF_INET6 >> 8) & 0xff;
    memcpy(b + 2,  &s->sin6_port,     2);
    memcpy(b + 4,  &s->sin6_flowinfo, 4);
    memcpy(b + 8,  &s->sin6_addr,     16);
    memcpy(b + 24, &s->sin6_scope_id, 4);
    *bcap = 28;
  } else {
    *bcap = 0;
  }
}

/* ======================================================================== *
 *  init
 * ======================================================================== */
void nx_net_init(void) {
  nifmInitialize(NifmServiceType_User);

  /* Enlarged socket buffers: the nxlink-tier defaults ZeroWindow-throttle CDN
   * pulls to a crawl. 1 MB TCP windows + more sessions for parallel bundle DLs. */
  static u8 sockpool[1024 * 1024] __attribute__((aligned(0x1000)));
  SocketInitConfig cfg = {
    .tcp_tx_buf_size     = 0x8000,
    .tcp_rx_buf_size     = 0x10000,
    .tcp_tx_buf_max_size = 0x40000,
    .tcp_rx_buf_max_size = 0x100000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 16,
    .bsd_service_type = BsdServiceType_User,
  };
  (void)sockpool;
  Result rc = socketInitialize(&cfg);
  if (R_FAILED(rc)) {
    socketInitializeDefault();
  }
}

/* ======================================================================== *
 *  socket calls
 * ======================================================================== */
int nx_socket(int domain, int type, int protocol) {
  return socket(fam_b2n(domain), socktype_b2n(type), protocol);
}

int nx_connect(int fd, const void *addr, unsigned addrlen) {
  struct sockaddr_storage ss; socklen_t nl = sa_b2n(addr, addrlen, &ss);
  if (!nl) { errno = EAFNOSUPPORT; return -1; }
  int r = connect(fd, (struct sockaddr *)&ss, nl);
  return r;
}

int nx_bind(int fd, const void *addr, unsigned addrlen) {
  struct sockaddr_storage ss; socklen_t nl = sa_b2n(addr, addrlen, &ss);
  if (!nl) { errno = EAFNOSUPPORT; return -1; }
  return bind(fd, (struct sockaddr *)&ss, nl);
}

int nx_listen(int fd, int backlog) { return listen(fd, backlog); }

int nx_accept(int fd, void *addr, void *addrlen) {
  struct sockaddr_storage ss; socklen_t nl = sizeof ss;
  int c = accept(fd, (struct sockaddr *)&ss, &nl);
  if (c >= 0 && addr && addrlen) { unsigned cap = *(unsigned *)addrlen;
    sa_n2b((struct sockaddr *)&ss, addr, &cap); *(unsigned *)addrlen = cap; }
  return c;
}

int nx_shutdown(int fd, int how) { return shutdown(fd, how); }

long nx_send(int fd, const void *buf, size_t len, int flags) {
  return send(fd, buf, len, msgflags_b2n(flags));
}
long nx_recv(int fd, void *buf, size_t len, int flags) {
  return recv(fd, buf, len, msgflags_b2n(flags));
}
long nx_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen) {
  if (!addr || !addrlen) return send(fd, buf, len, msgflags_b2n(flags));
  struct sockaddr_storage ss; socklen_t nl = sa_b2n(addr, addrlen, &ss);
  if (!nl) { errno = EAFNOSUPPORT; return -1; }
  return sendto(fd, buf, len, msgflags_b2n(flags), (struct sockaddr *)&ss, nl);
}
long nx_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, void *addrlen) {
  struct sockaddr_storage ss; socklen_t nl = sizeof ss;
  long n = recvfrom(fd, buf, len, msgflags_b2n(flags), (struct sockaddr *)&ss, &nl);
  if (n >= 0 && addr && addrlen) { unsigned cap = *(unsigned *)addrlen;
    sa_n2b((struct sockaddr *)&ss, addr, &cap); *(unsigned *)addrlen = cap; }
  return n;
}

/* msghdr has the same field layout on bionic/newlib (LP64); only msg_name (a
 * sockaddr) and msg_flags differ. Translate name in place via a scratch copy. */
struct nx_msghdr { void *msg_name; unsigned msg_namelen; void *msg_iov; size_t msg_iovlen;
                   void *msg_control; size_t msg_controllen; int msg_flags; };
long nx_sendmsg(int fd, const void *msg, int flags) {
  const struct nx_msghdr *m = (const struct nx_msghdr *)msg;
  struct msghdr nm; memset(&nm, 0, sizeof nm);
  struct sockaddr_storage ss; socklen_t nl = 0;
  if (m->msg_name && m->msg_namelen) { nl = sa_b2n(m->msg_name, m->msg_namelen, &ss);
    nm.msg_name = &ss; nm.msg_namelen = nl; }
  nm.msg_iov = (struct iovec *)m->msg_iov; nm.msg_iovlen = m->msg_iovlen;
  nm.msg_control = m->msg_control; nm.msg_controllen = m->msg_controllen;
  return sendmsg(fd, &nm, msgflags_b2n(flags));
}
long nx_recvmsg(int fd, void *msg, int flags) {
  struct nx_msghdr *m = (struct nx_msghdr *)msg;
  struct msghdr nm; memset(&nm, 0, sizeof nm);
  struct sockaddr_storage ss;
  if (m->msg_name && m->msg_namelen) { nm.msg_name = &ss; nm.msg_namelen = sizeof ss; }
  nm.msg_iov = (struct iovec *)m->msg_iov; nm.msg_iovlen = m->msg_iovlen;
  nm.msg_control = m->msg_control; nm.msg_controllen = m->msg_controllen;
  long n = recvmsg(fd, &nm, msgflags_b2n(flags));
  if (n >= 0 && m->msg_name && m->msg_namelen && nm.msg_namelen) {
    unsigned cap = m->msg_namelen; sa_n2b((struct sockaddr *)&ss, m->msg_name, &cap);
    m->msg_namelen = cap;
  }
  return n;
}

int nx_setsockopt(int fd, int level, int optname, const void *optval, unsigned optlen) {
  int nl = level_b2n(level), nn = sockopt_b2n(level, optname);
  /* SO_RCVTIMEO/SO_SNDTIMEO carry a struct timeval -- identical layout, pass through */
  int r = setsockopt(fd, nl, nn, optval, optlen);
  if (r < 0) return 0;
  return 0;
}
int nx_getsockopt(int fd, int level, int optname, void *optval, void *optlen) {
  int nl = level_b2n(level), nn = sockopt_b2n(level, optname);
  return getsockopt(fd, nl, nn, optval, (socklen_t *)optlen);
}
int nx_getsockname(int fd, void *addr, void *addrlen) {
  struct sockaddr_storage ss; socklen_t nl = sizeof ss;
  int r = getsockname(fd, (struct sockaddr *)&ss, &nl);
  if (r == 0 && addr && addrlen) { unsigned cap = *(unsigned *)addrlen;
    sa_n2b((struct sockaddr *)&ss, addr, &cap); *(unsigned *)addrlen = cap; }
  return r;
}
int nx_getpeername(int fd, void *addr, void *addrlen) {
  struct sockaddr_storage ss; socklen_t nl = sizeof ss;
  int r = getpeername(fd, (struct sockaddr *)&ss, &nl);
  if (r == 0 && addr && addrlen) { unsigned cap = *(unsigned *)addrlen;
    sa_n2b((struct sockaddr *)&ss, addr, &cap); *(unsigned *)addrlen = cap; }
  return r;
}

/* ======================================================================== *
 *  getaddrinfo -- rebuild the result list in the exact layout consumed by
 *  AB2's libil2cpp, with translated families and sockaddrs.
 *
 *  This Unity/Mono PAL caller does not use bionic's public-header field order.
 *  Its native code at libil2cpp+0x0428C1FC/+0x0428C210/+0x0428C288 reads:
 *    ai_family    @ +0x04
 *    ai_canonname @ +0x18
 *    ai_addr      @ +0x20
 *    ai_next      @ +0x28
 */
struct bio_addrinfo {
  int ai_flags; int ai_family; int ai_socktype; int ai_protocol;
  unsigned ai_addrlen; int _pad;
  char *ai_canonname; void *ai_addr; struct bio_addrinfo *ai_next;
};
_Static_assert(offsetof(struct bio_addrinfo, ai_canonname) == 0x18, "AB2 addrinfo canonname ABI");
_Static_assert(offsetof(struct bio_addrinfo, ai_addr)      == 0x20, "AB2 addrinfo address ABI");
_Static_assert(offsetof(struct bio_addrinfo, ai_next)      == 0x28, "AB2 addrinfo next ABI");

int nx_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
  if (res) *res = NULL;
  struct addrinfo nh; struct addrinfo *nhp = NULL;
  if (hints) {
    const struct bio_addrinfo *bh = (const struct bio_addrinfo *)hints;
    memset(&nh, 0, sizeof nh);
    nh.ai_family   = fam_b2n(bh->ai_family);
    nh.ai_socktype = bh->ai_socktype & 0xff;
    nh.ai_protocol = bh->ai_protocol;
    if (bh->ai_flags & BIO_AI_PASSIVE)     nh.ai_flags |= AI_PASSIVE;
    if (bh->ai_flags & BIO_AI_CANONNAME)   nh.ai_flags |= AI_CANONNAME;
    if (bh->ai_flags & BIO_AI_NUMERICHOST) nh.ai_flags |= AI_NUMERICHOST;
    nhp = &nh;
  }
  struct addrinfo *nres = NULL;
  int rc = getaddrinfo(node, service, nhp, &nres);
  if (rc != 0 || !nres) {
    return rc ? rc : EAI_FAIL;
  }
  struct bio_addrinfo *head = NULL, *tail = NULL;
  for (struct addrinfo *p = nres; p; p = p->ai_next) {
    struct bio_addrinfo *b = (struct bio_addrinfo *)calloc(1, sizeof *b);
    if (!b) break;
    b->ai_flags    = 0;
    b->ai_family   = fam_n2b(p->ai_family);
    b->ai_socktype = p->ai_socktype;
    b->ai_protocol = p->ai_protocol;
    if (p->ai_addr && p->ai_addrlen) {
      void *ba = calloc(1, 28);
      unsigned cap = 28; sa_n2b(p->ai_addr, ba, &cap);
      if (cap) { b->ai_addr = ba; b->ai_addrlen = cap; } else free(ba);
    }
    if (p->ai_canonname && !head) b->ai_canonname = strdup(p->ai_canonname);
    if (tail) tail->ai_next = b; else head = b;
    tail = b;
  }
  freeaddrinfo(nres);
  if (!head) return EAI_FAIL;
  if (res) *res = head;
  return 0;
}

void nx_freeaddrinfo(void *res) {
  struct bio_addrinfo *p = (struct bio_addrinfo *)res;
  while (p) { struct bio_addrinfo *n = p->ai_next;
    if (p->ai_addr) free(p->ai_addr);
    if (p->ai_canonname) free(p->ai_canonname);
    free(p); p = n; }
}

int nx_getnameinfo(const void *addr, unsigned addrlen, char *host, unsigned hostlen,
                   char *serv, unsigned servlen, int flags) {
  struct sockaddr_storage ss; socklen_t nl = sa_b2n(addr, addrlen, &ss);
  if (!nl) { if (host && hostlen) host[0] = 0; if (serv && servlen) serv[0] = 0; return EAI_FAMILY; }
  return getnameinfo((struct sockaddr *)&ss, nl, host, hostlen, serv, servlen, flags);
}

void *nx_gethostbyname(const char *name) {
  /* Deliberately unsupported: the legacy hostent API is not thread-safe and the
   * game only needs it as a fallback. Return NULL so callers use getaddrinfo. */
  (void)name; return NULL;
}

int nx_inet_pton(int af, const char *src, void *dst) { return inet_pton(fam_b2n(af), src, dst); }
const char *nx_inet_ntop(int af, const void *src, char *dst, unsigned size) {
  return inet_ntop(fam_b2n(af), src, dst, size);
}
unsigned nx_inet_addr(const char *cp) { return (unsigned)inet_addr(cp); }

int nx_poll(void *fds, unsigned long nfds, int timeout) {
  /* struct pollfd {int fd; short events; short revents} + POLLIN/OUT bits are
   * identical on bionic and newlib -- pass straight through. */
  return poll((struct pollfd *)fds, (nfds_t)nfds, timeout);
}
int nx_select(int nfds, void *rd, void *wr, void *ex, void *timeout) {
  return select(nfds, (fd_set *)rd, (fd_set *)wr, (fd_set *)ex, (struct timeval *)timeout);
}

int nx_fcntl(int fd, int cmd, int arg) {
  if (fakefd_is_fake(fd)) return 0;   /* pipe/synthetic fds: keep old no-op */
  if (cmd == BIO_F_GETFL) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return 0;
    int out = 0; if (fl & O_NONBLOCK) out |= BIO_O_NONBLOCK; return out;
  }
  if (cmd == BIO_F_SETFL) {
    int nfl = fcntl(fd, F_GETFL, 0); if (nfl < 0) nfl = 0;
    if (arg & BIO_O_NONBLOCK) nfl |= O_NONBLOCK; else nfl &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, nfl);
  }
  return 0;
}

int nx_ioctl(int fd, unsigned long req, void *arg) {
  if (fakefd_is_fake(fd)) return -1;
  if (req == (unsigned long)BIO_FIONBIO) {   /* set/clear non-blocking */
    int on = arg ? *(int *)arg : 0, fl = fcntl(fd, F_GETFL, 0); if (fl < 0) fl = 0;
    if (on) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl);
  }
  return ioctl(fd, req, arg);
}
