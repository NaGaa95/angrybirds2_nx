#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int Result;
typedef struct { bool held; } Mutex;
static inline void mutexLock(Mutex *m) { assert(!m->held); m->held = true; }
static inline void mutexUnlock(Mutex *m) { assert(m->held); m->held = false; }
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)
#define NATIVE_WINDOW_TRANSFORM_ROT_90 4
#define NATIVE_WINDOW_TRANSFORM_ROT_270 7
typedef enum { AppletOperationMode_Handheld, AppletOperationMode_Console } AppletOperationMode;
typedef struct {
  bool valid;
  Mutex mutex;
  u64 slots_configured;
  u32 width, height, default_width, default_height, transform;
  int crop[4];
} NWindow;

AppletOperationMode appletGetOperationMode(void);
bool nwindowIsValid(NWindow *nw);
NWindow *nwindowGetDefault(void);
Result nwindowGetDimensions(NWindow *nw, u32 *w, u32 *h);
Result nwindowSetDimensions(NWindow *nw, u32 w, u32 h);
Result nwindowSetCrop(NWindow *nw, int left, int top, int right, int bottom);
Result nwindowSetTransform(NWindow *nw, u32 transform);
