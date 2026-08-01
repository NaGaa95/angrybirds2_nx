/* Android framework handlers used by Unity's JNI bridge. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "unity_jni.h"
#include "util.h"
#include "config.h"   /* AB2_PACKAGE etc. */

/* FakeID layout we read (must match jni_fake.c). Only the char[] fields. */
struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

#define SCREEN_W_HANDHELD 720   /* fbstub45 PORTRAIT (stable) */
#define SCREEN_H_HANDHELD 1280
#define SCREEN_W_DOCKED   1080
#define SCREEN_H_DOCKED   1920
#define REFRESH_HZ        60

static char g_root[192];            /* e.g. /switch/zookeeper                 */
static char g_assets[208];          /* g_root "/assets"                      */

static int  has(const char *s, const char *sub){ return strstr(s,sub)!=NULL; }

/* --------------------------------------------------------------------------
 * stateful handle objects (streams / fds / prefs) -- jni_fake's objects are
 * label-only, so we carry our own. Opaque to the engine; recovered via recv.
 * -------------------------------------------------------------------------- */
enum { UJ_TAG = 0x554a4831 /*'UJH1'*/ };
enum { UJ_INPUTSTREAM, UJ_AFD, UJ_FD, UJ_PREFS, UJ_EDITOR, UJ_GENERIC,
       UJ_MAP, UJ_SET, UJ_ITER, UJ_ENTRY, UJ_BOXED };

typedef struct {
  uint32_t tag; int kind;
  FILE *fp;                 /* InputStream                                  */
  int   fd;                 /* AFD / FD                                     */
  long  off, len;           /* AFD region                                  */
  int   idx;                /* UJ_ITER cursor / UJ_ENTRY kv index           */
  char  btype;              /* UJ_BOXED: 'I' int 'L' long 'B' bool 'F' float*/
  long long bival;          /* UJ_BOXED int/long/bool payload               */
  double bfval;             /* UJ_BOXED float payload                       */
} UHandle;

static UHandle *uh_new(int kind){
  UHandle *h = calloc(1,sizeof *h);
  h->tag = UJ_TAG; h->kind = kind; h->fd = -1; return h;
}
/* jni_fake.c free_ref() should call this for UJ_TAG; until then streams that
 * the engine forgets to close() leak. Optional hardening, see header note. */
void unity_handle_free(void *p){
  UHandle *h = p; if (!h || h->tag!=UJ_TAG) return;
  if (h->fp) fclose(h->fp);
  if (h->fd>=0) close(h->fd);
  free(h);
}
static int is_uh(void *p,int kind){ UHandle*h=p; return h && h->tag==UJ_TAG && h->kind==kind; }

int unity_owns_recv(void *p){
  UHandle *h=p; return h && h->tag==UJ_TAG;
}

const char *unity_recv_class(void *p){
  UHandle *h=p;
  if (!h || h->tag!=UJ_TAG) return NULL;
  switch (h->kind){
    case UJ_INPUTSTREAM: return "java/io/InputStream";
    case UJ_AFD:         return "android/content/res/AssetFileDescriptor";
    case UJ_FD:          return "java/io/FileDescriptor";
    case UJ_PREFS:       return "android/content/SharedPreferences";
    case UJ_EDITOR:      return "android/content/SharedPreferences$Editor";
    case UJ_MAP:         return "java/util/Map";
    case UJ_SET:         return "java/util/Set";
    case UJ_ITER:        return "java/util/Iterator";
    case UJ_ENTRY:       return "java/util/Map$Entry";
    case UJ_BOXED:
      if (h->btype=='I') return "java/lang/Integer";
      if (h->btype=='L') return "java/lang/Long";
      if (h->btype=='F') return "java/lang/Float";
      if (h->btype=='B') return "java/lang/Boolean";
      return "java/lang/Object";
    default:             return "java/lang/Object";
  }
}

/* --------------------------------------------------------------------------
 * asset path: AssetManager.open("bin/Data/x") -> g_root/assets/bin/Data/x
 * (the engine's *direct* fopen of the data path is handled separately by the
 *  libc_shim fopen redirect + the Context path getters below.)
 * -------------------------------------------------------------------------- */
static void asset_path(char *out,size_t n,const char *name){
  while (name && (name[0]=='/' )) name++;
  snprintf(out,n,"%s/%s",g_assets,name?name:"");
}

/* Path handed to *managed* code (persistentDataPath / dataPath via the Context
 * getters below). It must be Unix-rooted ("/switch/zookeeper"): Mono/IL2CPP on
 * Android uses Unix path rules where ":" is NOT a root marker, so a "sdmc:/..."
 * path is treated as *relative* and Path.Combine() concatenates it after a
 * relative asset path -> "assets/bin/Data/sdmc:/switch/zookeeper". newlib then
 * sees the embedded "sdmc:" mid-path, mis-parses the device, and null-derefs
 * (Data Abort at the devoptab mkdir_r slot, +0x68). Stripping the device prefix
 * fixes this; newlib still resolves the device-less absolute path via the
 * default device (sdmc), set by our boot chdir, so file I/O is unaffected. Our
 * internal g_root/g_assets keep the explicit "sdmc:" prefix. */
static const char *managed_root(void){
  const char *c = strchr(g_root, ':');
  return (c && c[1] == '/') ? c + 1 : g_root;
}

/* ==========================================================================
 * SharedPreferences  (Unity PlayerPrefs == save data)  -> g_root/prefs.kv
 * flat "type\tkey\tvalue" lines; value url-ish escaped on tab/newline.
 * ========================================================================== */
typedef struct { char type; char *key; char *val; } KV;
static KV   *g_kv = NULL; static int g_kv_n=0, g_kv_cap=0; static int g_kv_dirty=0;

static char *prefs_file(char *buf,size_t n){ snprintf(buf,n,"%s/prefs.kv",g_root); return buf; }

static void kv_set(char type,const char*key,const char*val){
  for (int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)){
    g_kv[i].type=type; free(g_kv[i].val); g_kv[i].val=strdup(val); g_kv_dirty=1; return; }
  if (g_kv_n==g_kv_cap){ g_kv_cap=g_kv_cap?g_kv_cap*2:32; g_kv=realloc(g_kv,g_kv_cap*sizeof(KV)); }
  g_kv[g_kv_n].type=type; g_kv[g_kv_n].key=strdup(key); g_kv[g_kv_n].val=strdup(val);
  g_kv_n++; g_kv_dirty=1;
}
static KV *kv_get(const char*key){ for(int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)) return &g_kv[i]; return NULL; }
static void kv_remove(const char*key){
  for(int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)){
    free(g_kv[i].key);free(g_kv[i].val); g_kv[i]=g_kv[--g_kv_n]; g_kv_dirty=1; return; }
}
static void kv_clear(void){ for(int i=0;i<g_kv_n;i++){free(g_kv[i].key);free(g_kv[i].val);} g_kv_n=0; g_kv_dirty=1; }

static void esc(FILE*f,const char*s){ for(;*s;s++){ if(*s=='\\'||*s=='\t'||*s=='\n'){fputc('\\',f);
  fputc(*s=='\t'?'t':*s=='\n'?'n':'\\',f);} else fputc(*s,f);} }
static char *unesc(char*s){ char*o=s,*w=s; for(;*o;o++){ if(*o=='\\'&&o[1]){o++;
  *w++=(*o=='t')?'\t':(*o=='n')?'\n':*o;} else *w++=*o;} *w=0; return s; }

static void prefs_load(void){
  char p[256], bak[272], tmp[272];
  prefs_file(p,sizeof p);
  snprintf(bak,sizeof bak,"%s.bak",p);
  snprintf(tmp,sizeof tmp,"%s.tmp",p);
  FILE*f=fopen(p,"rb");
  /* A power loss can land between the two renames in prefs_flush(). Prefer the
   * previous known-good image, then the fully-written temporary image. */
  if(!f) f=fopen(bak,"rb");
  if(!f) f=fopen(tmp,"rb");
  if(!f){  return; }
  char line[2048];
  while (fgets(line,sizeof line,f)){
    char *nl=strchr(line,'\n'); if(nl)*nl=0;
    if(!line[0]) continue;
    char type=line[0]; char *k=line+2;            /* "T\tkey\tval"          */
    char *t1=strchr(k,'\t'); if(!t1) continue; *t1=0; char*v=t1+1;
    kv_set(type, unesc(k), unesc(v));
  }
  fclose(f); g_kv_dirty=0;
}
static void prefs_flush(void){
  if(!g_kv_dirty){  return; }
  char p[256], bak[272], tmp[272];
  prefs_file(p,sizeof p);
  snprintf(bak,sizeof bak,"%s.bak",p);
  snprintf(tmp,sizeof tmp,"%s.tmp",p);
  remove(tmp);
  FILE*f=fopen(tmp,"wb");
  if(!f){  return; }
  for(int i=0;i<g_kv_n;i++){ fputc(g_kv[i].type,f); fputc('\t',f);
    esc(f,g_kv[i].key); fputc('\t',f); esc(f,g_kv[i].val); fputc('\n',f); }
  int write_ok = !ferror(f) && fflush(f)==0;
  if (write_ok && fsync(fileno(f))!=0 && errno!=ENOSYS && errno!=EINVAL)
    write_ok=0;
  if (fclose(f)!=0) write_ok=0;
  if(!write_ok){
    
    return;
  }

  /* libnx's FAT rename does not promise POSIX replacement semantics. Move the
   * old file aside first, install the new one, and restore on failure. */
  remove(bak);
  int had_old = (rename(p,bak)==0);
  if(!had_old && errno!=ENOENT){
    
    return;
  }
  if(rename(tmp,p)!=0){
    if(had_old) rename(bak,p);
    return;
  }
  if(had_old) remove(bak);
  g_kv_dirty=0;
  
}

void unity_jni_flush(void){ prefs_flush(); }

/* ==========================================================================
 * getAll() boxed values: turn a stored KV into the Java object Unity expects.
 * Strings come back as a native FakeString (Unity reads them via
 * GetStringUTFChars); primitives come back as our UJ_BOXED handle, which
 * jni_fake.c recognises by receiver for IsInstanceOf + intValue/longValue/
 * booleanValue/floatValue. Type char matches kv_set(): S/I/L/B/F.
 * ========================================================================== */
static void *uh_box_from_kv(const KV *kv){
  if (!kv) return jni_make_string("");
  switch (kv->type){
    case 'I': case 'L': case 'B': {
      UHandle *h = uh_new(UJ_BOXED);
      h->btype = kv->type;
      h->bival = strtoll(kv->val, NULL, 10);
      if (kv->type=='B') h->bival = (kv->val[0]=='1'||kv->val[0]=='t'||kv->val[0]=='T') ? 1 : 0;
      return h;
    }
    case 'F': {
      UHandle *h = uh_new(UJ_BOXED);
      h->btype = 'F'; h->bfval = strtod(kv->val, NULL);
      return h;
    }
    default: /* 'S' and anything else -> string */
      return jni_make_string(kv->val);
  }
}

/* Receiver-keyed accessors used by jni_fake.c (see unity_jni.h). */
int unity_is_boxed(void *p){
  UHandle *h = p; return (h && h->tag==UJ_TAG && h->kind==UJ_BOXED) ? 1 : 0;
}
uint64_t unity_boxed_int(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0;
  if (h->btype=='F') return (uint64_t)(long long)h->bfval;
  return (uint64_t)h->bival;
}
float unity_boxed_float(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0.0f;
  return (h->btype=='F') ? (float)h->bfval : (float)h->bival;
}
/* 1/0 if obj is one of our boxed primitives and matches/!matches clazz; -1 if
 * obj is not ours (jni_fake.c then applies its own rules). */
int unity_isinstance(void *p, const char *clazz){
  UHandle *h = p; if (!h || h->tag!=UJ_TAG || h->kind!=UJ_BOXED) return -1;
  if (!clazz) return 0;
  switch (h->btype){
    case 'I': return strstr(clazz,"Integer") ? 1 : 0;
    case 'L': return strstr(clazz,"Long")    ? 1 : 0;
    case 'F': return strstr(clazz,"Float")   ? 1 : 0;
    case 'B': return strstr(clazz,"Boolean") ? 1 : 0;
  }
  return 0;
}

/* ==========================================================================
 * class ownership + dispatch
 * ========================================================================== */
int unity_owns_class(const char *cls){
  return has(cls,"AssetManager") || has(cls,"java/io/InputStream") ||
         has(cls,"AssetFileDescriptor") || has(cls,"java/io/FileDescriptor") ||
         has(cls,"SharedPreferences") || has(cls,"SharedPreferences$Editor") ||
         has(cls,"java/util/Map") || has(cls,"java/util/Set") ||
         has(cls,"java/util/Iterator") || has(cls,"java/util/HashMap") ||
         has(cls,"view/Display") || has(cls,"DisplayManager") ||
         has(cls,"res/Configuration") || has(cls,"res/Resources") ||
         has(cls,"DisplayMetrics") || has(cls,"content/Context") ||
         has(cls,"unity3d/player/UnityPlayer");
}

/* ---- object-returning calls --------------------------------------------- */
void *unity_dispatch_object(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *rcls=unity_recv_class(recv);
  const char *cls=rcls?rcls:id->cls, *m=id->name;

  /* Unity commonly resolves this inherited Context method from an Activity or
   * even java/lang/Object. Match the exact method name so the real persistent
   * store is returned instead of an opaque Context placeholder. */
  if (!strcmp(m,"getSharedPreferences")){
    
    return uh_new(UJ_PREFS);
  }

  /* AssetManager.open(name) -> InputStream ; openFd(name) -> AssetFileDescriptor */
  if (has(cls,"AssetManager")){
    if (has(m,"openFd") || has(m,"openNonAssetFd")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[320]; asset_path(path,sizeof path,name);
      int fd = open(path,O_RDONLY);
      
      if(fd<0){ return NULL; /* CHECK: engine expects exception; NULL usually ok */ }
      struct stat st; fstat(fd,&st);
      UHandle*h=uh_new(UJ_AFD); h->fd=fd; h->off=0; h->len=st.st_size; return h;
    }
    if (has(m,"open")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[320]; asset_path(path,sizeof path,name);
      FILE*fp=fopen(path,"rb");
      
      if(!fp) return NULL;
      UHandle*h=uh_new(UJ_INPUTSTREAM); h->fp=fp; return h;
    }
    if (has(m,"list")) return jni_make_object("String[]"); /* CHECK: empty array */
    return jni_make_object("AssetManager");
  }

  /* AssetFileDescriptor.getFileDescriptor() -> FileDescriptor (carries the fd) */
  if (has(cls,"AssetFileDescriptor")){
    if (has(m,"getFileDescriptor") || has(m,"getParcelFileDescriptor")){
      UHandle*a=recv; UHandle*fd=uh_new(UJ_FD); fd->fd = is_uh(a,UJ_AFD)?a->fd:-1; return fd;
    }
    return jni_make_object("AssetFileDescriptor");
  }

  /* SharedPreferences.edit() -> Editor ; getString -> String ; getAll -> Map */
  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    if (has(m,"edit")) return uh_new(UJ_EDITOR);
    if (has(m,"getString")){
      const char *key = jni_string_utf(va_arg(va,void*));
      const char *def = jni_string_utf(va_arg(va,void*));
      KV*kv=kv_get(key);
      return jni_make_string(kv?kv->val:def);
    }
    if (has(m,"getAll")){                 /* Unity PlayerPrefs LOAD entry point */
      
      return uh_new(UJ_MAP);
    }
    if (has(m,"getStringSet")) return jni_make_object("Set");
    return jni_make_object("SharedPreferences");
  }

  /* ---- getAll() Map iteration: Map.entrySet/keySet -> Set -> Iterator -> ----
   * Entry{getKey,getValue}. The whole chain just walks the live g_kv list; the
   * Iterator carries a cursor, each Entry captures one index. Map.get(key) is
   * also handled in case Unity takes the keySet()+get() path on some build. */
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    if (has(m,"entrySet") || has(m,"keySet")) return uh_new(UJ_SET);
    if (has(m,"get")){ const char*k=jni_string_utf(va_arg(va,void*)); return uh_box_from_kv(kv_get(k)); }
    return uh_new(UJ_MAP);
  }
  if (has(cls,"java/util/Set")){
    if (has(m,"iterator")){ UHandle*it=uh_new(UJ_ITER); it->idx=0; return it; }
    return uh_new(UJ_SET);
  }
  if (has(cls,"java/util/Iterator")){
    if (has(m,"next")){                   /* return current entry, advance cursor */
      UHandle*it=recv;
      int i = is_uh(it,UJ_ITER) ? it->idx : 0;
      if (is_uh(it,UJ_ITER)) it->idx++;
      UHandle*e=uh_new(UJ_ENTRY); e->idx=i; return e;
    }
    return jni_make_object("java/util/Iterator");
  }
  if (has(cls,"java/util/Map") && has(cls,"Entry")){   /* java/util/Map$Entry */
    UHandle*e=recv; int i = is_uh(e,UJ_ENTRY) ? e->idx : -1;
    if (i<0 || i>=g_kv_n) return jni_make_string("");
    if (has(m,"getKey"))   return jni_make_string(g_kv[i].key);
    if (has(m,"getValue")) return uh_box_from_kv(&g_kv[i]);
    return jni_make_string("");
  }
  if (has(cls,"SharedPreferences$Editor")){
    /* putX all return the Editor (chained: editor.putInt(k,v).apply()), so the
     * engine reaches them via CallObjectMethod -> HERE, not the int path. All
     * four primitive puts MUST kv_set here or the write is silently dropped
     * (this was the save bug: int/bool prefs, incl. Unity's storage-version
     * marker, never persisted -> "Upgrading PlayerPrefs storage" every launch).
     * An EMPTY key means the key-encoding path (String([B)/Uri.encode) produced
     * nothing; storing under "" makes every such pref collide into one slot and
     * corrupts Screenmanager resolution -> bad res -> crash. Skip those. */
    if (has(m,"putString")){ const char*k=jni_string_utf(va_arg(va,void*));
      const char*v=jni_string_utf(va_arg(va,void*));
      if(!k[0]){  return recv; }
      kv_set('S',k,v);  return recv; }
    if (has(m,"putInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); char b[32]; snprintf(b,sizeof b,"%d",v);
      if(!k[0]){  return recv; }
      kv_set('I',k,b);  return recv; }
    if (has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      if(!k[0]){  return recv; }
      kv_set('L',k,b);  return recv; }
    if (has(m,"putFloat")){ const char*k=jni_string_utf(va_arg(va,void*));
      double v=va_arg(va,double); char b[32]; snprintf(b,sizeof b,"%.9g",v);
      if(!k[0]){  return recv; }
      kv_set('F',k,b);  return recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int);
      if(!k[0]){  return recv; }
      kv_set('B',k,v?"1":"0");  return recv; }
    if (has(m,"remove")){ const char*k=jni_string_utf(va_arg(va,void*)); kv_remove(k); return recv; }
    if (has(m,"clear")){ kv_clear(); return recv; }
    return recv;
  }

  /* Display / DisplayManager / Resources / Context: object getters */
  if (has(cls,"DisplayManager") && has(m,"getDisplay")) return jni_make_object("Display");
  if (has(cls,"res/Resources")){
    if (has(m,"getConfiguration")) return jni_make_object("Configuration");
    if (has(m,"getDisplayMetrics")) return jni_make_object("DisplayMetrics");
    return jni_make_object("Resources");
  }
  if (has(cls,"content/Context")){
    /* path getters -> our staged dir, so engine fopen()s land on the SD card */
    if (has(m,"getFilesDir")||has(m,"getCacheDir")||has(m,"getDataDir")||has(m,"getExternalFilesDir"))
      return jni_make_object("File");                 /* File.getAbsolutePath -> g_root below */
    if (has(m,"getPackageName")) return jni_make_string(AB2_PACKAGE);
    if (has(m,"getPackageCodePath")||has(m,"getPackageResourcePath")) return jni_make_string(managed_root());
    if (has(m,"getAssets")) return jni_make_object("AssetManager");
    if (has(m,"getResources")) return jni_make_object("Resources");
    if (has(m,"getSystemService")) return jni_make_object("Service");
    return jni_make_object("Context");
  }
  if (has(cls,"File") && (has(m,"getAbsolutePath")||has(m,"getPath")||has(m,"toString")))
    return jni_make_string(managed_root());

  /* UnityPlayer host queries that return objects -> benign */
  if (has(cls,"UnityPlayer")) return jni_make_object("UnityPlayer");

  return jni_make_object(cls); /* default: opaque handle, never NULL */
}

/* ---- int / boolean / long calls ----------------------------------------- */
uint64_t unity_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *rcls=unity_recv_class(recv);
  const char *cls=rcls?rcls:id->cls, *m=id->name;

  /* getAll() iteration: Iterator.hasNext + a few Map predicates. (Integer/Long/
   * Boolean unboxing is routed by RECEIVER in jni_fake.c, not here.) */
  if (has(cls,"java/util/Iterator") && has(m,"hasNext")){
    UHandle*it=recv; return (uint64_t)((is_uh(it,UJ_ITER) && it->idx < g_kv_n) ? 1 : 0);
  }
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    if (has(m,"size"))    return (uint64_t)g_kv_n;
    if (has(m,"isEmpty")) return (uint64_t)(g_kv_n==0);
    if (has(m,"containsKey")){ const char*k=jni_string_utf(va_arg(va,void*)); return (uint64_t)(kv_get(k)?1:0); }
  }

  /* InputStream.read() / read([B) / read([B,off,len) / available / skip */
  if (has(cls,"java/io/InputStream")){
    UHandle*h=recv; if(!is_uh(h,UJ_INPUTSTREAM)||!h->fp) return (uint64_t)-1;
    if (has(m,"available")){ long cur=ftell(h->fp); fseek(h->fp,0,SEEK_END);
      long end=ftell(h->fp); fseek(h->fp,cur,SEEK_SET); return (uint64_t)(end-cur); }
    if (has(m,"skip")){ long nskip=(long)va_arg(va,long long); fseek(h->fp,nskip,SEEK_CUR); return (uint64_t)nskip; }
    if (has(m,"close")){ fclose(h->fp); h->fp=NULL; return 0; }
    if (has(m,"read")){
      if (strstr(id->sig,"([B")){                     /* read(byte[][,off,len]) */
        void *arr = va_arg(va,void*);
        int alen=0; char *buf = jni_bytearray_data(arr,&alen);
        int off=0, len=alen;
        if (strstr(id->sig,"([BII)")){ off=va_arg(va,int); len=va_arg(va,int); }
        size_t got=fread(buf+off,1,(size_t)len,h->fp);
        return got? (uint64_t)got : (uint64_t)-1;     /* -1 == EOF, per InputStream */
      }
      int c=fgetc(h->fp); return (uint64_t)(c==EOF? -1 : c); /* read() one byte */
    }
    return 0;
  }

  /* SharedPreferences getters (Int/Long/Boolean + contains) */
  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    if (has(m,"contains")){ const char*k=jni_string_utf(va_arg(va,void*)); return kv_get(k)?1:0; }
    if (has(m,"getInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int def=va_arg(va,int); KV*kv=kv_get(k);
      return (uint64_t)(kv? strtoll(kv->val,NULL,10) : def); }
    if (has(m,"getLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long def=va_arg(va,long long); KV*kv=kv_get(k);
      return (uint64_t)(kv? strtoll(kv->val,NULL,10) : def); }
    if (has(m,"getBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int def=va_arg(va,int); KV*kv=kv_get(k); return (uint64_t)(kv? (kv->val[0]=='1'||kv->val[0]=='t') : def); }
    return 0;
  }
  /* Editor.putInt/Long/Boolean(...)Z?  most return the Editor (object), but
   * commit() returns Z. Route the primitive puts here since the key+value are
   * primitive-shaped, then have the engine ignore the int return. CHECK. */
  if (has(cls,"SharedPreferences$Editor")){
    if (has(m,"commit")){  prefs_flush(); return 1; }
    if (has(m,"putInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); char b[32]; snprintf(b,sizeof b,"%d",v);
      kv_set('I',k,b); return (uint64_t)(uintptr_t)recv; }
    if (has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      kv_set('L',k,b); return (uint64_t)(uintptr_t)recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); kv_set('B',k,v?"1":"0"); return (uint64_t)(uintptr_t)recv; }
    return (uint64_t)(uintptr_t)recv;
  }

  /* AssetFileDescriptor.getStartOffset()/getLength()/getDeclaredLength() (long) */
  if (has(cls,"AssetFileDescriptor")){
    UHandle*a=recv;
    if (has(m,"getStartOffset")) return (uint64_t)(is_uh(a,UJ_AFD)?a->off:0);
    if (has(m,"getLength")||has(m,"getDeclaredLength")) return (uint64_t)(is_uh(a,UJ_AFD)?a->len:0);
    return 0;
  }
  /* FileDescriptor: some engines read the raw int via a field, not a call.
   * If Unity calls FileDescriptor.getInt$()/getFd(), hand back the fd. CHECK. */
  if (has(cls,"java/io/FileDescriptor")){ UHandle*f=recv; return (uint64_t)(is_uh(f,UJ_FD)?(unsigned)f->fd:0); }

  /* Display / DisplayMetrics / Configuration ints */
  if (has(cls,"view/Display")||has(cls,"DisplayMetrics")||has(cls,"DisplayManager")){
    int docked = 0; /* CHECK: query appletGetOperationMode() at call time */
    if (has(m,"getWidth")||has(m,"WidthPixels")||has(m,"getRawWidth"))  return docked?SCREEN_W_DOCKED:SCREEN_W_HANDHELD;
    if (has(m,"getHeight")||has(m,"HeightPixels")||has(m,"getRawHeight"))return docked?SCREEN_H_DOCKED:SCREEN_H_HANDHELD;
    if (has(m,"getRotation")) return 0;
    if (has(m,"getDisplayId")) return 0;
    return 0;
  }
  return 0;
}

/* ---- float calls -------------------------------------------------------- */
float unity_dispatch_float(void *recv, const void *id_, va_list va){ const struct FakeID *id=id_;
  const char *rcls=unity_recv_class(recv);
  const char *cls=rcls?rcls:id->cls, *m=id->name;
  if (has(cls,"SharedPreferences") && !has(cls,"Editor") && has(m,"getFloat")){
    const char*k=jni_string_utf(va_arg(va,void*));
    double def=va_arg(va,double); KV*kv=kv_get(k);
    return kv?(float)strtod(kv->val,NULL):(float)def;
  }
  return 0.0f;
}

/* ---- void calls --------------------------------------------------------- */
void unity_dispatch_void(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *rcls=unity_recv_class(recv);
  const char *cls=rcls?rcls:id->cls, *m=id->name;
  if (has(cls,"java/io/InputStream") && has(m,"close")){ UHandle*h=recv;
    if(is_uh(h,UJ_INPUTSTREAM)&&h->fp){fclose(h->fp);h->fp=NULL;} return; }
  if (has(cls,"AssetFileDescriptor") && has(m,"close")){ UHandle*a=recv;
    if(is_uh(a,UJ_AFD)&&a->fd>=0){close(a->fd);a->fd=-1;} return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"apply")){  prefs_flush(); return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"putString")){ /* if routed here as void */
    const char*k=jni_string_utf(va_arg(va,void*)); const char*v=jni_string_utf(va_arg(va,void*));
    kv_set('S',k,v); return; }
  if (has(cls,"UnityPlayer")){
    /* setOrientation/lowMemory/configurationChanged/etc. -> no-op */
    return;
  }
  (void)recv;(void)va;
}

/* ========================================================================== 
 * jvalue[] dispatch. JNI's jvalue is an 8-byte union on arm64; declaring the
 * same union here lets us preserve object, integer, long and float arguments.
 * ========================================================================== */
typedef union {
  uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j;
  float f; double d; void *l;
} UJValue;

static const char *arg_string(const UJValue *a,int i){
  return (a && a[i].l)?jni_string_utf(a[i].l):"";
}

int unity_dispatch_object_a(void *recv,const void *id_,const void *args,void **out){
  const struct FakeID *id=id_; const char*m=id->name; const UJValue*a=args;
  UHandle*h=recv;
  if(!strcmp(m,"getSharedPreferences")){
    *out=uh_new(UJ_PREFS);
    
    return 1;
  }
  if(!unity_owns_recv(h)) return 0;
  if(h->kind==UJ_PREFS){
    if(has(m,"edit")){ *out=uh_new(UJ_EDITOR); return 1; }
    if(has(m,"getAll")){ 
      *out=uh_new(UJ_MAP); return 1; }
    if(has(m,"getString")){ KV*kv=kv_get(arg_string(a,0));
      *out=jni_make_string(kv?kv->val:arg_string(a,1)); return 1; }
    if(has(m,"getStringSet")){ *out=jni_make_object("Set"); return 1; }
  }
  if(h->kind==UJ_EDITOR){
    const char*k=arg_string(a,0); char b[48];
    if(has(m,"putString")){
      if(k[0]){kv_set('S',k,arg_string(a,1)); }
      *out=recv; return 1;
    }
    if(has(m,"putInt")){ int v=a?a[1].i:0; snprintf(b,sizeof b,"%d",v);
      if(k[0]){kv_set('I',k,b);} *out=recv; return 1; }
    if(has(m,"putLong")){ long long v=a?(long long)a[1].j:0; snprintf(b,sizeof b,"%lld",v);
      if(k[0]){kv_set('L',k,b);} *out=recv; return 1; }
    if(has(m,"putFloat")){ float v=a?a[1].f:0.0f; snprintf(b,sizeof b,"%.9g",(double)v);
      if(k[0]){kv_set('F',k,b);} *out=recv; return 1; }
    if(has(m,"putBoolean")){ int v=a?a[1].z:0;
      if(k[0]){kv_set('B',k,v?"1":"0");} *out=recv; return 1; }
    if(has(m,"remove")){kv_remove(k);*out=recv;return 1;}
    if(has(m,"clear")){kv_clear();*out=recv;return 1;}
  }
  if(h->kind==UJ_MAP){
    if(has(m,"entrySet")||has(m,"keySet")){*out=uh_new(UJ_SET);return 1;}
    if(has(m,"get")){*out=uh_box_from_kv(kv_get(arg_string(a,0)));return 1;}
  }
  if(h->kind==UJ_SET && has(m,"iterator")){UHandle*it=uh_new(UJ_ITER);it->idx=0;*out=it;return 1;}
  if(h->kind==UJ_ITER && has(m,"next")){UHandle*e=uh_new(UJ_ENTRY);e->idx=h->idx++;*out=e;return 1;}
  if(h->kind==UJ_ENTRY){
    int i=h->idx;
    if(i<0||i>=g_kv_n){*out=jni_make_string("");return 1;}
    if(has(m,"getKey")){*out=jni_make_string(g_kv[i].key);return 1;}
    if(has(m,"getValue")){*out=uh_box_from_kv(&g_kv[i]);return 1;}
  }
  return 0;
}

int unity_dispatch_int_a(void *recv,const void *id_,const void *args,uint64_t *out){
  const struct FakeID *id=id_; const char*m=id->name; const UJValue*a=args;
  UHandle*h=recv; if(!unity_owns_recv(h)) return 0;
  if(h->kind==UJ_PREFS){ const char*k=arg_string(a,0); KV*kv=kv_get(k);
    if(has(m,"contains")){*out=kv?1:0;return 1;}
    if(has(m,"getInt")){*out=(uint64_t)(kv?strtoll(kv->val,NULL,10):(a?a[1].i:0));return 1;}
    if(has(m,"getLong")){*out=(uint64_t)(kv?strtoll(kv->val,NULL,10):(a?a[1].j:0));return 1;}
    if(has(m,"getBoolean")){*out=kv?(kv->val[0]=='1'||kv->val[0]=='t'):(a?a[1].z:0);return 1;}
  }
  if(h->kind==UJ_EDITOR && has(m,"commit")){prefs_flush();*out=1;return 1;}
  if(h->kind==UJ_MAP){
    if(has(m,"size")){*out=g_kv_n;return 1;}
    if(has(m,"isEmpty")){*out=g_kv_n==0;return 1;}
    if(has(m,"containsKey")){*out=kv_get(arg_string(a,0))?1:0;return 1;}
  }
  if(h->kind==UJ_ITER && has(m,"hasNext")){*out=h->idx<g_kv_n;return 1;}
  return 0;
}

int unity_dispatch_float_a(void *recv,const void *id_,const void *args,float *out){
  const struct FakeID *id=id_; const UJValue*a=args; UHandle*h=recv;
  if(unity_owns_recv(h)&&h->kind==UJ_PREFS&&has(id->name,"getFloat")){
    KV*kv=kv_get(arg_string(a,0)); *out=kv?(float)strtod(kv->val,NULL):(a?a[1].f:0.0f); return 1;
  }
  return 0;
}

int unity_dispatch_void_a(void *recv,const void *id_,const void *args){
  const struct FakeID *id=id_; const UJValue*a=args; UHandle*h=recv;
  if(!unity_owns_recv(h)||h->kind!=UJ_EDITOR) return 0;
  if(has(id->name,"apply")){prefs_flush();return 1;}
  if(has(id->name,"putString")){const char*k=arg_string(a,0);if(k[0])kv_set('S',k,arg_string(a,1));return 1;}
  return 0;
}

/* ========================================================================== */
void unity_jni_init(const char *data_root){
  snprintf(g_root,sizeof g_root,"%s",data_root && *data_root ? data_root : GAME_HOME);
  snprintf(g_assets,sizeof g_assets,"%s/assets",g_root);
  prefs_load();
  /* caller (jni_fake.c jni_init) should also intern every UNITY_JNI_CLASSES[]
   * name so FindClass returns non-NULL classes. */
}
