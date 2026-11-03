/* jni_fake.c -- Android/JNI compatibility environment for Unity.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "jni_unimpl.h"
#include "libc_shim.h"   /* managed_path: device-less paths for managed code */
#include "opensles.h"    /* audio_fmod_open/write: FMOD native-audio output sink */
#include "unity_jni.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// local reference registry (matches the engine's Push/PopLocalFrame brackets)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    mutexUnlock(&locals_lock);
  }
  return ref;
}

// interned-string pool: the engine re-creates the same constant strings (class
// names, the activity name) constantly; pool them by content so repeats don't
// fill the local-ref table. Pooled strings are never reg_local'd, and free_ref
// skips them (range check below).
#define MAX_ISTR 512
static FakeString istr_pool[MAX_ISTR];
static int istr_count = 0;

static void free_ref(void *ref) {
  if (!ref)
    return;
  if ((char *)ref >= (char *)istr_pool && (char *)ref < (char *)&istr_pool[MAX_ISTR])
    return;  // interned string -- pooled, never freed
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    /* UHandles intentionally live for the process lifetime. Android framework
     * calls such as SharedPreferences.Editor.putInt() return the same jobject
     * receiver. Unity can then delete one JNI wrapper while retaining another
     * wrapper for that identical pointer; freeing it here creates a dangling
     * editor before the following apply()/commit(). Properly freeing these
     * handles requires JNI reference counts, not a raw DeleteGlobalRef free. */
    default: break; // TAG_ID / TAG_CLASS / UJ_TAG are pooled or externally owned
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// object constructors
// ---------------------------------------------------------------------------

// Intern objects by label -- one pooled object per class (TAG_CLASS so free_ref()
// leaves it alone, never reg_local'd) -- so the engine's frequent NewObject calls
// don't fill the local-ref table. Safe: our objects are opaque, stateless handles
// dispatched by method class, not by identity.
#define MAX_IOBJ 512
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) r = &iobj_pool[0];
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  mutexLock(&locals_lock);
  for (int i = 0; i < istr_count; i++)            // repeats reuse the pooled string
    if (!strcmp(istr_pool[i].utf, u)) { void *r = &istr_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (istr_count < MAX_ISTR) {
    FakeString *s = &istr_pool[istr_count++];
    s->tag = TAG_STRING;
    s->utf = strdup(u);
    mutexUnlock(&locals_lock);
    return s;                                      // pooled, not reg_local'd
  }
  mutexUnlock(&locals_lock);
  FakeString *s = calloc(1, sizeof(*s));           // pool full: one-off local string
  s->tag = TAG_STRING;
  s->utf = strdup(u);
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// UTF-16 code-unit count of a modified-UTF-8 string (Java String.length()).
// ASCII -> byte count; astral planes count as a surrogate pair. Used by both
// GetStringLength and the String.length() upcall handler.
static juint utf16_len(const char *str) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    const unsigned char c = *p;
    juint adv; uint32_t cp;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) {
      if (!p[k]) { adv = k; break; }
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    n += (cp >= 0x10000) ? 2u : 1u;
    p += adv;
  }
  return n;
}

// ---------------------------------------------------------------------------
// interned classes + singletons
// ---------------------------------------------------------------------------

#define MAX_CLASSES 1024
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) {
    
    return &class_pool[0];
  }
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && c->tag == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_activity_obj = NULL;   // the MyNativeActivity instance
static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// The engine fetches the ClassLoader every frame; hand back a cached singleton
// (pooled, never reg_local'd) so it doesn't fill the local-ref table.
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

// ---------------------------------------------------------------------------
// method/field ID pool (class-aware)
// ---------------------------------------------------------------------------

#define MAX_IDS 8192
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  
  return id;
}

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

static const char *first_string_arg(const char *sig, va_list va);

static const char *lang_code(void) {
  if (config.language == LANG_JA) return "ja";
  if (config.language == LANG_EN) return "en";
  // LANG_AUTO: resolve the Switch system language once
  static const char *cached = NULL;
  if (!cached) {
    cached = "en";
    u64 code; SetLanguage sl;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl))) {
        switch (sl) {
          case SetLanguage_JA:                            cached = "ja"; break;
          case SetLanguage_FR: case SetLanguage_FRCA:     cached = "fr"; break;
          case SetLanguage_DE:                            cached = "de"; break;
          case SetLanguage_IT:                            cached = "it"; break;
          case SetLanguage_ES: case SetLanguage_ES419:    cached = "es"; break;
          case SetLanguage_PT: case SetLanguage_PTBR:     cached = "pt"; break;
          case SetLanguage_NL:                            cached = "nl"; break;
          case SetLanguage_RU:                            cached = "ru"; break;
          case SetLanguage_KO:                            cached = "ko"; break;
          case SetLanguage_ZHCN: case SetLanguage_ZHHANS:
          case SetLanguage_ZHTW: case SetLanguage_ZHHANT: cached = "zh"; break;
          default:                                        cached = "en"; break;
        }
      }
      setExit();
    }
  }
  return cached;
}

/* Locale getters consistent with lang_code() (libunity builds its culture string
 * as getLanguage()+"-"+getCountry()). */
typedef struct { const char *lang, *ctry, *iso3l, *iso3c, *loc; } LangRow;
static const LangRow *lang_row(void) {
  static const LangRow rows[] = {
    {"en","US","eng","USA","en_US"}, {"ja","JP","jpn","JPN","ja_JP"},
    {"fr","FR","fra","FRA","fr_FR"}, {"de","DE","deu","DEU","de_DE"},
    {"it","IT","ita","ITA","it_IT"}, {"es","ES","spa","ESP","es_ES"},
    {"pt","PT","por","PRT","pt_PT"}, {"nl","NL","nld","NLD","nl_NL"},
    {"ru","RU","rus","RUS","ru_RU"}, {"ko","KR","kor","KOR","ko_KR"},
    {"zh","CN","zho","CHN","zh_CN"},
  };
  const char *l = lang_code();
  for (unsigned i = 0; i < sizeof rows / sizeof *rows; i++)
    if (!strcmp(rows[i].lang, l)) return &rows[i];
  return &rows[0];
}

const char *jni_locale_name(void) {
  return lang_row()->loc;
}

/* AndroidJavaObject wraps a JNI jobject by asking GetObjectClass(). Our opaque
 * pooled objects normally report java/lang/Object, which is fine for generic
 * placeholders but loses java.util.Locale's identity. Rovio's Locale helper then
 * resolves instance methods (getCountry/toLanguageTag) on Object. Recognize only
 * our interned Locale handle here so other intentionally-generic objects keep the
 * old behaviour. */
static int is_locale_object(const void *obj) {
  const uintptr_t p = (uintptr_t)obj;
  const uintptr_t first = (uintptr_t)&iobj_pool[0];
  const uintptr_t last = (uintptr_t)&iobj_pool[MAX_IOBJ];
  if (!obj || p < first || p >= last)
    return 0;
  const FakeObject *o = obj;
  return o->tag == TAG_CLASS && !strcmp(o->label, "java/util/Locale");
}

static void *locale_method_result(const char *method) {
  const LangRow *lr = lang_row();
  const char *value = NULL;
  if (!strcmp(method, "getLanguage"))          value = lr->lang;
  else if (!strcmp(method, "getCountry"))      value = lr->ctry;
  else if (!strcmp(method, "getISO3Language")) value = lr->iso3l;
  else if (!strcmp(method, "getISO3Country"))  value = lr->iso3c;
  else if (!strcmp(method, "toString") || name_has(method, "getDisplayName"))
    value = lr->loc;
  if (!strcmp(method, "toLanguageTag")) {
    static char tag[16];
    snprintf(tag, sizeof tag, "%s-%s", lr->lang, lr->ctry);
    value = tag;
  }
  if (value) {
    return jni_make_string(value);
  }
  return NULL;
}

// Return the first non-empty String argument in a JNI argument list.
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

/* jni_string_utf is defined far below (shared with unity_jni.c) and unity_jni.h
 * is included after this point, so forward-declare it for act_object's
 * getProperty()/locale arg reads. */
const char *jni_string_utf(void *jstr);

/* AudioManager.getProperty(PROPERTY_OUTPUT_*): FMOD reads the native sample rate /
 * frames-per-buffer to size its audio path; empty -> framesPerBuffer 0 -> FMOD error
 * 60. Hand back Switch-sane values. The key argument is lost on this JNI call path,
 * so field_object records the last PROPERTY_OUTPUT_* field read in g_last_output_prop
 * and we fall back to it (1 = sample rate, 2 = frames-per-buffer). */
static int g_last_output_prop = 0;

static void *getproperty_value(const char *key) {
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  if (which == 1) return jni_make_string("48000");   /* native output sample rate */
  if (which == 2) return jni_make_string("256");     /* native frames-per-buffer */
  return jni_make_string("");
}

static void *act_object(const FakeID *id, va_list va) {
  /* Report the Android Choreographer as unavailable (null) so the engine takes its
   * non-vsync fallback instead of blocking on a vsync callback we can't deliver.
   * Covers Swappy's SwappyDisplayManager too. Must precede the generic handlers. */
  if ((name_has(id->cls, "Choreographer") || name_has(id->cls, "SwappyDisplayManager")) &&
      (name_has(id->name, "getInstance") || sig_returns(id->sig, "Landroid/view/Choreographer;")))
    return NULL;
  /* Uri.encode/decode: Unity round-trips PlayerPrefs keys through these. We are
   * the storage, so identity (return the input string) round-trips correctly and
   * keeps keys non-empty. Must precede the generic handlers. */
  if ((name_has(id->cls, "net/Uri") || sig_returns(id->sig,"Ljava/lang/String;")) &&
      (!strcmp(id->name, "encode") || !strcmp(id->name, "decode"))) {
    void *arg = va_arg(va, void *);
    /* Unity 6's collapsed Object static wrapper supplies its cached jclass as
     * the first variadic item. It cannot be a String; the next item is the key. */
    if (arg && *(uint32_t *)arg == TAG_CLASS) arg = va_arg(va, void *);
    return arg;
  }
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  // version / package / device / storage strings
  if (name_has(id->name, "VersionName")) return jni_make_string(AB2_VERSION_NAME);
  if (name_has(id->name, "PackageName")) return jni_make_string(AB2_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  // Environment.getExternalStorageState() must return the SAME token as the
  // Environment.MEDIA_MOUNTED field ("mounted", see field_object) or the engine
  // decides external storage is unavailable and the save path never initialises.
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  // Locale.getDefault() is declared through AndroidJavaObject's generic bridge as
  // returning Object in this Unity build, not Locale. Match the class+method rather
  // than relying only on the JNI return signature, or it falls through to NULL and
  // Rovio.Abba.Frameworks.Localization.Locale.GetCurrentLocale NREs.
  if (name_has(id->cls, "Locale")) {
    if (!strcmp(id->name, "getDefault")) {
      return jni_make_object("java/util/Locale");
    }
    void *result = locale_method_result(id->name);
    if (result) return result;
  }
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path"))
    return jni_make_string(managed_path(GAME_HOME));
  // Rovio Beacon SDK: DeviceInfoImpl_Android..ctor fetches a JSON device blob via
  // AndroidJavaClass("com.rovio.beacon.DeviceInfo").CallStatic<string>() and feeds it to
  // SimpleJSON JSONNode.Parse(). Our generic String fallback returns "" -> Parse("") returns
  // NULL -> the ctor's null-check throws NRE -> TypeInitializationException on Beacon.DeviceInfo
  // -> BeaconSDK.Initialize() aborts -> Session..ctor throws -> error popup. Return a valid empty
  // JSON object so Parse yields a non-null node. (DataPath is already handled by the Path matcher
  // above; Locale must be a real locale so the deviceToken is sane.)
  if (name_has(id->cls, "beacon/DeviceInfo")) {
    if (name_has(id->name, "Locale") || name_has(id->name, "locale"))
      return jni_make_string(lang_row()->loc);
    return jni_make_string("{}");
  }
  // Android object getters that must NOT be null (getPackageInfo/getApplicationInfo
  // reach PackageInfo.versionName/versionCode -> Application.version); a null here
  // blanked the version. Hand back live opaque objects for field_object to service.
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  // Locale.getDefault() must be non-null or the engine's culture resolves to
  // SystemLanguage.Unknown and the game is forced to English.
  if (sig_returns(id->sig, "Ljava/util/Locale;"))
    return jni_make_object("java/util/Locale");
  /* AndroidJavaObject.Call<byte[]>() must receive a real JNI primitive array.
   * Returning NULL here becomes a managed null byte[]; the treasure/reward flow
   * then faults in File.WriteAllBytes and never releases its interaction layer.
   * An unsupported Android serializer has no payload on Switch, so use a valid
   * empty array: File.WriteAllBytes completes and the owning async task follows
   * its normal modal-cleanup path. */
  if (sig_returns(id->sig, "[B")) {
    return make_pri_array_adopt(malloc(1), 0, 1);
  }
  if (sig_returns(id->sig, "Ljava/lang/String;"))
    return jni_make_string(""); // UUID, asset-pack name, etc.
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  // Integer.parseInt / Long.parseLong: FMOD parses getProperty()'s results through
  // these; the old 0 fall-through made framesPerBuffer 0 -> FMOD error 60.
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    
    return v;
  }
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  // isGooglePlayServicesAvailable(): return SERVICE_MISSING (1) so the Play Games
  // plugin cleanly disables itself instead of signing in against absent GMS.
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  // Rovio AndroidPermissionsPlugin.checkPermission -> bool, TRUE == GRANTED (RefreshPermission
  // Queue does `tbnz w0,#0` to SKIP enqueue). Returning 0 (the old default) marks every
  // permission DENIED -> RequestPermission fires an async grant callback the port can never
  // deliver -> the AskForAllPermissions state machine never completes -> boot PARKS on the
  // loading screen. Report GRANTED so OnAllPermissionsGranted fires immediately.
  if (name_has(id->name, "checkPermission")     || name_has(id->name, "hasPermission") ||
      name_has(id->name, "isPermissionGranted") || name_has(id->name, "checkNotificationStatus"))
    return 1;
  if (name_has(id->name, "shouldShowRequestPermissionRationale")) return 0;
  (void)va;
  // every other "is something open / clicked / ok" probe -> false/0
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  return 0.0f;
}

static void act_void(const FakeID *id, va_list va) {
  (void)va;
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
  // openStore / sendBroadcast / IME open / Mobage / web view: no-op
}

// ---------------------------------------------------------------------------
// top-level dispatch by class + return kind
// ---------------------------------------------------------------------------

/* Delegate Unity and input classes to their stateful handlers. */
#include "unity_input.h"

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  /* MotionEvent.obtain(MotionEvent): copy factory -- return a real UEvent copy so
   * getSource/getX/getY on the copy hit our handlers (else the event is dropped). */
  /* Unity's cached MotionEvent class is java/lang/Object on this port (the same
   * collapsed cache seen by IsInstanceOf), so class-based routing loses this
   * static factory and returns NULL.  Match the exact Android signature instead:
   * it is unambiguous, and preserving the source UEvent is what makes the later
   * getSource/getAction/getX/getY calls reach our receiver-based dispatch. */
  if (!strcmp(id->name, "obtain") &&
      strstr(id->sig, "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;")) {
    void *src = va_arg(va, void *);
    return unity_motionevent_obtain(src);
  }
  /* String.getBytes([charset]) -> the string's UTF-8 bytes (Unity's PlayerPrefs key
   * encoding needs real bytes or every encoded key collides). Route by receiver. */
  if (recv && *(uint32_t *)recv == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  /* Stateful/pooled receivers are routed below to their specialised module.
   * None of those modules implements a byte[] result, and returning its generic
   * NULL used to bypass act_object's non-null fallback. Enforce the JNI contract
   * before receiver ownership can swallow an unsupported Android serializer. */
  if (sig_returns(id->sig, "[B")) {
    return make_pri_array_adopt(malloc(1), 0, 1);
  }
  /* The wrapped Locale instance's method IDs may be resolved against
   * java/lang/Object (see is_locale_object). Route by receiver identity so
   * getLanguage/getCountry/toLanguageTag still return real Switch locale data. */
  if (is_locale_object(recv)) {
    void *result = locale_method_result(id->name);
    if (result) return result;
  }
  if (unity_owns_recv(recv) || !strcmp(id->name,"getSharedPreferences") ||
      unity_owns_class(id->cls))
    return unity_dispatch_object(recv, id, va);
  return act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  // String instance methods via CallIntMethod (length() sizes path buffers). The
  // receiver is our FakeString reported as java/lang/Object, so route on the receiver
  // tag + method name, not id->cls (else length() returns 0 and buffers overflow).
  if (recv && *(uint32_t *)recv == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) return 0;
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
  }
  /* Boxed PlayerPrefs value (Integer/Long/Boolean) from getAll(): unbox by the
   * receiver so only our own boxes are affected. intValue/longValue/booleanValue
   * all land here (CallInt/Long/BooleanMethod -> dispatch_int). */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  /* MotionEvent/KeyEvent getters: the engine resolves these via
   * GetObjectClass(event) -> java/lang/Object, so id->cls is NOT the real
   * class. Route on the receiver tag (mirrors the FakeString case above), or
   * touch getters silently fall through to act_int and return 0. */
  if (input_owns_recv(recv)) return input_dispatch_int(recv, id, va);
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) return unity_dispatch_int(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_int(recv, id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) return unity_dispatch_float(recv,id,va);
  if (input_owns_recv(recv)) return input_dispatch_float(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) { unity_dispatch_void(recv, id, va); return; }
  act_void(id, va);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  /* Unity 6 asks GetObjectClass(InputEvent) first and resolves either the KeyEvent
   * or MotionEvent getter set from that result.  Reporting java/lang/Object made a
   * real touch resolve getKeyCode(), so nativeInjectEvent rejected it before it
   * ever reached Unity's touch queue.  Preserve the exact kind carried by UEvent;
   * copies returned by MotionEvent.obtain retain the same tag/kind. */
  if (input_owns_recv(obj))
    return intern_class(input_recv_is_motion(obj)
                        ? "android/view/MotionEvent"
                        : "android/view/KeyEvent");
  /* Keep the single typed Android object required by managed boot code typed.
   * AndroidJavaObject(IntPtr) caches this class and uses it to resolve the next
   * instance method. Reporting Object here makes Locale.getDefault() succeed,
   * but its following toLanguageTag()/getCountry() call resolve on Object and
   * return null to Locale.GetCurrentLocale(). */
  if (is_locale_object(obj)) return intern_class("java/util/Locale");
  const char *uclass=unity_recv_class(obj);
  if (uclass) return intern_class(uclass);
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

/* String(byte[][,charset]) constructor: decode the byte array (UTF-8) into a real
 * FakeString so Unity's PlayerPrefs keys aren't all empty and colliding. Other
 * ctors are unaffected (still a labelled object). */
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  FakeID *m = mid;
  /* Unity 6 sometimes resolves this constructor against java/lang/Object even
   * though the exact signature is Java String(byte[], charset). Recognise the
   * unambiguous constructor signature as well as the nominal class; otherwise
   * the result becomes a generic pooled object and its later getBytes() call can
   * produce managed null (also collapsing every encoded PlayerPrefs key). */
  const int byte_string_ctor = m && !strcmp(m->name, "<init>") &&
                               !strcmp(m->sig, "([BLjava/lang/String;)V");
  if ((cn && strstr(cn, "java/lang/String")) || byte_string_ctor) {
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  return jni_make_object(cn);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  va_list va; va_start(va, mid); void *a0 = va_arg(va, void *); va_end(va);
  return new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; void *a0 = va_arg(va, void *);
  return new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* IsInstanceOf (slot 32). We can't track the runtime type of opaque fake jobjects,
 * so answer optimistically (assume the cast succeeds) -- returning 0 trapped the game
 * in a per-frame retry loop. Exact answers for input events and boxed values below. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  /* nativeInjectEvent classifies the event by instanceof KeyEvent/MotionEvent, so
   * answer by the handle's real kind (a blind 1 gets a touch read as a key). */
  if (input_owns_recv(obj)) {
    if (strstr(cn, "MotionEvent")) return input_recv_is_motion(obj) ? 1 : 0;
    if (strstr(cn, "KeyEvent"))    return input_recv_is_motion(obj) ? 0 : 1;
    if (strstr(cn, "InputEvent"))  return 1;
    /* On this Unity 6 build the nativeInjectEvent caches sometimes contain an
     * untyped java/lang/Object handle.  Optimistically returning true made its
     * first (KeyEvent) test win for every touch.  Use the verified two-test
     * order only for an untyped handle; exact class names above remain exact. */
    const juint answer=(juint)input_instanceof_untyped(obj);
    return answer;
  }
  /* Boxed PlayerPrefs values from getAll(): Unity reads each value with
   * IsInstanceOf(value, Integer/Long/Float/Boolean/String) then unboxes. These
   * MUST be exact or every value is misread as the first type checked. */
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (obj && *(uint32_t *)obj == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
    /* other classes: fall through to the optimistic answer below */
  }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share class-aware dispatch) --------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- Call<type>MethodA / NewObjectA (jvalue[] args) -------------------------
// SWIG bindings and AndroidJavaObject.CallStatic<T>()/Call<T>() marshal their
// arguments into a jvalue[] array and invoke the "A" variants. A va_list cannot
// be reconstructed from jvalue[] portably, so we forward to the variadic form
// with no varargs: the dispatch keys off the resolved method name and the
// object/value getters ignore positional args (defaulting to a non-null handle
// of the right class), which is what these init paths need. Previously these
// slots fell through to the unimplemented stub and returned 0/null, hanging the
// first scene (e.g. UNIMPL slot 116 == CallStaticObjectMethodA).
static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  // getProperty()'s String key lives in jvalue[0] and does NOT survive the
  // va_list-less forward below, so pull it directly. FMOD's OpenSL output reads
  // PROPERTY_OUTPUT_FRAMES_PER_BUFFER through this "A" path; without the key it
  // got "" -> framesPerBuffer 0 -> FMOD error 60.
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  if (a && (name_has(id->cls, "net/Uri") || sig_returns(id->sig,"Ljava/lang/String;")) &&
      (!strcmp(id->name, "encode") || !strcmp(id->name, "decode"))) {
    void *arg = (void *)((void *const *)a)[0];
    if (arg && *(uint32_t *)arg == TAG_CLASS) arg = (void *)((void *const *)a)[1];
    return arg;                                  /* identity, jvalue path */
  }
  void *out=NULL;
  if (unity_dispatch_object_a(r,id,a,&out)) return out;
  return j_CallObjectMethod(e, r, id);
}
static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallBooleanMethod(e,r,id); }
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  // parseInt/parseLong via the jvalue[] path: read the String from jvalue[0].
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    
    return v;
  }
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallIntMethod(e, r, id);
}
static juint j_CallLongMethodA   (void *e, void *r, FakeID *id, const void *a){
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallLongMethod(e,r,id); }
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){
  float out=0; if(unity_dispatch_float_a(r,id,a,&out))return out;
  return j_CallFloatMethod(e,r,id); }
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){
  if(unity_dispatch_void_a(r,id,a))return;
  j_CallVoidMethod(e,r,id); }
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  return new_object_dispatch(cls, mid, a ? ((void *const *)a)[0] : NULL); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) { // naive UTF-16 -> UTF-8 (BMP)
    const uint32_t c = u[i];
    if (c < 0x80) tmp[o++] = (char)c;
    else if (c < 0x800) { tmp[o++] = 0xC0 | (c >> 6); tmp[o++] = 0x80 | (c & 0x3F); }
    else { tmp[o++] = 0xE0 | (c >> 12); tmp[o++] = 0x80 | ((c >> 6) & 0x3F); tmp[o++] = 0x80 | (c & 0x3F); }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// GetStringUTFRegion: the engine reads ALL its strings through this (not
// GetStringUTFChars), so it must work. Copies the [start, start+len) region as
// modified UTF-8 into buf. Our strings are ASCII (paths / archive names), where
// UTF-16 char offsets == UTF-8 byte offsets, so a byte copy is exact.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
// GetStringRegion: UTF-16 variant; widen ASCII bytes into jchar (uint16) buf.
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  for (int i = 0; i < len; i++) buf[i] = (uint8_t)s[start + i];
}
// GetStringLength must return the UTF-16 code-unit count, not the byte count
// (CJK text is multi-byte in UTF-8); engine code sizes UTF-16 buffers with it.
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  return (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) a->items[i] = val;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- fields -----------------------------------------------------------------
// The engine and game read Java fields (android.os.Build.*, Build.VERSION.SDK_INT,
// PackageInfo.versionName/versionCode, DisplayMetrics.*). Route every read through a
// name-based dispatcher on the FakeID; a universal null/0 blanked the app version and
// zeroed display metrics.
#define APP_VERSION_NAME AB2_VERSION_NAME  /* "26.4.3" */
#define APP_VERSION_CODE AB2_VERSION_CODE  /* 50297008 */
#define NX_SDK_INT       33        /* Android 13 -- high enough to pass any minSdk gate  */

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  // PackageInfo / ApplicationInfo version string
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  // UnityPlayer.currentActivity is THE Activity -- null NPEs every currentActivity
  // .getXxx() in managed code, so hand back a live opaque Activity.
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    // AB2's InitializeJavaPlugin block 1 reads the sibling static `activityContext` (NOT
    // currentActivity); a null there logs the CRITICAL "Failed to get activityContext from
    // UnityPlayer" and passes a null activity into the Java plugin. Hand back a live activity.
    if (!strcmp(n, "activityContext"))  return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
    // AB2's AndroidPermissionsPlugin.InitializeJavaPlugin reads UnityPlayer's static
    // `androidPermissionsPlugin`; a null there logs "Failed to get androidPermissionsPlugin
    // from UnityPlayer". Hand back a live opaque object so init succeeds; the plugin's
    // checkPermission(...) call then routes to act_int -> 1 == GRANTED (see act_int).
    if (!strcmp(n, "androidPermissionsPlugin")) {
      void *plugin = jni_make_object("com/rovio/permissions/AndroidPermissionsPlugin");
      
      return plugin;
    }
  }
  // AudioManager.PROPERTY_OUTPUT_* static keys, read just before getProperty(key).
  // Record which one in g_last_output_prop so getProperty() can answer when the key
  // argument is lost on the JNI call path (see getproperty_value).
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  // Context.*_SERVICE name constants -> the strings getSystemService() expects
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  // Environment.MEDIA_MOUNTED MUST equal getExternalStorageState()'s return
  // ("mounted", set in act_object) or the storage check fails and save data is
  // disabled. Keep both in lockstep.
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  // android.os.Build identity strings (all public static final String)
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  // Any other String-typed field -> "" (non-null avoids NPEs in string ops).
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  // Any other object field stays null; array fields handled by the caller.
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  // UnityPlayer integer statics
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return 320;
    if (!strcmp(n, "widthPixels")) return android_native_width();
    if (!strcmp(n, "heightPixels"))return android_native_height();
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  // DisplayMetrics integer fields (width/height/dpi)
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return android_native_width();
    if (!strcmp(n, "heightPixels")) return android_native_height();
    if (!strcmp(n, "densityDpi"))   return 320;    /* xhdpi bucket                */
  }
  return 0;
}

/* DisplayMetrics.density / xdpi / ydpi / scaledDensity are float fields. 0 would
 * make dp->px scaling collapse, so hand back a sane xhdpi density (2.0). */
static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }

// --- reflection bridge (proxy support) --------------------------------------
// Unity's AndroidJavaProxy converts reflected Method/Field objects into
// jmethod/jfieldIDs via these. We carry no real reflection, but a non-null opaque
// ID lets the proxy bind and store; an invoked callback routes through act_* and
// no-ops, which is right for our stubbed events.
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env; (void)m; return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env; (void)f; return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

// --- misc -------------------------------------------------------------------

/* Capture FMODAudioDevice's native bridge (fmodGetInfo/fmodProcess) here --
 * RegisterNatives is the only place these file-local addresses are exposed -- so a
 * native playback thread can pull PCM from FMOD (the Java run() loop never runs). */
typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod_;
void *g_fmod_getinfo = 0, *g_fmod_process = 0, *g_fmod_micdata = 0;
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  const char *cn = class_name_of(cls);
  const JNINativeMethod_ *m = methods;
  int is_fmod = name_has(cn, "fmod") || name_has(cn, "FMOD");
  
  if (is_fmod && m) {
    for (int i = 0; i < n; i++) {
      
      if (!m[i].name) continue;
      if      (!strcmp(m[i].name, "fmodGetInfo"))        g_fmod_getinfo = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcess"))        g_fmod_process = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcessMicData")) g_fmod_micdata = m[i].fn;
    }
    
  }
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

// ---------------------------------------------------------------------------
// FMOD native-audio pump
// ---------------------------------------------------------------------------
// fmodProcess(env, this, ByteBuffer) renders one FMOD mixer block into
// env->GetDirectBufferAddress(ByteBuffer). With no JVM the Java run() loop never
// calls it, so this native thread does and pushes the PCM to the SDL sink. The only
// JNIEnv entry it uses is GetDirectBufferAddress (slot 230), which hands back our
// staging buffer; the captured fn-ptrs get non-NULL `this`/buffer tokens.

#define FMOD_STAGING_BYTES (64 * 1024)   // generous: must exceed one mixer block
static unsigned char g_fmod_staging[FMOD_STAGING_BYTES];
static int  g_fmod_bb_token   = 0;       // stand-in jobject for the ByteBuffer
static int  g_fmod_this_token = 0;       // stand-in jobject for `this`
static int  g_fmod_started    = 0;
static int  g_fmod_stop       = 0;
static pthread_t g_fmod_thread;

typedef int (*fmod_getinfo_fn)(void *env, void *thiz, int which);
typedef int (*fmod_process_fn)(void *env, void *thiz, void *bytebuffer);

enum {
  FMOD_INFO_OUTPUT_RATE = 0,
  FMOD_INFO_DSP_BLOCK_LENGTH,
  FMOD_INFO_DSP_BUFFER_COUNT,
  FMOD_INFO_OUTPUT_TYPE,
  FMOD_INFO_CHANNELS,
  FMOD_INFO_COUNT
};

// slot 230: every ByteBuffer we ever pass is our own staging buffer.
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; (void)buf; return g_fmod_staging;
}
// slot 231 (defensive -- the disasm shows fmodProcess never calls it).
static long j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; (void)buf; return (long)FMOD_STAGING_BYTES;
}

static int fmod_output_info_valid(const int info[FMOD_INFO_COUNT]) {
  const int rate = info[FMOD_INFO_OUTPUT_RATE];
  const int block_frames = info[FMOD_INFO_DSP_BLOCK_LENGTH];
  const int buffer_count = info[FMOD_INFO_DSP_BUFFER_COUNT];
  const int channels = info[FMOD_INFO_CHANNELS];

  if (rate < 8000 || rate > 192000 || block_frames <= 0 || buffer_count <= 0)
    return 0;
  if (channels != 1 && channels != 2 && channels != 6)
    return 0;

  const size_t frame_bytes = (size_t)channels * sizeof(int16_t);
  return (size_t)block_frames <= FMOD_STAGING_BYTES / frame_bytes;
}

static int fmod_wait_output_info(fmod_getinfo_fn getinfo,
                                 int info[FMOD_INFO_COUNT]) {
  if (!getinfo)
    return 0;

  int previous[FMOD_INFO_COUNT];
  int have_previous = 0;
  while (!g_fmod_stop && !jni_quit_requested) {
    int current[FMOD_INFO_COUNT];
    for (int i = 0; i < FMOD_INFO_COUNT; i++)
      current[i] = getinfo(fake_env, &g_fmod_this_token, i);

    if (fmod_output_info_valid(current)) {
      if (have_previous &&
          current[FMOD_INFO_OUTPUT_RATE] == previous[FMOD_INFO_OUTPUT_RATE] &&
          current[FMOD_INFO_DSP_BLOCK_LENGTH] == previous[FMOD_INFO_DSP_BLOCK_LENGTH] &&
          current[FMOD_INFO_DSP_BUFFER_COUNT] == previous[FMOD_INFO_DSP_BUFFER_COUNT] &&
          current[FMOD_INFO_CHANNELS] == previous[FMOD_INFO_CHANNELS]) {
        memcpy(info, current, sizeof(current));
        return 1;
      }
      memcpy(previous, current, sizeof(previous));
      have_previous = 1;
    } else {
      have_previous = 0;
    }
    svcSleepThread(10000000ULL); // 10 ms
  }
  return 0;
}

static void *fmod_audio_thread(void *arg) {
  (void)arg;
  /* libunity reads bionic TLS (including its stack protector) on every engine
   * thread.  Sharing the main-thread block causes false canary failures. */
  static uint8_t fmod_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(fmod_tls);
  fmod_getinfo_fn getinfo = (fmod_getinfo_fn)g_fmod_getinfo;
  fmod_process_fn process = (fmod_process_fn)g_fmod_process;
  if (!process)
    return NULL;

  // start() fires before the render loop has driven a System::update(), so the FMOD
  // mixer's DSP buffers aren't allocated yet and fmodProcess would fault. Wait a batch
  // of frames (each drives a System::update) before the first call.
  extern uint32_t port_frame_count(void);
  const uint32_t warmup_frames = 120;
  uint32_t f0 = port_frame_count();
  while (!g_fmod_stop && !jni_quit_requested &&
         port_frame_count() - f0 < warmup_frames)
    svcSleepThread(10000000ULL); // 10 ms
  if (g_fmod_stop || jni_quit_requested)
    return NULL;

  int info[FMOD_INFO_COUNT];
  if (!fmod_wait_output_info(getinfo, info))
    return NULL;

  const int rate = info[FMOD_INFO_OUTPUT_RATE];
  const int channels = info[FMOD_INFO_CHANNELS];
  const int block = info[FMOD_INFO_DSP_BLOCK_LENGTH] *
                    channels * (int)sizeof(int16_t);

  if (!audio_fmod_open(rate, channels))
    return NULL;

  // pace to realtime via the device queue; target ~4 blocks buffered.
  const uint32_t hi = (uint32_t)block * 6;
  const uint32_t lo = (uint32_t)block * 3;
  while (!g_fmod_stop && !jni_quit_requested) {
    while (!g_fmod_stop && !jni_quit_requested && audio_fmod_queued() > hi)
      svcSleepThread(2000000ULL); // 2 ms
    if (g_fmod_stop || jni_quit_requested)
      break;

    // refill toward the low watermark
    do {
      if (process(fake_env, &g_fmod_this_token, &g_fmod_bb_token) != 0)
        break;
      uint32_t q = 0;
      if (!audio_fmod_write(g_fmod_staging, block, &q))
        return NULL;
      if (q > hi) break;
    } while (!g_fmod_stop && !jni_quit_requested && audio_fmod_queued() < lo);
    svcSleepThread(2000000ULL); // 2 ms
  }
  return NULL;
}

void fmod_audio_bind(void *getinfo, void *process, void *micdata) {
  g_fmod_getinfo = getinfo;
  g_fmod_process = process;
  g_fmod_micdata = micdata;
  
}

// Called directly by main.c; dispatch_void remains as a fallback for builds that
// actually instantiate FMODAudioDevice.start().
void fmod_audio_start(void) {
  if (g_fmod_started) return;
  if (!g_fmod_process) {  return; }
  g_fmod_stop = 0;
  g_fmod_started = 1;
  if (pthread_create(&g_fmod_thread, NULL, fmod_audio_thread, NULL) != 0) {
    
    g_fmod_started = 0;
    return;
  }
  
}

void fmod_audio_shutdown(void) {
  if (!g_fmod_started) return;
  g_fmod_stop = 1;
  pthread_join(g_fmod_thread, NULL);
  g_fmod_started = 0;
  
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
/* Accessors used by unity_jni.c and unity_input.c to read
 * (otherwise static) FakeString / FakePriArray without duplicating the structs. */
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  jni_fill_unimpl(env_table); // indexed stubs: log the exact unimplemented slot

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // "A" (jvalue[]) variants -- instance
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // "A" (jvalue[]) variants -- static (SWIG / AndroidJavaObject.CallStatic<T>)
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[230] = (void *)j_GetDirectBufferAddress;  // fmodProcess drains via this
  env_table[231] = (void *)j_GetDirectBufferCapacity; // defensive (unused by fmodProcess)

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
