/* android_native_unity.c -- the NDK symbols libunity.so imports (ANativeWindow ->
 * libnx NWindow, ALooper -> condvar wait/wake, ASensor* -> "no sensors"). Unity is
 * not a NativeActivity, so the engine is driven by JNI-registered natives (main.c)
 * with no ANativeActivity glue. The engine creates its own EGL context from the
 * ANativeWindow, so the host must NOT create an SDL_GL/EGL context (SDL is audio +
 * HID only). Needs devkitA64 + libnx + switch-mesa; not host-compilable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include <GLES3/gl3.h>
#include "util.h"
#include "config.h" /* config.portrait */
#include "android_native_unity.h"

/* ==========================================================================
 * ALooper -- Unity uses it as a per-thread wait/wake primitive (not real fd
 * polling), so a condvar-backed looper is sufficient. If the engine turns out
 * to register real fds, extend the fake-fd poll layer here.
 * ========================================================================== */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
/* Unity 6 fetches ALooper_forThread() for the UI thread and only builds the main-thread
 * NdkLooper on a non-NULL result; a NULL leaves the main NdkLooper global NULL and a later
 * post to it faults (NdkLooper::WaitForCreation on NULL). Create-on-demand so it's valid. */
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 1); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Unity rarely uses these two, but provide them for completeness. */
int ALooper_addFd(ALooper *l,int fd,int ident,int events,void *cb,void *data){
  (void)l;(void)fd;(void)ident;(void)events;(void)cb;(void)data; return 1; }
int ALooper_removeFd(ALooper *l,int fd){ (void)l;(void)fd; return 1; }

/* ==========================================================================
 * Sensors -- report none. Unity imports these
 * must exist and return a clean empty state rather than be missing symbols.)
 * ========================================================================== */
void *ASensorManager_getInstance(void){ static int x; return &x; }
void *ASensorManager_getInstanceForPackage(const char *p){ (void)p; return ASensorManager_getInstance(); }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

/* No orientation sensor: report level. */
void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* ==========================================================================
 * HID -> Unity input via nativeInjectEvent.
 * Handheld: touchscreen passes through. Docked (no touch): left stick drives a
 * virtual cursor, A = tap. Builds a fake MotionEvent (unity_input.c) and calls
 * the recovered nativeInjectEvent(env, thiz, event, deviceId).
 * ========================================================================== */
#include "unity_input.h"

static PadState g_pad;
static HidTouchScreenState g_touch;
static int   g_prev_touch = 0;        /* pointers down last frame */
static float g_cursor_x = 640, g_cursor_y = 360;
static float g_last_tx = 360, g_last_ty = 640;  /* last handheld touch (game space) for UP */
static int   g_prev_a = 0;
static int   g_cursor_shown = 0;      /* draw the docked cursor only while it is in use */

void android_native_input_init(void){
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
}

/* inject signature == recovered nativeInjectEvent: (env,thiz,InputEvent,int)->Z */
typedef uint8_t (*inject_fn)(void*,void*,void*,int);

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  const u32 g_w = android_native_width(), g_h = android_native_height();
  padUpdate(&g_pad);

  /* ---- handheld touchscreen ---- */
  int n = hidGetTouchScreenStates(&g_touch, 1);
  if (n > 0 && g_touch.count > 0){
    g_cursor_shown = 0;                                 /* touching -> hide the stick cursor */
    int   ids[UI_MAX_POINTERS]; float xs[UI_MAX_POINTERS]; float ys[UI_MAX_POINTERS];
    int c = g_touch.count > UI_MAX_POINTERS ? UI_MAX_POINTERS : g_touch.count;
    /* The Switch panel reports touches in its native 1280x720 landscape space; the
     * game render is rotated onto it per config.portrait (see nx_window_set_geom).
     * Undo that same rotation so a tap lands where the player sees it. */
    const float PANEL_W = 1280.0f, PANEL_H = 720.0f;
    for (int i=0;i<c;i++){ ids[i]=(int)g_touch.touches[i].finger_id;
      float px=(float)g_touch.touches[i].x, py=(float)g_touch.touches[i].y;
      if (config.portrait == 0) {          /* AB2 landscape: panel and game are identical */
        xs[i]= px * ((float)g_w / PANEL_W);
        ys[i]= py * ((float)g_h / PANEL_H);
      } else if (config.portrait == 2) {   /* ROT_270 (CCW): game(PANEL_H-py, px) */
        xs[i]= (PANEL_H-py) * ((float)g_w / PANEL_H);
        ys[i]=  px          * ((float)g_h / PANEL_W);
      } else {                             /* ROT_90 (CW, default): game(py, PANEL_W-px) */
        xs[i]=  py          * ((float)g_w / PANEL_H);
        ys[i]= (PANEL_W-px) * ((float)g_h / PANEL_W);
      } }
    g_last_tx = xs[0]; g_last_ty = ys[0];               /* remember for the UP */
    int action = g_prev_touch ? AMOTION_ACTION_MOVE : AMOTION_ACTION_DOWN;
    inject(env, thiz, unity_motionevent(action, c, ids, xs, ys), 0);
    g_prev_touch = c;
    return;
  }
  if (g_prev_touch){                                  /* released -> UP at last pos */
    int   ids[1]={0}; float xs[1]={g_last_tx}, ys[1]={g_last_ty};
    inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    g_prev_touch = 0;
    return;
  }

  /* ---- no touch: left-stick virtual cursor for docked play, A = tap. Drawn as a
     visible dot (android_native_draw_cursor). The stick is mapped through the same
     portrait rotation so it moves the way the player sees it on the TV. ---- */
  HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
  float sx = (ls.x / 32767.0f) * 14.0f, sy = (ls.y / 32767.0f) * 14.0f;   /* ~14 px/frame */
  if (config.portrait == 0) { g_cursor_x += sx; g_cursor_y -= sy; }       /* landscape */
  else if (config.portrait == 2) { g_cursor_x += sy; g_cursor_y += sx; }  /* ROT_270 CCW */
  else                           { g_cursor_x -= sy; g_cursor_y -= sx; }  /* ROT_90  CW  */
  if (g_cursor_x < 0) g_cursor_x = 0;
  if (g_cursor_x > g_w) g_cursor_x = g_w;
  if (g_cursor_y < 0) g_cursor_y = 0;
  if (g_cursor_y > g_h) g_cursor_y = g_h;
  if (sx*sx + sy*sy > 0.25f) g_cursor_shown = 1;       /* stick moved -> reveal cursor */

  int a = (padGetButtons(&g_pad) & HidNpadButton_A) ? 1 : 0;
  if (a) g_cursor_shown = 1;
  int ids[1]={0}; float xs[1]={g_cursor_x}, ys[1]={g_cursor_y};
  if (a && !g_prev_a) {
    inject(env, thiz, unity_motionevent(AMOTION_ACTION_DOWN, 1, ids, xs, ys), 0);
  }
  else if (a && g_prev_a)  inject(env, thiz, unity_motionevent(AMOTION_ACTION_MOVE, 1, ids, xs, ys), 0);
  else if (!a && g_prev_a) {
    inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
  }
  g_prev_a = a;

  /* B -> Android Back key (menu-back), edge-triggered */
  static int prev_b = 0;
  int b = (padGetButtons(&g_pad) & HidNpadButton_B) ? 1 : 0;
  if (b && !prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
  if (!b && prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
  prev_b = b;
}

/* Docked cursor overlay: a soft dot drawn into the game's render surface just before
 * the swap (imports.c calls this), so the compositor rotates it together with the game.
 * No-op unless the stick cursor is active, and every engine GL state we touch is saved
 * and restored so Unity's own rendering is unaffected. */
static GLuint cur_link(const char *vs, const char *fs){
  GLuint v=glCreateShader(GL_VERTEX_SHADER);   glShaderSource(v,1,&vs,0); glCompileShader(v);
  GLuint f=glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f,1,&fs,0); glCompileShader(f);
  GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
  GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
  glDeleteShader(v); glDeleteShader(f);
  if(!ok){ glDeleteProgram(p); return 0; }
  return p;
}
void android_native_draw_cursor(void){
  if (!g_cursor_shown) return;
  const u32 g_w = android_native_width(), g_h = android_native_height();
  static struct { GLuint prog; GLint pos, loc, feather; int tried; } c = {0,0,0,0,0};
  if (!c.tried){
    c.tried = 1;
    c.prog = cur_link(
      "attribute vec2 aPos; attribute vec2 aLocal; varying vec2 vL;"
      "void main(){ vL=aLocal; gl_Position=vec4(aPos,0.0,1.0); }",
      "precision mediump float; varying vec2 vL; uniform float uF;"
      "void main(){ float d=length(vL);"
      " float a=1.0-smoothstep(1.0-uF,1.0,d);"
      " float core=1.0-smoothstep(0.72-uF,0.72+uF,d);"
      " gl_FragColor=vec4(mix(vec3(0.05),vec3(0.98),core), a*0.85); }");
    if (c.prog){ c.pos=glGetAttribLocation(c.prog,"aPos"); c.loc=glGetAttribLocation(c.prog,"aLocal"); c.feather=glGetUniformLocation(c.prog,"uF"); }
  }
  if (!c.prog) return;

  float cx = (g_cursor_x / (float)g_w) * 2.0f - 1.0f;
  float cy = 1.0f - (g_cursor_y / (float)g_h) * 2.0f;
  float r  = 18.0f * ((float)(g_w > g_h ? g_w : g_h) / 1280.0f);
  float rx = r/(float)g_w*2.0f, ry = r/(float)g_h*2.0f;
  const GLfloat pos[8]  = { cx-rx,cy-ry, cx+rx,cy-ry, cx-rx,cy+ry, cx+rx,cy+ry };
  static const GLfloat local[8] = { -1,-1, 1,-1, -1,1, 1,1 };

  GLint pprog,pbuf,pvao,pvp[4],bsr,bdr,bsa,bda,ber,bea;
  GLboolean e_bl=glIsEnabled(GL_BLEND), e_dp=glIsEnabled(GL_DEPTH_TEST),
            e_sc=glIsEnabled(GL_SCISSOR_TEST), e_cu=glIsEnabled(GL_CULL_FACE);
  glGetIntegerv(GL_CURRENT_PROGRAM,&pprog); glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&pbuf);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&pvao); glGetIntegerv(GL_VIEWPORT,pvp);
  glGetIntegerv(GL_BLEND_SRC_RGB,&bsr); glGetIntegerv(GL_BLEND_DST_RGB,&bdr);
  glGetIntegerv(GL_BLEND_SRC_ALPHA,&bsa); glGetIntegerv(GL_BLEND_DST_ALPHA,&bda);
  glGetIntegerv(GL_BLEND_EQUATION_RGB,&ber); glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&bea);

  glBindVertexArray(0);                                /* scratch VAO: client arrays allowed */
  glViewport(0,0,(GLsizei)g_w,(GLsizei)g_h);
  glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
  glBindBuffer(GL_ARRAY_BUFFER,0); glUseProgram(c.prog); glUniform1f(c.feather, 2.5f/r);
  glEnableVertexAttribArray(c.pos); glEnableVertexAttribArray(c.loc);
  glVertexAttribPointer(c.pos,2,GL_FLOAT,GL_FALSE,0,pos);
  glVertexAttribPointer(c.loc,2,GL_FLOAT,GL_FALSE,0,local);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  glDisableVertexAttribArray(c.pos); glDisableVertexAttribArray(c.loc);

  glBindBuffer(GL_ARRAY_BUFFER,(GLuint)pbuf); glBindVertexArray((GLuint)pvao);
  glUseProgram((GLuint)pprog); glViewport(pvp[0],pvp[1],pvp[2],pvp[3]);
  glBlendEquationSeparate((GLenum)ber,(GLenum)bea);
  glBlendFuncSeparate((GLenum)bsr,(GLenum)bdr,(GLenum)bsa,(GLenum)bda);
  if(!e_bl) glDisable(GL_BLEND);
  if(e_dp)  glEnable(GL_DEPTH_TEST);
  if(e_sc)  glEnable(GL_SCISSOR_TEST);
  if(e_cu)  glEnable(GL_CULL_FACE);
}
