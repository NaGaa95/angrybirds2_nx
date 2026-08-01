/* Minimal local replacement for the unavailable Firebase Android libraries.
 * Object-producing bindings receive a stable non-null handle; scalar, status,
 * and void bindings return zero. This lets optional Firebase initialization
 * complete without providing remote configuration, analytics, or messaging. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Shared, zero-filled backing store handed out for every "object handle" the
 * SWIG layer expects. Big enough that any field reads SWIG/managed performs on a
 * proxy land on zeroed memory rather than faulting. One buffer for all types is
 * fine: our delete/dispose stubs are no-ops, so nothing is freed or aliased in a
 * way that matters. */
static uint8_t g_fb_obj[1024] __attribute__((aligned(16)));

static long  fb_stub_zero(void)   { return 0; }
static void *fb_stub_handle(void) { return g_fb_obj; }

/* The Firebase default app name constant (firebase::kDefaultAppName). The SWIG
 * string-returning name getters must hand back this exact, non-null C string:
 * the managed FirebaseApp uses it as the key into nameToProxy, and a null there
 * throws inside GetInstance() during the dependency check (faulting the task and
 * hanging the FirebaseLoading gate).
 *
 * CRITICAL: it must be a HEAP allocation, not a static. SWIG's C# string-return
 * marshaling frees the returned char* (real Firebase returns a freshly allocated
 * copy from its SWIG string helper). free() on a static/rodata pointer walks a
 * bogus malloc chunk header and faults (observed: Data Abort at 0x0 inside
 * CreateInternal's string marshal). Hand out a fresh malloc'd copy each call so
 * the marshaler's free() is valid; the getter is called rarely, so the worst
 * case (if a given call site does not free) is a few leaked bytes. */
static void *fb_stub_default_name(void) {
  static const char name[] = "__FIRAPP_DEFAULT";
  char *p = (char *)malloc(sizeof(name));
  if (p) memcpy(p, name, sizeof(name));
  return p;
}

/* Does this dlsym name belong to the Firebase SWIG surface we're replacing? */
static int fb_is_firebase_symbol(const char *s) {
  if (!s) return 0;
  if (strstr(s, "_CSharp_"))                 return 1;  /* Firebase_<Mod>_CSharp_* */
  if (!strncmp(s, "Firebase_", 9))           return 1;
  if (!strncmp(s, "SWIGRegister", 12))       return 1;  /* exception/string cb reg */
  if (!strncmp(s, "SWIG", 4) && strstr(s, "Firebase")) return 1;
  return 0;
}

/* A symbol whose managed return is an object/handle/pointer (must be non-null so
 * the proxy's cPtr guard passes). SWIG factory/accessor naming conventions:
 *   new_X, X_CreateInternal, ...Create..., ...GetInstance..., DefaultInstance,
 *   GetReference..., ...Future... (future handles), ..._SWIGUpcast (base ptr). */
static int fb_returns_handle(const char *s) {
  /* Future completion pollers must read as 0: kFutureStatusComplete == 0 and
   * "no error" == 0. Returning a non-null pointer here would make any Task that
   * polls a Future (RemoteConfig/Messaging fetches) spin forever. Catch these
   * before the "Future" handle rule below. */
  if (strstr(s, "GetStatus"))      return 0;
  if (strstr(s, "GetError"))       return 0;
  /* The SWIG Future surface uses lowercase property getters: FutureBase_status()
   * must read kFutureStatusComplete(0) and FutureBase_error() "no error"(0), or
   * the Future->Task bridge polls forever (e.g. RemoteConfig SetDefaultsAsync).
   * These contain "Future" so must be caught before the "Future"->handle rule. */
  if (strstr(s, "FutureBase_status")) return 0;
  if (strstr(s, "FutureBase_error"))  return 0;   /* incl. error_message: null is fine when error==0 */
  if (strstr(s, "new_"))            return 1;
  if (strstr(s, "Create"))         return 1;   /* CreateInternal / Create__SWIG_* */
  if (strstr(s, "GetInstance"))    return 1;
  if (strstr(s, "DefaultInstance"))return 1;
  if (strstr(s, "Instance"))       return 1;   /* *_Instance, GetInstanceInternal */
  if (strstr(s, "App_get"))        return 1;   /* RemoteConfig/Messaging .App -> the app object */
  if (strstr(s, "GetReference"))   return 1;
  if (strstr(s, "Future"))         return 1;   /* future handle objects */
  if (strstr(s, "SWIGUpcast"))     return 1;   /* base-class pointer cast */
  if (strstr(s, "GetTask"))        return 1;
  return 0;
}

/* Resolve a Firebase SWIG symbol to a stub, or NULL if it's not ours.
 * Called from dlsym_fake() after the real-module / shim-table lookups miss. */
void *firebase_stub_lookup(const char *symbol) {
  if (!fb_is_firebase_symbol(symbol)) return NULL;
  /* App-identity name getters must return the non-null default-app name string,
   * not 0/null -- see fb_stub_default_name above. Covers DefaultName_get,
   * NameInternal_get and the FirebaseApp Name getter. */
  if (strstr(symbol, "DefaultName") ||
      strstr(symbol, "NameInternal") ||
      strstr(symbol, "_Name_get")    ||
      strstr(symbol, "get_Name")) {
    return (void *)&fb_stub_default_name;
  }
  if (fb_returns_handle(symbol)) {
    return (void *)&fb_stub_handle;
  }
  return (void *)&fb_stub_zero;
}
