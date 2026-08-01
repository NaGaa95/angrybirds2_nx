/* Dynamic-symbol resolution for the Android game libraries.
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (FF4 base)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Module-to-module C++ symbols resolve from libc++_shared.so. This table supplies
 * the bionic/newlib shims, graphics, audio, Android NDK, logging, and ABI helpers.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
extern int z_strncasecmp(const char *, const char *, unsigned long);  /* NULL-safe, unity_imports.c */
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>
#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "opensles.h"
#include "imports.h"
#include "android_native.h"
#include "android_media_stubs.h"
#include "unity_imports.h"

extern int *__errno(void);              // newlib

/* Android/bionic and devkitA64/newlib assign different numeric clock IDs for
 * the two clocks used most heavily by Unity:
 *
 *   Android CLOCK_REALTIME  = 0   newlib CLOCK_REALTIME  = 1
 *   Android CLOCK_MONOTONIC = 1   newlib CLOCK_MONOTONIC = 4
 *
 * AB2 also calls Android CLOCK_BOOTTIME (7) from HatchServerTime's
 * __getMonotonicBootTimeMillis. Passing that ID directly to Switch can fail and
 * leaves the caller's stack timespec undefined; the resulting garbage double
 * overflows DateTime.AddMilliseconds. Translate the Android ABI explicitly and
 * zero the output first so even an unexpected host error cannot leak garbage. */
static int clock_gettime_bionic(int android_id, struct timespec *tp) {
  if (!tp) {
    errno = EINVAL;
    return -1;
  }
  tp->tv_sec = 0;
  tp->tv_nsec = 0;

  clockid_t host_id;
  switch (android_id) {
    case 0:  /* CLOCK_REALTIME */
    case 5:  /* CLOCK_REALTIME_COARSE */
    case 8:  /* CLOCK_REALTIME_ALARM */
    case 11: /* CLOCK_TAI: realtime is the closest available clock */
      host_id = CLOCK_REALTIME;
      break;
    case 1: /* CLOCK_MONOTONIC */
    case 2: /* CLOCK_PROCESS_CPUTIME_ID: elapsed monotonic is sufficient here */
    case 3: /* CLOCK_THREAD_CPUTIME_ID */
    case 4: /* CLOCK_MONOTONIC_RAW */
    case 6: /* CLOCK_MONOTONIC_COARSE */
    case 7: /* CLOCK_BOOTTIME */
    case 9: /* CLOCK_BOOTTIME_ALARM */
      host_id = CLOCK_MONOTONIC;
      break;
    default:
      errno = EINVAL;
      return -1;
  }

  int rc = clock_gettime(host_id, tp);
  return rc;
}

// ---------------------------------------------------------------------------
// liblog
// ---------------------------------------------------------------------------

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; (void)tag; (void)fmt;
  return 0;
}
int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio;  return 0;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio; (void)tag; (void)fmt; (void)va;
  return 0;
}
void __assert2(const char *file, int line, const char *func, const char *expr) {
   abort();
}

// ---------------------------------------------------------------------------
// stack protector / cxxabi
// ---------------------------------------------------------------------------

uint64_t __stack_chk_guard_fake = 0x0ull; /* match install_bionic_tls's zeroed tpidr+0x28 slot */
void __stack_chk_fail_fake(void) {  abort(); }

int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void __cxa_finalize_fake(void *dso) { (void)dso; }

// stdin/stdout/stderr point into the fake __sF block (see libc_shim.c)
FILE *stderr_fake = (FILE *)&fake_sF[2];

// ---------------------------------------------------------------------------
// pthread: bionic allocates the opaque types inline and zero-inits them, so we
// lazily back them with heap-allocated newlib objects stashed through the
// caller's pointer slot.
// ---------------------------------------------------------------------------

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (attr && *attr == 1); // bionic PTHREAD_MUTEX_RECURSIVE == 1
  int ret;
  if (recursive) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m; return 0;
}
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) { pthread_mutex_destroy(*uid); free(*uid); *uid = NULL; }
  return 0;
}
static int ensure_mutex(pthread_mutex_t **uid) {
  // A real (already-initialized) handle is a heap pointer; anything small is a
  // bionic static initializer left in place: 0 = normal, 0x4000 = recursive,
  // 0x8000 = errorcheck, etc. Back ALL of those with a real newlib mutex.
  // (Was only handling 0 and 0x4000 -> a 0x8000 errorcheck static was treated
  // as a valid pointer and locked at address 0x8000: wild deref / hang.)
  if ((uintptr_t)*uid < 0x10000) {
    int recursive = ((uintptr_t)*uid == 0x4000);
    int a = 1;
    return pthread_mutex_init_fake(uid, recursive ? &a : NULL);
  }
  return 0;
}
int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  if (ensure_mutex(uid) < 0) return -1;
  if (pthread_mutex_trylock(*uid) == 0) return 0;
  return pthread_mutex_lock(*uid);
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) { if (ensure_mutex(uid) < 0) return -1; return pthread_mutex_trylock(*uid); }
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) { if (ensure_mutex(uid) < 0) return -1; return pthread_mutex_unlock(*uid); }
int pthread_mutex_timedlock_fake(pthread_mutex_t **uid, const struct timespec *abs) {
  (void)abs;
  if (ensure_mutex(uid) < 0) return -1;
  for (int i = 0; i < 1000; i++) {
    if (pthread_mutex_trylock(*uid) == 0) return 0;
    svcSleepThread(1000000ull);
  }
  return ETIMEDOUT;
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  (void)attr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) < 0) { free(c); return -1; }
  *cnd = c; return 0;
}
static int ensure_cond(pthread_cond_t **cnd) {
  // like ensure_mutex: any small value is a bionic static cond initializer that
  // needs a real newlib object (was only treating 0 as needing init, so a
  // non-zero sentinel was waited/signalled on as a garbage pointer).
  if ((uintptr_t)*cnd < 0x10000) return pthread_cond_init_fake(cnd, NULL);
  return 0;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_broadcast(*cnd); }
int pthread_cond_signal_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_signal(*cnd); }
int pthread_cond_destroy_fake(pthread_cond_t **cnd) { if (cnd && (uintptr_t)*cnd >= 0x10000) { pthread_cond_destroy(*cnd); free(*cnd); *cnd = NULL; } return 0; }
#define COND_WAIT_CAP_MS 16
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  // Cap the UNTIMED wait too (not just the timed one below). Unity's engine
  // (job system, GfxDevice sync, PreloadManager) blocks the main thread in a
  // plain pthread_cond_wait; a raced/lost pthread_cond_signal -- or a
  // bionic-static-cond object mismatch across signal/wait -- would then hang the
  // whole engine forever (every worker idle, UnityMain parked in
  // svcWaitProcessWideKeyAtomic == the observed frame-2 deadlock). Waking every
  // ~16ms and returning as a spurious wakeup lets the caller re-check its
  // predicate (POSIX-legal: correct waiters always loop on the predicate), which
  // breaks a lost-wakeup stall without changing correct behaviour.
  struct timespec cap;
  clock_gettime(CLOCK_MONOTONIC, &cap);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  += (cap.tv_nsec + add) / 1000000000L;
  cap.tv_nsec  = (cap.tv_nsec + add) % 1000000000L;
  int r = pthread_cond_timedwait(*cnd, *mtx, &cap);
  return (r == ETIMEDOUT) ? 0 : r;   // timeout -> report as spurious wakeup
}
// Bound every timed cond-wait to at most COND_WAIT_CAP_MS. The .so libs (libc++
// std::condition_variable, and Swappy's frame pacer) compute an ABSOLUTE deadline
// against CLOCK_MONOTONIC, but newlib/libnx's pthread_cond_timedwait may measure
// "now" against a different clock -- a mismatch turns a ~16 ms vsync wait into an
// effectively infinite one (the Swappy hang: engine wedged in condvarWaitTimeout,
// frame counter frozen, black screen). Re-deriving the deadline as
// now(MONOTONIC)+min(requested, CAP) guarantees the wait can't exceed the cap
// regardless of which clock newlib uses, so the pacer hits its timeout fallback
// and keeps pacing. Spurious/early wakeups are POSIX-legal (every correct waiter
// re-checks its predicate), so this is safe in general.
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  struct timespec now, cap;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  = now.tv_sec + (now.tv_nsec + add) / 1000000000L;
  cap.tv_nsec = (now.tv_nsec + add) % 1000000000L;
  // honor the caller's deadline if it's sooner than our cap; else clamp to cap
  const struct timespec *use = &cap;
  if (t && (t->tv_sec < cap.tv_sec ||
            (t->tv_sec == cap.tv_sec && t->tv_nsec <= cap.tv_nsec)))
    use = t;
  int r = pthread_cond_timedwait(*cnd, *mtx, use);
  return r;
}

int pthread_once_fake(volatile int *once, void (*init)(void)) {
  if (!once || !init) return -1;
  if (__sync_lock_test_and_set(once, 1) == 0) (*init)();
  return 0;
}

int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int t) { if (a) *a = t; return 0; }

// bionic pthread_attr_t is opaque storage we own; stash size/detach there
#define ATTR_MAGIC 0x41545452 /* 'ATTR' */
typedef struct { uint32_t magic; uint32_t detach; size_t stacksize; } OurAttr;

int pthread_attr_init_fake(void *a) { if (a) { OurAttr *o = a; o->magic = ATTR_MAGIC; o->detach = 0; o->stacksize = 0; } return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->detach = (uint32_t)s; } return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->stacksize = s; } return 0; }
int pthread_attr_getstacksize_fake(const void *a, size_t *s) { if (s) { const OurAttr *o = a; *s = (a && o->magic == ATTR_MAGIC && o->stacksize) ? o->stacksize : (512 * 1024); } return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

typedef struct { void *(*entry)(void *); void *arg; uint8_t tls[BIONIC_TLS_SIZE]; } ThreadStart;
static void *thread_trampoline(void *p) {
  ThreadStart *ts = (ThreadStart *)p;   /* leaked on purpose: tpidr points into ts->tls */
  install_bionic_tls(ts->tls);
  return ts->entry(ts->arg);
}
int pthread_create_fake(pthread_t *thread, const void *bionic_attr, void *entry, void *arg) {
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  size_t stack = 0;
  if (bionic_attr) {
    const OurAttr *o = bionic_attr;
    if (o->magic == ATTR_MAGIC) stack = o->stacksize;
  }
  if (stack < (2u << 20)) stack = 2u << 20; // 2 MB floor for the heavy engine threads
  pthread_attr_t attr; pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  const int r = pthread_create(thread, &attr, thread_trampoline, ts);
  pthread_attr_destroy(&attr);
  if (r != 0) { free(ts); return r; }
  return 0;
}
int pthread_join_fake(pthread_t thread, void **retval) {
  return pthread_join(thread, retval);
}
int pthread_setschedparam_fake(pthread_t t, int policy, const void *p) { (void)t; (void)policy; (void)p; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *old) { (void)how; (void)set; (void)old; return 0; }
int pthread_kill_fake(pthread_t t, int sig) { (void)t; (void)sig; return 0; }

// ---------------------------------------------------------------------------
// pthread TLS keys, multiplexed over a single real newlib key.
// devkitA64 backs pthread keys with a tiny pool (~16 libnx TLS slots), but
// Unity's runtime creates dozens during init (46 call sites). The ~17th
// pthread_key_create returns EAGAIN, and libunity treats that as fatal
// (asserts the key was created, else BRK). bionic allows 128 keys; emulate
// that: one real key holds a per-thread value array for up to 128 fake keys.
// ---------------------------------------------------------------------------
#define FAKE_KEYS_MAX 128
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; void (*dtor)(void *); } g_key_table[FAKE_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_master_key_ready;
typedef struct { void *values[FAKE_KEYS_MAX]; } KeyValues;

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  for (int iter = 0; iter < 4; iter++) {     // POSIX: rerun while dtors set new values
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      if (g_key_table[i].used && g_key_table[i].dtor && v) {
        kv->values[i] = NULL;
        g_key_table[i].dtor(v);
        again = 1;
      }
    }
    if (!again) break;
  }
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  pthread_mutex_lock(&g_key_mutex);
  if (!g_master_key_ready) {
    if (pthread_key_create(&g_master_key, master_key_dtor) != 0) {
      pthread_mutex_unlock(&g_key_mutex);
      
      return EAGAIN;
    }
    g_master_key_ready = 1;
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!g_key_table[i].used) {
      g_key_table[i].used = 1;
      g_key_table[i].dtor = dtor;
      *key = i + 1;                 // 1-based: a zeroed key is invalid
      pthread_mutex_unlock(&g_key_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_key_mutex);
  
  return EAGAIN;
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX) return EINVAL;
  pthread_mutex_lock(&g_key_mutex);
  g_key_table[key - 1].used = 0;
  g_key_table[key - 1].dtor = NULL;
  pthread_mutex_unlock(&g_key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return NULL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  return kv ? kv->values[key - 1] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return EINVAL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv) return ENOMEM;
    pthread_setspecific(g_master_key, kv);
  }
  kv->values[key - 1] = (void *)value;
  return 0;
}

// ---------------------------------------------------------------------------
// misc small shims
// ---------------------------------------------------------------------------

static int ret0_i(void) { return 0; }
static int retm1_i(void) { return -1; }
static unsigned ret0_u(void) { return 0; }
static int signal_stub(int s, void *h) { (void)s; (void)h; return 0; }
static int sigaction_stub(int s, const void *a, void *o) { (void)s; (void)a; (void)o; return 0; }
/* Real networking: route fcntl/ioctl to the libnx BSD bridge so O_NONBLOCK / FIONBIO
 * on socket fds actually take effect (curl's non-blocking connect path needs this).
 * nx_* guard synthetic pipe fds and translate bionic constants -> newlib. */
extern int nx_fcntl(int fd, int cmd, int arg);
extern int nx_ioctl(int fd, unsigned long req, void *arg);
static int ioctl_stub(int fd, unsigned long req, ...) {
  va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
  return nx_ioctl(fd, req, arg);
}
static int fcntl_stub(int fd, int cmd, ...) {
  va_list ap; va_start(ap, cmd); int arg = va_arg(ap, int); va_end(ap);
  return nx_fcntl(fd, cmd, arg);
}
static int tcgetattr_stub(int fd, void *t) { (void)fd; if (t) memset(t, 0, 60); return 0; }
static int tcsetattr_stub(int fd, int opt, const void *t) { (void)fd; (void)opt; (void)t; return 0; }

// POSIX file ops that devkitA64 newlib/libnx may not provide -- implement or
// stub them locally so the link never depends on their presence.
static int access_impl(const char *path, int mode) {
  (void)mode; struct stat st; return stat(path, &st) == 0 ? 0 : -1;
}
static int chmod_stub(const char *path, int mode) { (void)path; (void)mode; return 0; }
static int truncate_stub(const char *path, long len) { (void)path; (void)len; return 0; }
static int ftruncate_stub(int fd, long len) { (void)fd; (void)len; return 0; }
static int fsync_stub(int fd) { (void)fd; return 0; }
static int dup2_stub(int a, int b) { (void)a; return b; }
static long pread_impl(int fd, void *buf, size_t n, long off) {
  return pread_fake(fd, buf, n, off);
}
static long pwrite_impl(int fd, const void *buf, size_t n, long off) {
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, off, SEEK_SET) < 0) return -1;
  long r = write(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}
static int uname_fake(void *buf) { if (buf) memset(buf, 0, 390); return 0; }
static long sysconf_pass(int n) { return sysconf_fake(n); }
static char *g_tzname_fake[2] = { (char *)"UTC", (char *)"UTC" };
static int readlink_stub(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
static int link_stub(const char *a, const char *b) { (void)a; (void)b; return -1; }
static int symlink_stub(const char *a, const char *b) { (void)a; (void)b; return -1; }
static int fchmod_stub(int fd, int m) { (void)fd; (void)m; return 0; }
static int fchmodat_stub(int d, const char *p, int m, int f) { (void)d; (void)p; (void)m; (void)f; return 0; }
static int utimensat_stub(int d, const char *p, const void *t, int f) { (void)d; (void)p; (void)t; (void)f; return 0; }
static long sendfile_stub(int o, int i, long *off, size_t c) { (void)o; (void)i; (void)off; (void)c; return -1; }
static void *fdopendir_stub(int fd) { (void)fd; return NULL; }
static char *inet_ntoa_stub(uint32_t in) { (void)in; static char s[] = "0.0.0.0"; return s; }
static void _exit_fake(int code) { (void)code; extern void NX_NORETURN __libnx_exit(int rc); __libnx_exit(0); }

/* NULL-safe string ops. Unity's SystemInfo device-detection feeds the device
 * model/manufacturer (NULL in our fake JNI) straight into compare/search libc
 * calls; raw newlib then deref-NULL-crashes (this is what killed strcasecmp).
 * Treat NULL as an empty string: comparisons are unequal, searches find nothing,
 * length is zero -- so the device checks all fall through harmlessly. */
static int z_strcmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}
static int z_strncmp(const char *a, const char *b, size_t n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncmp(a, b, n);
}
static char *z_strstr(const char *h, const char *n) {
  if (!h || !n) return NULL;
  return strstr(h, n);
}
static char *z_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
static char *z_strrchr(const char *s, int c) { return s ? strrchr(s, c) : NULL; }
static size_t z_strlen(const char *s) { return s ? strlen(s) : 0; }

// engine-facing NDK functions implemented elsewhere ------------------------
// libandroid (android_native.c)
extern void *ALooper_prepare(int);
extern int   ALooper_addFd(void *, int, int, int, void *, void *);
extern int   ALooper_pollOnce(int, int *, int *, void **);
extern void  AInputQueue_attachLooper(void *, void *, int, void *, void *);
extern void  AInputQueue_detachLooper(void *);
extern int32_t AInputQueue_getEvent(void *, void **);
extern int32_t AInputQueue_preDispatchEvent(void *, void *);
extern void  AInputQueue_finishEvent(void *, void *, int);
extern int32_t AInputEvent_getType(const void *);
extern int32_t AMotionEvent_getAction(const void *);
extern size_t  AMotionEvent_getPointerCount(const void *);
extern int32_t AMotionEvent_getPointerId(const void *, size_t);
extern float   AMotionEvent_getX(const void *, size_t);
extern float   AMotionEvent_getY(const void *, size_t);
extern int32_t AKeyEvent_getKeyCode(const void *);
extern int32_t AKeyEvent_getFlags(const void *);
extern int32_t AKeyEvent_getRepeatCount(const void *);
extern int32_t ANativeWindow_setBuffersGeometry(void *, int32_t, int32_t, int32_t);
extern void *AConfiguration_new(void);
extern void  AConfiguration_fromAssetManager(void *, void *);
extern void  AConfiguration_getLanguage(void *, char *);
extern void  AConfiguration_getCountry(void *, char *);
extern void  AConfiguration_delete(void *);
extern int   ASensorEventQueue_enableSensor(void *, const void *);
extern int   ASensorEventQueue_disableSensor(void *, const void *);
extern int   ASensorEventQueue_setEventRate(void *, const void *, int32_t);
extern int   ASensorEventQueue_getEvents(void *, void *, size_t);
// AAsset compatibility
extern void *AAssetManager_fromJava(void *, void *);
extern void *AAssetManager_open(void *, const char *, int);
extern const void *AAsset_getBuffer(void *);
extern int64_t AAsset_getLength(void *);
extern void  AAsset_close(void *);
// AndroidBitmap compatibility
extern int AndroidBitmap_getInfo(void *, void *, void *);
extern int AndroidBitmap_lockPixels(void *, void *, void **);
extern int AndroidBitmap_unlockPixels(void *, void *);

// ---------------------------------------------------------------------------
// GL shader fixups
// ---------------------------------------------------------------------------

// The MVGL "mono" composite shader assigns `monoCol` without declaring it.
// Android's GL driver tolerated it; mesa's stricter GLSL ES compiler rejects it,
// which kills the offscreen->screen copy and leaves the panel black. Inject the
// missing declaration at upload time.
static char *shader_fixups(const char *src) {
  const char *find = "monoCol = vec3";
  char *pos = strstr(src, find);
  if (!pos) return NULL;
  const char *repl = "vec3 monoCol = vec3";
  const size_t pre = (size_t)(pos - src), flen = strlen(find), rlen = strlen(repl);
  char *out = malloc(strlen(src) + (rlen - flen) + 1);
  if (!out) return NULL;
  memcpy(out, src, pre);
  memcpy(out + pre, repl, rlen);
  strcpy(out + pre + rlen, pos + flen);
  return out;
}
static void gl_ShaderSource_log(GLuint sh, GLsizei count, const GLchar *const *strs, const GLint *lens) {
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++)
    total += (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
  char *buf = malloc(total + 1);
  if (!buf) { glShaderSource(sh, count, strs, lens); return; }
  size_t off = 0;
  for (GLsizei i = 0; i < count; i++) {
    size_t l = (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
    memcpy(buf + off, strs[i], l); off += l;
  }
  buf[off] = 0;
  char *fixed = shader_fixups(buf);
  const GLchar *final_src = fixed ? fixed : buf;
  glShaderSource(sh, 1, &final_src, NULL);
  free(buf);
  free(fixed);
}

// ---------------------------------------------------------------------------
// EGL surface-size fixup
// ---------------------------------------------------------------------------

// switch-mesa returns 0 for EGL_WIDTH/EGL_HEIGHT on the window surface even
// though it renders full-screen; the engine trusts that and sets a 0x0 viewport
// (nothing draws). Hand back the real panel size instead.
static EGLBoolean egl_QuerySurface_log(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val) {
  EGLBoolean r = eglQuerySurface(d, s, attr, val);
  if (val) {
    if (attr == 0x3057 /*EGL_WIDTH*/  && *val <= 0) { *val = screen_width;  r = EGL_TRUE; }
    if (attr == 0x3056 /*EGL_HEIGHT*/ && *val <= 0) { *val = screen_height; r = EGL_TRUE; }
  }
  return r;
}

static void gl_Viewport_log(GLint x, GLint y, GLsizei w, GLsizei h) {
  glViewport(x, y, w, h);
}
static void gl_DrawArrays_log(GLenum m, GLint f, GLsizei c) { glDrawArrays(m, f, c); }
static void gl_DrawElements_log(GLenum m, GLsizei c, GLenum t, const void *i) { glDrawElements(m, c, t, i); }

extern void android_native_draw_cursor(void);   /* docked cursor overlay (android_native_unity.c) */
static EGLBoolean egl_SwapBuffers_log(EGLDisplay d, EGLSurface s) {
  android_native_draw_cursor();          /* overlay the docked cursor, then present */
  return eglSwapBuffers(d, s);
}

// ---------------------------------------------------------------------------
// Unity 6 / AB2: NDK AChoreographer + window + misc stubs. Unity 6 drives frame
// timing via the NDK AChoreographer (not the JNI one), so libunity imports these
// directly. We return NULL/no-op: with no native frame callbacks the engine clock
// would freeze, but our TimeManager::Update hook + clock thread drive it instead,
// so a null Choreographer is not a boot blocker. Android video/camera imports are
// resolved separately by android_media_stubs.c and fail gracefully as unsupported.
// ---------------------------------------------------------------------------
/* Unity 6 paces frames via the NDK AChoreographer: it posts a frame callback each frame
 * and nativeRender pumps the NdkLooper waiting for the resulting frame message. With a
 * no-op choreographer no callbacks fire, so after a few frames nativeRender blocks
 * forever (cond_wait on the looper queue). Provide a WORKING fake: a non-NULL instance,
 * a small callback registry, and choreographer_tick() (driven by the clock thread at
 * ~vsync rate) that invokes each registered callback with a monotonic frame time. Unity
 * re-registers a callback every frame, so this keeps the frame loop advancing. */
typedef void (*achoreo_cb32)(long, void *);
typedef void (*achoreo_cb64)(int64_t, void *);
static struct { void *cb, *data; int is64; } g_choreo_cbs[32];
static pthread_mutex_t g_choreo_lock = PTHREAD_MUTEX_INITIALIZER;
static void *AChoreographer_getInstance_fake(void) {
  return (void *)0x4143484f; /* 'OCHA' non-NULL */
}
static void achoreo_post(void *cb, void *d, int is64) {
  pthread_mutex_lock(&g_choreo_lock);
  for (int i = 0; i < 32; i++) if (!g_choreo_cbs[i].cb) { g_choreo_cbs[i].cb = cb; g_choreo_cbs[i].data = d; g_choreo_cbs[i].is64 = is64; break; }
  pthread_mutex_unlock(&g_choreo_lock);
}
static void  AChoreographer_postFrameCallback_fake(void *c, void *cb, void *d)   { (void)c; achoreo_post(cb, d, 0); }
static void  AChoreographer_postFrameCallback64_fake(void *c, void *cb, void *d) { (void)c; achoreo_post(cb, d, 1); }
/* invoked by the clock thread (main.c) ~each vsync with the current monotonic time */
void choreographer_tick(int64_t now_ns) {
  void *cbs[32], *datas[32]; int is64s[32], n = 0;
  pthread_mutex_lock(&g_choreo_lock);
  for (int i = 0; i < 32; i++) if (g_choreo_cbs[i].cb) {
    cbs[n] = g_choreo_cbs[i].cb; datas[n] = g_choreo_cbs[i].data; is64s[n] = g_choreo_cbs[i].is64; n++;
    g_choreo_cbs[i].cb = NULL;
  }
  pthread_mutex_unlock(&g_choreo_lock);
  for (int i = 0; i < n; i++) {
    if (is64s[i]) ((achoreo_cb64)cbs[i])(now_ns, datas[i]);
    else          ((achoreo_cb32)cbs[i])((long)now_ns, datas[i]);
  }
}
static void *ANativeWindow_toSurface_fake(void *env, void *win) { (void)env; (void)win; return NULL; }
static int   ALooper_removeFd_fake(void *l, int fd) { (void)l; (void)fd; return 0; }
static long  process_vm_readv_fake(int pid, const void *lv, unsigned long lc,
                                   const void *rv, unsigned long rc, unsigned long f) {
  (void)pid; (void)lv; (void)lc; (void)rv; (void)rc; (void)f; return -1;  /* not supported */
}
static char *__gnu_strerror_r_fake(int e, char *buf, size_t n) { (void)e; if (buf && n) buf[0] = 0; return buf; }
extern char **environ;   /* libil2cpp reads it; newlib provides a valid (empty) char** */
static int msync_fake(void *addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; }

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- liblog / cxxabi / fortify markers ---
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_write", (uintptr_t)&__android_log_write },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "__assert2", (uintptr_t)&__assert2 },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__get_h_errno", (uintptr_t)&__get_h_errno_fake },

  // --- fortify wrappers ---
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__snprintf_chk", (uintptr_t)&__snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&__sprintf_chk_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__pread_chk", (uintptr_t)&__pread_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },

  // --- bionic misc ---
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__system_property_find", (uintptr_t)&__system_property_find_fake },   // Swappy API-level gate -> AChoreographer
  { "__system_property_read", (uintptr_t)&__system_property_read_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "sysconf", (uintptr_t)&sysconf_pass },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "uname", (uintptr_t)&uname_fake },
  { "openlog", (uintptr_t)&ret0_i },
  { "closelog", (uintptr_t)&ret0_i },
  { "syslog", (uintptr_t)&ret0_i },
  { "abort", (uintptr_t)&abort },
  { "_exit", (uintptr_t)&_exit_fake },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "memalign", (uintptr_t)&memalign },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "madvise", (uintptr_t)&madvise_fake },

  // --- mem/str ---
  { "memchr", (uintptr_t)&memchr }, { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy }, { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat }, { "strchr", (uintptr_t)&z_strchr },
  { "strcmp", (uintptr_t)&z_strcmp }, { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&z_strlen }, { "strncasecmp", (uintptr_t)&z_strncasecmp },
  { "strncmp", (uintptr_t)&z_strncmp }, { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&z_strrchr }, { "strstr", (uintptr_t)&z_strstr },
  { "strtod", (uintptr_t)&strtod }, { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol }, { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll }, { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull }, { "atoi", (uintptr_t)&atoi },
  { "qsort", (uintptr_t)&qsort }, { "rand", (uintptr_t)&rand }, { "srand", (uintptr_t)&srand },
  { "isalnum", (uintptr_t)&isalnum }, { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper }, { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },

  // --- wide / multibyte / locale ---
  { "wcslen", (uintptr_t)&wcslen }, { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp }, { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof }, { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold }, { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul }, { "wcstoull", (uintptr_t)&wcstoull },
  { "btowc", (uintptr_t)&btowc }, { "wctob", (uintptr_t)&wctob },
  { "mbrlen", (uintptr_t)&mbrlen }, { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbtowc", (uintptr_t)&mbtowc }, { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "wcrtomb", (uintptr_t)&wcrtomb }, { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "setlocale", (uintptr_t)&setlocale }, { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake }, { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake }, { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake }, { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake }, { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake }, { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake }, { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake }, { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake }, { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake }, { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake }, { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake }, { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  // --- printf family ---
  { "printf", (uintptr_t)&android_printf_sink }, { "puts", (uintptr_t)&puts },
  { "snprintf", (uintptr_t)&snprintf }, { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf }, { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf }, { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf }, { "vsscanf", (uintptr_t)&vsscanf },

  // --- math ---
  { "acosf", (uintptr_t)&acosf }, { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f }, { "cosf", (uintptr_t)&cosf },
  { "sinf", (uintptr_t)&sinf }, { "tanf", (uintptr_t)&tanf },
  { "expf", (uintptr_t)&expf }, { "logf", (uintptr_t)&logf },
  { "powf", (uintptr_t)&powf }, { "pow", (uintptr_t)&pow },
  { "fmodf", (uintptr_t)&fmodf }, { "sincosf", (uintptr_t)&sincosf_fake },
  // --- Unity 6 / AB2 additional libm + libc (exact newlib symbols) ---
  { "exp2", (uintptr_t)&exp2 }, { "tanhf", (uintptr_t)&tanhf },
  { "hypot", (uintptr_t)&hypot }, { "hypotf", (uintptr_t)&hypotf },
  { "remainder", (uintptr_t)&remainder }, { "remainderf", (uintptr_t)&remainderf },
  { "nextafter", (uintptr_t)&nextafter }, { "nextafterf", (uintptr_t)&nextafterf },
  { "strncat", (uintptr_t)&strncat },
  { "fseeko64", (uintptr_t)&fseeko }, { "ftello64", (uintptr_t)&ftello },  // off_t is 64-bit here
  // --- Unity 6 / AB2 NDK Choreographer/window + misc (stubs above) ---
  { "AChoreographer_getInstance", (uintptr_t)&AChoreographer_getInstance_fake },
  { "AChoreographer_postFrameCallback", (uintptr_t)&AChoreographer_postFrameCallback_fake },
  { "AChoreographer_postFrameCallback64", (uintptr_t)&AChoreographer_postFrameCallback64_fake },
  { "ANativeWindow_toSurface", (uintptr_t)&ANativeWindow_toSurface_fake },
  { "ALooper_removeFd", (uintptr_t)&ALooper_removeFd_fake },
  { "process_vm_readv", (uintptr_t)&process_vm_readv_fake },
  { "__gnu_strerror_r", (uintptr_t)&__gnu_strerror_r_fake },
  { "environ", (uintptr_t)&environ }, { "msync", (uintptr_t)&msync_fake },  // libil2cpp's 2 imports

  // --- time ---
  { "clock_gettime", (uintptr_t)&clock_gettime_bionic }, { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime }, { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime }, { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime }, { "time", (uintptr_t)&time },
  { "nanosleep", (uintptr_t)&nanosleep }, { "usleep", (uintptr_t)&usleep },
  { "tzset", (uintptr_t)&tzset }, { "tzname", (uintptr_t)&g_tzname_fake },
  { "getenv", (uintptr_t)&getenv_fake }, { "putenv", (uintptr_t)&putenv },

  // --- stdio (fake __sF aware) ---
  { "__sF", (uintptr_t)&fake_sF },
  { "stdin", (uintptr_t)&fake_sF[0] }, { "stdout", (uintptr_t)&fake_sF[1] }, { "stderr", (uintptr_t)&fake_sF[2] },
  { "fopen", (uintptr_t)&fopen_fake }, { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake }, { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake }, { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell_fake }, { "ftello", (uintptr_t)&ftello },
  { "fflush", (uintptr_t)&fflush_fake }, { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake }, { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake }, { "fgetc", (uintptr_t)&fgetc_fake },
  { "fgets", (uintptr_t)&fgets_fake }, { "getc", (uintptr_t)&getc_fake },
  { "getwc", (uintptr_t)&getc_fake }, { "fputwc", (uintptr_t)&fputc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake }, { "ungetwc", (uintptr_t)&ungetc_fake },
  { "feof", (uintptr_t)&feof_fake }, { "ferror", (uintptr_t)&ferror_fake },
  { "fileno", (uintptr_t)&fileno_fake }, { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },

  // --- filesystem ---
  { "open", (uintptr_t)&open_fake }, { "openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close_fake }, { "read", (uintptr_t)&read_fake },
  { "write", (uintptr_t)&write_fake }, { "pwrite", (uintptr_t)&pwrite_impl },
  { "pread", (uintptr_t)&pread_impl },
  { "lseek", (uintptr_t)&z_lseek }, { "pipe", (uintptr_t)&pipe_fake },
  { "poll", (uintptr_t)&poll_fake }, { "select", (uintptr_t)&select_fake },
  { "dup2", (uintptr_t)&dup2_stub }, { "fcntl", (uintptr_t)&fcntl_stub },
  { "ioctl", (uintptr_t)&ioctl_stub }, { "isatty", (uintptr_t)&isatty },
  { "tcgetattr", (uintptr_t)&tcgetattr_stub }, { "tcsetattr", (uintptr_t)&tcsetattr_stub },
  { "stat", (uintptr_t)&stat_fake }, { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake }, { "statfs", (uintptr_t)&statfs_fake },
  { "statvfs", (uintptr_t)&statvfs_fake }, { "access", (uintptr_t)&access_impl },
  { "mkdir", (uintptr_t)&mkdir_fake }, { "rmdir", (uintptr_t)&rmdir },
  { "unlink", (uintptr_t)&unlink }, { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "chdir", (uintptr_t)&chdir }, { "getcwd", (uintptr_t)&getcwd_fake },
  { "chmod", (uintptr_t)&chmod_stub }, { "fchmod", (uintptr_t)&fchmod_stub },
  { "fchmodat", (uintptr_t)&fchmodat_stub }, { "truncate", (uintptr_t)&truncate_stub },
  { "ftruncate", (uintptr_t)&ftruncate_stub }, { "fsync", (uintptr_t)&fsync_stub },
  { "link", (uintptr_t)&link_stub }, { "symlink", (uintptr_t)&symlink_stub },
  { "readlink", (uintptr_t)&readlink_stub }, { "utime", (uintptr_t)&ret0_i },
  { "utimensat", (uintptr_t)&utimensat_stub }, { "sendfile", (uintptr_t)&sendfile_stub },
  { "opendir", (uintptr_t)&opendir }, { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake }, { "fdopendir", (uintptr_t)&fdopendir_stub },
  { "realpath", (uintptr_t)&realpath_fake },
  { "strerror", (uintptr_t)&strerror }, { "strerror_r", (uintptr_t)&strerror_r_fake },

  // --- signals / setjmp ---
  { "signal", (uintptr_t)&signal_stub }, { "sigaction", (uintptr_t)&sigaction_stub },
  { "sigaddset", (uintptr_t)&ret0_i }, { "sigemptyset", (uintptr_t)&ret0_i },
  { "setjmp", (uintptr_t)&setjmp }, { "longjmp", (uintptr_t)&longjmp },
  { "siglongjmp", (uintptr_t)&longjmp },

  // --- process / ids ---
  { "getpid", (uintptr_t)&getpid_fake }, { "getuid", (uintptr_t)&ret0_u },
  { "geteuid", (uintptr_t)&ret0_u }, { "getegid", (uintptr_t)&ret0_u },
  { "getpwuid", (uintptr_t)&getpwuid_fake }, { "getrusage", (uintptr_t)&getrusage_fake },
  { "fork", (uintptr_t)&fork_fake }, { "execvp", (uintptr_t)&execvp_fake },
  { "waitpid", (uintptr_t)&waitpid_fake }, { "kill", (uintptr_t)&kill_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },

  // --- dynamic loader ---
  { "dlopen", (uintptr_t)&dlopen_fake }, { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake }, { "dlsym", (uintptr_t)&dlsym_fake },

  // --- networking (offline) ---
  { "socket", (uintptr_t)&socket_fake }, { "connect", (uintptr_t)&connect_fake },
  { "bind", (uintptr_t)&bind_fake }, { "listen", (uintptr_t)&listen_fake },
  { "accept", (uintptr_t)&accept_fake }, { "send", (uintptr_t)&send_fake },
  { "recv", (uintptr_t)&recv_fake }, { "sendto", (uintptr_t)&sendto_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake }, { "shutdown", (uintptr_t)&shutdown_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake }, { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "getsockname", (uintptr_t)&getsockname_fake }, { "getpeername", (uintptr_t)&getpeername_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake }, { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake }, { "gethostname", (uintptr_t)&gethostname_fake },
  { "getservbyname", (uintptr_t)&getservbyname_fake },
  { "if_nametoindex", (uintptr_t)&if_nametoindex_fake }, { "if_indextoname", (uintptr_t)&if_indextoname_fake },
  // inet_aton returns 0 on FAILURE (nonzero == success); use ret0_i so callers
  // see a clean failure. inet_pton returns <=0 on error, so retm1_i is correct.
  { "inet_aton", (uintptr_t)&ret0_i }, { "inet_pton", (uintptr_t)&retm1_i },
  { "inet_ntoa", (uintptr_t)&inet_ntoa_stub },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake }, { "pthread_join", (uintptr_t)&pthread_join_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach }, { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_self", (uintptr_t)&pthread_self }, { "pthread_kill", (uintptr_t)&pthread_kill_gc },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_fake }, { "pthread_key_delete", (uintptr_t)&pthread_key_delete_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_fake }, { "pthread_setspecific", (uintptr_t)&pthread_setspecific_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutex_timedlock", (uintptr_t)&pthread_mutex_timedlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0_i },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_fake },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake },
  { "sem_init", (uintptr_t)&sem_init_fake }, { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake }, { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake },

  // --- EGL (mesa; QuerySurface wrapped to fix the 0x0 surface-size report) ---
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay }, { "eglInitialize", (uintptr_t)&eglInitialize },
  { "eglTerminate", (uintptr_t)&eglTerminate }, { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
  { "eglCreateContext", (uintptr_t)&eglCreateContext }, { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
  { "eglSwapBuffers", (uintptr_t)&egl_SwapBuffers_log }, { "eglQuerySurface", (uintptr_t)&egl_QuerySurface_log },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext }, { "eglDestroySurface", (uintptr_t)&eglDestroySurface },

  // --- GLES2 (mesa) ---
  { "glActiveTexture", (uintptr_t)&glActiveTexture }, { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer }, { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer }, { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate }, { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData }, { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor }, { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil }, { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader }, { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram }, { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace }, { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers }, { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers }, { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures }, { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask }, { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray }, { "glDrawArrays", (uintptr_t)&gl_DrawArrays_log },
  { "glDrawElements", (uintptr_t)&gl_DrawElements_log }, { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D }, { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers }, { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures }, { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetError", (uintptr_t)&glGetError }, { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv }, { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram", (uintptr_t)&glLinkProgram }, { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset }, { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage }, { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&gl_ShaderSource_log }, { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilMask", (uintptr_t)&glStencilMask }, { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glTexImage2D", (uintptr_t)&glTexImage2D }, { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D }, { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i }, { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform3fv", (uintptr_t)&glUniform3fv }, { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv }, { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer }, { "glViewport", (uintptr_t)&gl_Viewport_log },

  // --- libandroid: NativeActivity API (android_native.c) ---
  { "ALooper_prepare", (uintptr_t)&ALooper_prepare }, { "ALooper_addFd", (uintptr_t)&ALooper_addFd },
  { "ALooper_pollOnce", (uintptr_t)&ALooper_pollOnce },
  { "AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper },
  { "AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper },
  { "AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent },
  { "AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent },
  { "AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent },
  { "AInputEvent_getType", (uintptr_t)&AInputEvent_getType },
  { "AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction },
  { "AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount },
  { "AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId },
  { "AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX },
  { "AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY },
  { "AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode },
  { "AKeyEvent_getFlags", (uintptr_t)&AKeyEvent_getFlags },
  { "AKeyEvent_getRepeatCount", (uintptr_t)&AKeyEvent_getRepeatCount },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry },
  { "AConfiguration_new", (uintptr_t)&AConfiguration_new },
  { "AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager },
  { "AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage },
  { "AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry },
  { "AConfiguration_delete", (uintptr_t)&AConfiguration_delete },
  { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor },
  { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor },
  { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate },
  { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents },

  // --- AAsset compatibility ---
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
  { "AAsset_close", (uintptr_t)&AAsset_close },

  // --- AndroidBitmap compatibility ---
  { "AndroidBitmap_getInfo", (uintptr_t)&AndroidBitmap_getInfo },
  { "AndroidBitmap_lockPixels", (uintptr_t)&AndroidBitmap_lockPixels },
  { "AndroidBitmap_unlockPixels", (uintptr_t)&AndroidBitmap_unlockPixels },

  // --- OpenSL ES (opensles.c) ---
  // FMOD's OpenSL output dlsym()s a fixed set of SL_IID_* at init and treats ANY
  // NULL as a fatal "Error loading file" (19) -- it aborts before even calling
  // slCreateEngine. (Observed: it asks for ENGINE, ANDROIDSIMPLEBUFFERQUEUE,
  // ANDROIDCONFIGURATION, PLAY, then RECORD -> NULL -> abort.) opensles.c already
  // DEF_IID()-defines all 49 as unique self-pointers, so export every one; the
  // exact request order then can't matter.
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  SL_IID(3DCOMMIT), SL_IID(3DDOPPLER), SL_IID(3DGROUPING), SL_IID(3DLOCATION),
  SL_IID(3DMACROSCOPIC), SL_IID(3DSOURCE), SL_IID(ANDROIDCONFIGURATION),
  SL_IID(ANDROIDEFFECT), SL_IID(ANDROIDEFFECTCAPABILITIES), SL_IID(ANDROIDEFFECTSEND),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(AUDIODECODERCAPABILITIES), SL_IID(AUDIOENCODER),
  SL_IID(AUDIOENCODERCAPABILITIES), SL_IID(AUDIOIODEVICECAPABILITIES), SL_IID(BASSBOOST),
  SL_IID(BUFFERQUEUE), SL_IID(DEVICEVOLUME), SL_IID(DYNAMICINTERFACEMANAGEMENT),
  SL_IID(DYNAMICSOURCE), SL_IID(EFFECTSEND), SL_IID(ENGINE), SL_IID(ENGINECAPABILITIES),
  SL_IID(ENVIRONMENTALREVERB), SL_IID(EQUALIZER), SL_IID(LED), SL_IID(METADATAEXTRACTION),
  SL_IID(METADATATRAVERSAL), SL_IID(MIDIMESSAGE), SL_IID(MIDIMUTESOLO), SL_IID(MIDITEMPO),
  SL_IID(MIDITIME), SL_IID(MUTESOLO), SL_IID(NULL), SL_IID(OBJECT), SL_IID(OUTPUTMIX),
  SL_IID(PITCH), SL_IID(PLAY), SL_IID(PLAYBACKRATE), SL_IID(PREFETCHSTATUS),
  SL_IID(PRESETREVERB), SL_IID(RATEPITCH), SL_IID(RECORD), SL_IID(SEEK), SL_IID(THREADSYNC),
  SL_IID(VIBRA), SL_IID(VIRTUALIZER), SL_IID(VISUALIZATION), SL_IID(VOLUME),
  #undef SL_IID
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

// ---- extra libc imports needed by libFirebaseCpp* --------------------------
// ctype/wctype are macros in newlib, so wrap them to take addresses. inotify
// isn't supported on Switch -> report unavailable (Firebase falls back to
// polling / skips file watching).
#include <wctype.h>
#define FBX_CT(n)  static int    fbx_##n(int c){return n(c);}
#define FBX_WCT(n) static int    fbx_##n(wint_t c){return n(c);}
FBX_CT(isalpha) FBX_CT(islower) FBX_CT(toupper)
FBX_WCT(iswalpha) FBX_WCT(iswblank) FBX_WCT(iswcntrl) FBX_WCT(iswdigit)
FBX_WCT(iswlower) FBX_WCT(iswprint) FBX_WCT(iswpunct) FBX_WCT(iswspace)
FBX_WCT(iswupper) FBX_WCT(iswxdigit) FBX_WCT(towupper)
static int    fbx_strcoll(const char*a,const char*b){return strcoll(a,b);}
static char  *fbx_strpbrk(const char*a,const char*b){return strpbrk(a,b);}
static size_t fbx_strxfrm(char*a,const char*b,size_t n){return strxfrm(a,b,n);}
static int    fbx_wcscoll(const wchar_t*a,const wchar_t*b){return wcscoll(a,b);}
static size_t fbx_wcsxfrm(wchar_t*a,const wchar_t*b,size_t n){return wcsxfrm(a,b,n);}
static int    fbx_inotify_init(void){errno=ENOSYS;return -1;}
static int    fbx_inotify_add_watch(int fd,const char*p,uint32_t m){(void)fd;(void)p;(void)m;errno=ENOSYS;return -1;}
static mode_t fbx_umask_chk(mode_t m){(void)m;return 0;} // no umask on Switch

// Additional Unity/IL2CPP libc imports
// (found by the static UND-vs-imports diff, tools/re). so_util POISONS
// unresolved imports, so any of these being called would trap -> add them.
// Math funcs delegate to newlib libm (-lm is linked); the TTY/exec/netdb ones
// are Switch-appropriate stubs (no terminal / no child processes here).
static double vln_cosh(double x){ return cosh(x); }
static double vln_sinh(double x){ return sinh(x); }
static double vln_tanh(double x){ return tanh(x); }
static float  vln_nearbyintf(float x){ return nearbyintf(x); }
static void   vln_perror(const char*s){  }
static char  *vln_strcasestr(const char*h,const char*n){
  if(!n||!*n) return (char*)h;
  for(; *h; h++){ const char*a=h,*b=n;
    while(*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)){a++;b++;}
    if(!*b) return (char*)h; }
  return NULL;
}
static int    vln_tcflush(int fd,int q){ (void)fd;(void)q; return 0; }      // no TTY on Switch
static int    vln_execl(const char*p,const char*a,...){ (void)p;(void)a; errno=ENOSYS; return -1; }
static const char *vln_gai_strerror(int e){ (void)e; return "getaddrinfo error"; }
DynLibFunction firebase_extra_functions[] = {
  { "isalpha",(uintptr_t)&fbx_isalpha },{ "islower",(uintptr_t)&fbx_islower },
  { "toupper",(uintptr_t)&fbx_toupper },
  { "iswalpha",(uintptr_t)&fbx_iswalpha },{ "iswblank",(uintptr_t)&fbx_iswblank },
  { "iswcntrl",(uintptr_t)&fbx_iswcntrl },{ "iswdigit",(uintptr_t)&fbx_iswdigit },
  { "iswlower",(uintptr_t)&fbx_iswlower },{ "iswprint",(uintptr_t)&fbx_iswprint },
  { "iswpunct",(uintptr_t)&fbx_iswpunct },{ "iswspace",(uintptr_t)&fbx_iswspace },
  { "iswupper",(uintptr_t)&fbx_iswupper },{ "iswxdigit",(uintptr_t)&fbx_iswxdigit },
  { "towupper",(uintptr_t)&fbx_towupper },
  { "strcoll",(uintptr_t)&fbx_strcoll },{ "strpbrk",(uintptr_t)&fbx_strpbrk },
  { "strxfrm",(uintptr_t)&fbx_strxfrm },{ "wcscoll",(uintptr_t)&fbx_wcscoll },
  { "wcsxfrm",(uintptr_t)&fbx_wcsxfrm },
  { "inotify_init",(uintptr_t)&fbx_inotify_init },
  { "inotify_add_watch",(uintptr_t)&fbx_inotify_add_watch },
  { "__umask_chk",(uintptr_t)&fbx_umask_chk },
  // Additional imported symbols
  { "cosh",(uintptr_t)&vln_cosh },{ "sinh",(uintptr_t)&vln_sinh },{ "tanh",(uintptr_t)&vln_tanh },
  { "nearbyintf",(uintptr_t)&vln_nearbyintf },{ "perror",(uintptr_t)&vln_perror },
  { "strcasestr",(uintptr_t)&vln_strcasestr },{ "tcflush",(uintptr_t)&vln_tcflush },
  { "execl",(uintptr_t)&vln_execl },{ "gai_strerror",(uintptr_t)&vln_gai_strerror },
};
size_t firebase_extra_numfunctions = sizeof(firebase_extra_functions)/sizeof(*firebase_extra_functions);

void update_imports(void) { /* no runtime hook swaps needed */ }

/* Combined shim table (base + Unity supplementary), built once and shared by
 * both import resolution and dlsym(). */
static DynLibFunction *g_combined = NULL;
static int g_combined_n = 0;
static void build_combined(void) {
  if (g_combined) return;
  g_combined_n = (int)dynlib_numfunctions + unity_dynlib_numfunctions
               + (int)firebase_extra_numfunctions + (int)android_media_numfunctions;
  g_combined = malloc((size_t)g_combined_n * sizeof(DynLibFunction));
  size_t off = 0;
  memcpy(g_combined + off, dynlib_functions, dynlib_numfunctions * sizeof(DynLibFunction));
  off += dynlib_numfunctions;
  memcpy(g_combined + off, unity_dynlib_functions,
         (size_t)unity_dynlib_numfunctions * sizeof(DynLibFunction));
  off += unity_dynlib_numfunctions;
  memcpy(g_combined + off, firebase_extra_functions,
         firebase_extra_numfunctions * sizeof(DynLibFunction));
  off += firebase_extra_numfunctions;
  memcpy(g_combined + off, android_media_functions,
         android_media_numfunctions * sizeof(DynLibFunction));
}

/* Search the shim table by name -- used by dlsym() for the GLES/EGL/libc
 * entry points the engine resolves dynamically (not module exports). */
uintptr_t dynlib_find_export(const char *name) {
  if (!name) return 0;
  build_combined();
  for (int i = 0; i < g_combined_n; i++)
    if (strcmp(name, g_combined[i].symbol) == 0)
      return g_combined[i].func;
  return 0;
}

void crx_resolve_imports(so_module *mod) {
  so_relocate(mod);
  /* Search the base and Unity tables in one pass before unresolved symbols are
   * replaced with failure stubs. */
  build_combined();
  so_resolve(mod, g_combined, g_combined_n, 1);
}
