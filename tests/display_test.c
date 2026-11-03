#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <EGL/egl.h>

#include "android_native_unity.h"
#include "config.h"
#include "unity_jni.h"

Config config = { .screen_width = -1, .screen_height = -1 };
static AppletOperationMode operation_mode;
static NWindow native = { .valid = true, .default_width = 1280, .default_height = 720 };
static int dimensions_calls, crop_calls, transform_calls, dimensions_error;

AppletOperationMode appletGetOperationMode(void) { return operation_mode; }
bool nwindowIsValid(NWindow *nw) { return nw && nw->valid; }
NWindow *nwindowGetDefault(void) { return &native; }
Result nwindowGetDimensions(NWindow *nw, u32 *w, u32 *h) {
  if (!nwindowIsValid(nw)) return 1;
  *w = nw->width ? nw->width : nw->default_width;
  *h = nw->height ? nw->height : nw->default_height;
  return 0;
}
Result nwindowSetDimensions(NWindow *nw, u32 w, u32 h) {
  dimensions_calls++;
  if (dimensions_error) return dimensions_error;
  if ((nw->width || nw->height) && nw->slots_configured) return 1;
  nw->width = w; nw->height = h;
  memset(nw->crop, 0, sizeof nw->crop);
  return 0;
}
Result nwindowSetCrop(NWindow *nw, int l, int t, int r, int b) {
  crop_calls++;
  nw->crop[0] = l; nw->crop[1] = t; nw->crop[2] = r; nw->crop[3] = b;
  return 0;
}
Result nwindowSetTransform(NWindow *nw, u32 transform) {
  transform_calls++; nw->transform = transform; return 0;
}

/* Unused by display queries, but referenced by the complete JNI int dispatcher. */
const char *jni_string_utf(void *s) { (void)s; return ""; }
void *jni_bytearray_data(void *a, int *n) { (void)a; *n = 0; return NULL; }
void *jni_make_object(const char *s) { (void)s; assert(0); return NULL; }
void *jni_make_string(const char *s) { (void)s; assert(0); return NULL; }
const char *unityfs_redirect_path(const char *s) { assert(0); return s; }

/* Compile the real JNI field and EGL fallback functions, extracted by run_display_tests.py. */
typedef uint64_t juint;
typedef struct { uint32_t tag; char cls[96], name[64], sig[160]; } FakeID;
static int name_has(const char *s, const char *part) { return strstr(s, part) != NULL; }
#define APP_VERSION_CODE AB2_VERSION_CODE
#define NX_SDK_INT 30
#include "field_int.inc"

static EGLBoolean query_result = EGL_TRUE;
static EGLint query_value;
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *value) {
  (void)d; (void)s; (void)attr;
  if (query_result && value) *value = query_value;
  return query_result;
}
#include "egl_query.inc"

static uint64_t call_int(FakeID *id, ...) {
  va_list args; va_start(args, id);
  uint64_t value = unity_dispatch_int(NULL, id, args);
  va_end(args); return value;
}

static void check_size(ANativeWindow *nw, int w, int h) {
  assert(android_native_width() == (u32)w);
  assert(android_native_height() == (u32)h);
  assert(ANativeWindow_getWidth(nw) == w);
  assert(ANativeWindow_getHeight(nw) == h);
  u32 actual_w, actual_h;
  assert(nwindowGetDimensions(&native, &actual_w, &actual_h) == 0);
  assert(actual_w == (u32)w && actual_h == (u32)h);
  assert(native.crop[2] == w && native.crop[3] == h);

  const char *classes[] = { "android/util/DisplayMetrics", "com/unity3d/player/UnityPlayer" };
  for (unsigned i = 0; i < sizeof classes / sizeof *classes; i++) {
    FakeID id = {0}; strcpy(id.cls, classes[i]);
    strcpy(id.name, "widthPixels"); assert(field_int(&id) == (juint)w);
    strcpy(id.name, "heightPixels"); assert(field_int(&id) == (juint)h);
  }
  FakeID id = {0}; strcpy(id.cls, "android/view/Display");
  strcpy(id.name, "getWidth"); assert(call_int(&id) == (uint64_t)w);
  strcpy(id.name, "getRawWidth"); assert(call_int(&id) == (uint64_t)w);
  strcpy(id.name, "getHeight"); assert(call_int(&id) == (uint64_t)h);
  strcpy(id.name, "getRawHeight"); assert(call_int(&id) == (uint64_t)h);

  query_result = EGL_TRUE; query_value = 0;
  EGLint value = -1;
  assert(egl_QuerySurface_log(0, 0, EGL_WIDTH, &value) && value == w);
  assert(egl_QuerySurface_log(0, 0, EGL_HEIGHT, &value) && value == h);
  query_value = 480;
  assert(egl_QuerySurface_log(0, 0, EGL_WIDTH, &value) && value == 480);
  query_result = EGL_FALSE; value = 0;
  assert(!egl_QuerySurface_log(0, 0, EGL_WIDTH, &value) && value == 0);
  query_result = EGL_TRUE; query_value = 0;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  operation_mode = strcmp(argv[1], "handheld") ? AppletOperationMode_Console : AppletOperationMode_Handheld;
  if (!strcmp(argv[1], "custom")) { config.screen_width = 960; config.screen_height = 540; }
  if (!strcmp(argv[1], "rotation")) config.portrait = 2;
  android_native_update_mode();
  ANativeWindow *nw = android_native_window();
  assert(nw);
  int w = operation_mode == AppletOperationMode_Console ? 1920 : 1280;
  int h = operation_mode == AppletOperationMode_Console ? 1080 : 720;
  if (!strcmp(argv[1], "custom")) { w = 960; h = 540; }
  check_size(nw, w, h);
  assert(native.transform == (!strcmp(argv[1], "rotation") ? 7u : 0u));

  /* Saved handheld resolution requested during a docked boot. */
  assert(ANativeWindow_setBuffersGeometry(nw, 1280, 720, 1) == 0);
  check_size(nw, 1280, 720);
  assert(ANativeWindow_setBuffersGeometry(nw, 0, 0, 0) == 0);
  check_size(nw, w, h);

  /* Failed resizes must leave dimensions, crop and transform intact. */
  int old_crops = crop_calls, old_transforms = transform_calls;
  dimensions_error = 1;
  assert(ANativeWindow_setBuffersGeometry(nw, 800, 600, 1) == -EIO);
  assert(crop_calls == old_crops && transform_calls == old_transforms);
  dimensions_error = 0; check_size(nw, w, h);
  assert(ANativeWindow_setBuffersGeometry(nw, 0, 720, 1) == -EINVAL);
  assert(ANativeWindow_setBuffersGeometry(nw, -1, -1, 1) == -EINVAL);
  assert(ANativeWindow_setBuffersGeometry(NULL, 800, 600, 1) == -EINVAL);

  /* Mesa registers buffers once; repeat acquisition and mode changes cannot
   * rewrite their dimensions or pretend an in-place resize succeeded. */
  native.slots_configured = 7;
  int old_sets = dimensions_calls;
  assert(ANativeWindow_fromSurface(NULL, NULL) == nw);
  assert(ANativeWindow_setBuffersGeometry(nw, w, h, 1) == 0);
  assert(dimensions_calls == old_sets);
  assert(ANativeWindow_setBuffersGeometry(nw, 800, 600, 1) == -EBUSY);
  operation_mode = operation_mode == AppletOperationMode_Console
      ? AppletOperationMode_Handheld : AppletOperationMode_Console;
  android_native_update_mode(); check_size(nw, w, h);
  assert(dimensions_calls == old_sets);

  /* After EGL releases the buffers, selecting the other mode is safe. */
  native.slots_configured = 0;
  native.width = native.height = 0;
  android_native_update_mode();
  w = config.screen_width > 0 ? config.screen_width : operation_mode == AppletOperationMode_Console ? 1920 : 1280;
  h = config.screen_height > 0 ? config.screen_height : operation_mode == AppletOperationMode_Console ? 1080 : 720;
  check_size(nw, w, h);
  printf("PASS %s: window, JNI, EGL fallback, saved resolution, failed resize, docking and surface recreation\n", argv[1]);
  return 0;
}
