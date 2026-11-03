/* Unity's Android display and NWindow must describe the same buffer geometry. */
#include <errno.h>
#include <switch.h>

#include "android_native_unity.h"
#include "config.h"

static Mutex display_lock;
static NWindow *window;
static u32 width = 1280, height = 720;
static u32 default_width = 1280, default_height = 720;

/* Caller holds display_lock. Mesa owns registered buffers: changing NWindow
 * dimensions underneath an EGL surface cannot resize those allocations. */
static int set_geometry(NWindow *nw, u32 w, u32 h) {
  if (!nwindowIsValid(nw)) return -EINVAL;

  mutexLock(&nw->mutex);
  u32 old_w = 0, old_h = 0;
  Result rc = nwindowGetDimensions(nw, &old_w, &old_h);
  if (R_SUCCEEDED(rc) && (old_w != w || old_h != h)) {
    if (nw->slots_configured) {
      mutexUnlock(&nw->mutex);
      return -EBUSY;
    }
    rc = nwindowSetDimensions(nw, w, h);
  }
  if (R_SUCCEEDED(rc)) rc = nwindowSetCrop(nw, 0, 0, w, h);
  if (R_SUCCEEDED(rc)) {
    u32 transform = config.portrait == 2 ? NATIVE_WINDOW_TRANSFORM_ROT_270
                    : config.portrait == 1 ? NATIVE_WINDOW_TRANSFORM_ROT_90 : 0;
    rc = nwindowSetTransform(nw, transform);
  }
  mutexUnlock(&nw->mutex);

  if (R_FAILED(rc)) return -EIO;
  width = w;
  height = h;
  return 0;
}

void android_native_update_mode(void) {
  mutexLock(&display_lock);
  if (window) {
    mutexLock(&window->mutex);
    bool active = window->slots_configured != 0;
    mutexUnlock(&window->mutex);
    if (active) {
      /* Docking changes the output, not an existing EGL surface's buffers.
       * Keep its dimensions until Unity releases/recreates that surface. */
      mutexUnlock(&display_lock);
      return;
    }
  }

  bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  u32 w = docked ? 1920 : 1280;
  u32 h = docked ? 1080 : 720;
  if (config.screen_width > 0 && config.screen_height > 0) {
    w = (u32)config.screen_width;
    h = (u32)config.screen_height;
  }
  default_width = w;
  default_height = h;
  if (window) set_geometry(window, w, h);
  else { width = w; height = h; }
  mutexUnlock(&display_lock);
}

u32 android_native_width(void) {
  mutexLock(&display_lock);
  u32 value = width;
  mutexUnlock(&display_lock);
  return value;
}

u32 android_native_height(void) {
  mutexLock(&display_lock);
  u32 value = height;
  mutexUnlock(&display_lock);
  return value;
}

ANativeWindow *android_native_window(void) {
  mutexLock(&display_lock);
  if (!window) window = nwindowGetDefault();
  int rc = set_geometry(window, width, height);
  ANativeWindow *result = rc == 0 ? (ANativeWindow *)window : NULL;
  mutexUnlock(&display_lock);
  return result;
}

void ANativeWindow_acquire(ANativeWindow *nw) { (void)nw; }
void ANativeWindow_release(ANativeWindow *nw) { (void)nw; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface) {
  (void)env; (void)surface;
  return android_native_window();
}
int32_t ANativeWindow_getWidth(ANativeWindow *nw) {
  return nwindowIsValid((NWindow *)nw) ? (int32_t)android_native_width() : -EINVAL;
}
int32_t ANativeWindow_getHeight(ANativeWindow *nw) {
  return nwindowIsValid((NWindow *)nw) ? (int32_t)android_native_height() : -EINVAL;
}
int32_t ANativeWindow_getFormat(ANativeWindow *nw) {
  return nwindowIsValid((NWindow *)nw) ? 1 /* RGBA_8888 */ : -EINVAL;
}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *nw, int32_t w, int32_t h,
                                        int32_t format) {
  (void)format; /* The EGL configuration selects the buffer format. */
  if (w < 0 || h < 0 || (w == 0) != (h == 0)) return -EINVAL;
  mutexLock(&display_lock);
  int rc = set_geometry((NWindow *)nw, w ? (u32)w : default_width,
                                     h ? (u32)h : default_height);
  mutexUnlock(&display_lock);
  return rc;
}
