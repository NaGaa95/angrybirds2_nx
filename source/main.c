/* Angry Birds 2 26.4.3 / Unity 6000.3.6f1 Switch wrapper. */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "unity_jni.h"
#include "unityfs_merge.h"

#define DATA_ROOT  GAME_HOME          /* sdmc:/switch/angrybird2_nx */
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* Unity FMOD audio is drained through the native direct-ByteBuffer pump. */

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
size_t g_mmap_big_align = MMAP_ARENA_ALIGN_36;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* NATIVE ENGINE CLOCK FIX (the headline black-screen fix). The port drives the
 * player loop itself and never delivers Choreographer frame callbacks, so
 * TimeManager::Update's newTime is frozen, deltaTime collapses to the 1e-5 clamp,
 * and async scene loads never integrate. Fix: hook Update's entry, replay its
 * prologue, and re-enter the frameless body with newTime = startupRef + wallclock
 * so realtimeSinceStartup advances at wall rate. Offsets in unity_entrypoints.h. */
static void (*g_unity_update_body)(void *, double) = NULL; /* 0x4410f8 Update body */
static uint64_t g_clk_base_ns = 0;
static void   *g_tm = NULL;          /* captured TimeManager instance (for the clock thread) */
static Mutex   g_clock_lock;         /* serialize main-hook vs background clock-thread ticks */
/* Unity's vsync/frame-presented counter (libunity global @ OFF_vsync_counter). No
 * real vsync here, so the clock thread bumps it to unblock frame-pacing waits. */
static volatile uint64_t *g_vsync_counter = NULL;
/* Wall-clock ns of the last main-thread Update hook; the clock thread drives the
 * engine clock only when this goes stale (main thread parked in a scene-load job). */
static volatile uint64_t g_last_main_tick_ns = 0;
#define CLOCK_STALL_NS 100000000ULL   /* 100ms of main-thread silence => treat as stalled */
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* advance the engine clock by re-running Update's body with a wall-clock newTime.
 * Shared by the main-thread Update hook and the background clock thread. */
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall = (double)(now - g_clk_base_ns) / 1e9;         /* seconds since first tick */
  /* Unity 6's TimeManager uses RationalTime internally (no simple double startupRef);
   * Update(double) converts newTime -> RationalTime and computes delta from its own
   * previous newTime. So pass wall directly: delta advances at wall rate. */
  if (g_unity_update_body) g_unity_update_body(tm, wall);
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;                                                 /* capture for the clock thread */
  g_last_main_tick_ns = nx_now_ns();                         /* main thread is driving the clock */
  *(volatile uint64_t *)((char *)tm + OFF_TM_frameCount_u64) += 1; /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + OFF_TM_aux_u32) += 1;        /* aux counter++            */
  if (*(volatile uint8_t *)((char *)tm + OFF_TM_pause_u8) != 0) return; /* paused -> early ret */
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
/* Keep the engine clock ticking from an independent thread while nativeRender is
 * parked in a synchronous scene-load job (else the clock freezes -> black screen).
 * trylock so we never block the main hook or invert lock order. */
static Thread g_clock_thr;
extern void choreographer_tick(int64_t now_ns);  /* imports.c: drive Unity 6 NDK frame callbacks */
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  uint64_t last_choreo = 0;
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);                  /* ~8ms => ~120Hz clock keep-alive */
    /* simulate a vsync tick so libunity's frame-pacing waits advance (always) */
    if (g_vsync_counter) __atomic_add_fetch(g_vsync_counter, 1, __ATOMIC_RELAXED);
    /* Drive Unity 6's NDK AChoreographer frame callbacks at ~60Hz so nativeRender's
     * NdkLooper frame-wait is fed and the player loop advances. */
    uint64_t now = nx_now_ns();
    if (now - last_choreo >= 16000000ULL) { last_choreo = now; choreographer_tick((int64_t)now); }
    /* Drive the clock here ONLY while the main thread is stalled; ticking during
     * normal play would fragment Time.deltaTime into slow motion. */
    void *tm = g_tm;
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {
      nx_clock_tick(tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_start_clock_thread(void) {
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  /* vsync/frame-presented counter (0 until recovered -> clock thread skips the bump) */
  g_vsync_counter = OFF_vsync_counter ? (volatile uint64_t *)(ub + OFF_vsync_counter) : NULL;
  if (!OFF_TimeManager_Update_entry || !OFF_TimeManager_Update_body) {
    return;
  }
  g_unity_update_body = (void (*)(void *, double))(ub + OFF_TimeManager_Update_body);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  so_patch_code((void *)(ub + OFF_TimeManager_Update_entry), stub, sizeof stub);
}

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }

#if AB2_BOOST_INITIAL_LOADING
/* FastLoad raises the CPU clock to 1785 MHz but lowers GPU clocks, so keep it
 * through Boot/Main setup and restore normal clocks when Menu/gameplay loads. */
typedef bool (*scene_predicate_fn)(int32_t scene_handle, void *method);

static scene_predicate_fn g_scene_is_boot;
static scene_predicate_fn g_scene_is_main;
static bool g_boot_scene_seen;
static bool g_main_scene_seen;
static bool g_boot_boost_active;
void *g_boot_scene_loaded_resume;
uintptr_t g_boot_scene_loaded_x22_page;

static void nx_boot_boost_start(void) {
  cpu_boost(1);
  g_boot_boost_active = true;
}

static void nx_boot_boost_stop(void) {
  if (!g_boot_boost_active) return;
  cpu_boost(0);
  g_boot_boost_active = false;
}

__attribute__((used)) void nx_boot_boost_scene_loaded(int32_t scene_handle,
                                                       int32_t mode, void *method) {
  (void)mode;
  (void)method;
  if (!g_boot_boost_active) return;
  if (g_scene_is_boot(scene_handle, NULL)) {
    g_boot_scene_seen = true;
    return;
  }
  if (g_scene_is_main(scene_handle, NULL)) {
    g_main_scene_seen = true;
    return;
  }
  if (g_boot_scene_seen || g_main_scene_seen) nx_boot_boost_stop();
}

__asm__(
".text\n.align 2\n.global ab2_boot_scene_loaded_trampoline\n"
".type ab2_boot_scene_loaded_trampoline,%function\n"
"ab2_boot_scene_loaded_trampoline:\n"
"  stp x0, x1, [sp, #-0x20]!\n"
"  stp x2, x30, [sp, #0x10]\n"
"  bl  nx_boot_boost_scene_loaded\n"
"  ldp x2, x30, [sp, #0x10]\n"
"  ldp x0, x1, [sp], #0x20\n"
"  str x30, [sp, #-0x30]!\n"
"  stp x22, x21, [sp, #0x10]\n"
"  stp x20, x19, [sp, #0x20]\n"
"  adrp x16, g_boot_scene_loaded_x22_page\n"
"  add  x16, x16, :lo12:g_boot_scene_loaded_x22_page\n"
"  ldr  x22, [x16]\n"
"  adrp x16, g_boot_scene_loaded_resume\n"
"  add  x16, x16, :lo12:g_boot_scene_loaded_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n");
extern void ab2_boot_scene_loaded_trampoline(void);

static uintptr_t nx_decode_adrp(uintptr_t pc, uint32_t instruction) {
  int64_t pages = (int64_t)((((instruction >> 5) & 0x7ffffu) << 2) |
                            ((instruction >> 29) & 3u));
  if (pages & (1LL << 20)) pages -= 1LL << 21;
  return (uintptr_t)((intptr_t)(pc & ~(uintptr_t)0xfff) + pages * 0x1000);
}

static void nx_boot_boost_install_scene_hook(uintptr_t ib) {
  volatile uint32_t *is_boot = (volatile uint32_t *)(ib + OFF_IL2CPP_SCENE_IS_BOOT);
  volatile uint32_t *is_main = (volatile uint32_t *)(ib + OFF_IL2CPP_SCENE_IS_MAIN);
  volatile uint32_t *loaded =
      (volatile uint32_t *)(ib + OFF_IL2CPP_INTERNAL_SCENE_LOADED);
  if (is_boot[0] != AB2_SCENE_PREDICATE_ENTRY_0 ||
      is_boot[1] != AB2_SCENE_PREDICATE_ENTRY_1 ||
      is_main[0] != AB2_SCENE_PREDICATE_ENTRY_0 ||
      is_main[1] != AB2_SCENE_PREDICATE_ENTRY_1 ||
      loaded[0] != AB2_SCENE_LOADED_ENTRY_0 ||
      loaded[1] != AB2_SCENE_LOADED_ENTRY_1 ||
      loaded[2] != AB2_SCENE_LOADED_ENTRY_2 ||
      loaded[3] != AB2_SCENE_LOADED_ENTRY_3) {
    nx_boot_boost_stop();
    return;
  }

  g_scene_is_boot = (scene_predicate_fn)is_boot;
  g_scene_is_main = (scene_predicate_fn)is_main;
  g_boot_scene_loaded_x22_page = nx_decode_adrp((uintptr_t)&loaded[3], loaded[3]);
  g_boot_scene_loaded_resume = (void *)&loaded[4];
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_boot_scene_loaded_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_boot_scene_loaded_trampoline >> 32),
  };
  so_patch_code((void *)loaded, stub, sizeof stub);
}
#endif

/* Mapping zone for libmain, libunity, and libil2cpp with relocation headroom. */
#define SO_REGION_BYTES (288u * 1024 * 1024)

static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  const u64 address_limit = (u64)g_mmap_big_align << 12;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  if (sbase >= address_limit) return NULL;
  /* libnx/kernel create thread stacks + per-thread TLS (MemType_ThreadLocal) AFTER we arm
   * the window; virtmemAddReservation does NOT keep the kernel's TLS out. Empirically they
   * cluster in the LOW ~768MB of the stack region (observed up to ~0x309fb000). A window
   * overlapping that zone later fails to svcMapMemory a commit -> FATAL "Memory conflict
   * committing the game heap" (seen at 0x19000000 when the window landed at 0x8000000). So
   * prefer the largest hole that STARTS above the low zone; fall back to the absolute
   * largest only if no usable high hole exists. */
  const u64 lo_zone = sbase + 0x30000000ull;   /* skip the thread-resource churn band */
  u64 end = sbase + ssize;
  if (end < sbase || end > address_limit)
    end = address_limit;
  u64 a = sbase, best_a = 0, best_l = 0, hi_a = 0, hi_l = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (hs >= lo_zone && he - hs > hi_l) { hi_l = he - hs; hi_a = hs; }
      }
    }
    a = me;
  }
  /* Prefer a high hole (avoids the thread-resource zone) if it is usefully large; the
   * heap-backed arena absorbs any window shortfall, so a smaller-but-safe window wins. */
  int chose_hi = (hi_a && hi_l >= (256ull << 20));
  if (chose_hi) { best_a = hi_a; best_l = hi_l; }
  if (!best_a) return NULL;
  u64 aligned = (best_a + (g_mmap_big_align - 1)) & ~(g_mmap_big_align - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(g_mmap_big_align - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap used by the Android libraries. */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  const u64 limit36 = 1ull << 36;
  u64 aslr_base = 0, aslr_size = 0, stack_base = 0;
  const int aslr39 =
      R_SUCCEEDED(svcGetInfo(&aslr_base, InfoType_AslrRegionAddress,
                             CUR_PROCESS_HANDLE, 0)) &&
      R_SUCCEEDED(svcGetInfo(&aslr_size, InfoType_AslrRegionSize,
                             CUR_PROCESS_HANDLE, 0)) &&
      (aslr_base >= limit36 || aslr_size > limit36 - aslr_base);
  const int stack39 =
      R_SUCCEEDED(svcGetInfo(&stack_base, InfoType_StackRegionAddress,
                             CUR_PROCESS_HANDLE, 0)) &&
      stack_base >= limit36;
  if (aslr39 || stack39)
    g_mmap_big_align = MMAP_ARENA_ALIGN_39;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed aligned arena (no overcommit). */
  const size_t big_align    = g_mmap_big_align;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * 30) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* Verify the essential data files exist. unityfs_prepare() accepts either the
 * desktop-staged merged archive or the two ordinary APK asset trees. */
static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    /* The original base archive remains present in both supported layouts. */
    "assets/bin/Data/data.unity3d",
    /* Loose PAD streaming side-file (textures/audio); still referenced by bare name. */
    "assets/bin/Data/resources.resource",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n%s\nCheck your SD card layout (see BUILD.md).", files[i]);
  }
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;


static int nx_patch_unity_regions(uintptr_t ub) {
  static const struct { uint32_t off, from, to36, to39; } P[] = {
    {0xc5352c, 0x12be0009, 0x12bfc009, 0x12bf0009},
    {0xc5353c, 0x92648d35, 0x92679935, 0x92659135},
    {0xc53dbc, 0x52a20008, 0x52a04008, 0x52a10008},
    {0xc54f30, 0x52a20009, 0x52a04009, 0x52a10009},
    {0xc57014, 0x52a20009, 0x52a04009, 0x52a10009},
    {0xc574e0, 0x12be000d, 0x12bfc00d, 0x12bf000d},
    {0xc5750c, 0x92648d36, 0x92679936, 0x92659136},
    {0xc58cb0, 0xd35cdc33, 0xd359d033, 0xd35bd833},
    {0xc58cb8, 0xd35cfd15, 0xd359fd15, 0xd35bfd15},
    {0xc58d5c, 0x52a20008, 0x52a04008, 0x52a10008},
    {0xc58f8c, 0xd35cfc28, 0xd359fc28, 0xd35bfc28},
    {0xc58f9c, 0x92646c28, 0x92677828, 0x92657028},
    {0xc58fa4, 0xd35c9c2a, 0xd359902a, 0xd35b982a},
    {0xc58fb8, 0xd35cdc29, 0xd359d029, 0xd35bd829},
    {0xc58fbc, 0xb25c6feb, 0xb25f7beb, 0xb25d73eb},
    {0xc58fc0, 0xf2a2000b, 0xf2a0400b, 0xf2a1000b},
    {0xc59004, 0xcb0a7108, 0xcb0a6508, 0xcb0a6d08},
    {0xc59028, 0xd35c9c29, 0xd3599029, 0xd35b9829},
    {0xc5aa00, 0xd35c9e89, 0xd3599289, 0xd35b9a89},
  };
  const int N = (int)(sizeof P / sizeof P[0]);
  if (N == 0) {
    return 0;
  }
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur != P[i].from) {
      return 0;
    }
  }
  const int use39 = g_mmap_big_align == MMAP_ARENA_ALIGN_39;
  for (int i = 0; i < N; i++) {
    const uint32_t replacement = use39 ? P[i].to39 : P[i].to36;
    so_patch_code((void *)(ub + P[i].off), &replacement, sizeof replacement);
  }
  return 1;
}

#if AB2_SKIP_GPGS_ACTIVATE

static void nx_skip_gpgs_activate(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x088A7520u);
  const uint32_t RET = 0xd65f03c0u;   /* ret */
  if (*site != 0x17e97e02u) {         /* b 0x08306D28 */
    return;
  }
  so_patch_code((void *)site, &RET, sizeof RET);
}
#endif

#if AB2_FAKE_ANDROID_PERMISSIONS
/* AndroidPermissionsPlugin cannot perform a Java permission flow on Switch. Its
 * InitializeJavaPlugin method catches the missing UnityPlayer Java-field error, but
 * the exception still pollutes early boot; any denied result would also wait for an
 * Android callback that can never arrive. Skip the Java bootstrap and report every
 * permission granted. Both AB2 26.4.3 sites are objdump-verified. */
static void nx_patch_android_permissions(uintptr_t ib) {
  volatile uint32_t *init = (volatile uint32_t *)(ib + 0x04354014u);
  volatile uint32_t *check = (volatile uint32_t *)(ib + 0x04354C6Cu);
  if (*init == 0xd10183ffu) {                  /* sub sp,sp,#0x60 */
    const uint32_t RET = 0xd65f03c0u;
    so_patch_code((void *)init, &RET, sizeof RET);
  }
  if (check[0] == 0xa9bd5ffeu && check[1] == 0xa90157f6u) {
    const uint32_t GRANTED[2] = { 0x52800020u /* mov w0,#1 */, 0xd65f03c0u /* ret */ };
    so_patch_code((void *)check, GRANTED, sizeof GRANTED);
  }
}
#endif

#if AB2_DIRECT_LOCALE
typedef void *(*il2cpp_string_new_fn)(const char *utf8);
static il2cpp_string_new_fn p_locale_string_new;

/* Rovio.Abba.Frameworks.Localization.Locale.GetCurrentLocale()
 * @il2cpp+0x089EC85C is entirely an AndroidJavaClass/AndroidJavaObject wrapper:
 *   new AndroidJavaClass("java.util.Locale")
 *     .CallStatic<AndroidJavaObject>("getDefault").Call<string>("toString")
 * The first call returns a live fake Locale, but Unity's managed Java-reflection
 * method lookup returns null before the second call reaches dispatch_object().
 * Switch already supplied the locale to Unity's native JNI path successfully;
 * create the same Android-style managed string directly and avoid the redundant
 * Android reflection round-trip. Static IL2CPP methods pass MethodInfo* in x0;
 * it is intentionally ignored here and the Il2CppString* is returned in x0. */
static void *nx_locale_get_current(void *method) {
  (void)method;
  const char *locale = jni_locale_name();
  void *result = p_locale_string_new ? p_locale_string_new(locale) : NULL;
  return result;
}

static void nx_patch_direct_locale(uintptr_t ib) {
  p_locale_string_new = (il2cpp_string_new_fn)
      so_try_find_addr_rx(&il2cpp_mod, "il2cpp_string_new");
  if (!p_locale_string_new) {
    return;
  }
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x089EC85Cu);
  if (site[0] != 0xd10183ffu || site[1] != 0xf9001bfeu ||
      site[2] != 0xa90457f6u || site[3] != 0xa9054ff4u) {
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_locale_get_current & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_locale_get_current >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_SKIP_COSTUME_PRELOAD
typedef void *(*task_get_completed_fn)(void *method);
static task_get_completed_fn p_task_get_completed;

/* Rovio.Abba.Costumes.Client.Initializers.CostumeBundlesBackgroundInitializer
 * .InitializeAsync() @il2cpp+0x08968984 starts a full preload of the 222MB
 * expanded costume archive and stores that task at this+0x38. Hardware proves
 * the native AssetBundle request reads all 95,455,638 bytes and closes, but its
 * managed completion callback is never delivered, so ContentDownload later
 * awaits the field forever. Both costume initializers are declared NonBlocking,
 * and OverridableAssetProvider retains synchronous/lazy lookup paths. Store and
 * return the runtime's rooted Task.CompletedTask, skipping only eager warmup. */
static void *nx_costume_preload_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  if (self && task)
    __atomic_store_n((void **)((char *)self + 0x38), task, __ATOMIC_RELEASE);
  return task;
}

static void nx_skip_costume_preload(uintptr_t ib) {
  volatile uint32_t *completed = (volatile uint32_t *)(ib + 0x087B3FFCu);
  if (completed[0] != 0xf81e0ffeu || completed[1] != 0xa9014ff4u) {
    return;
  }
  p_task_get_completed = (task_get_completed_fn)completed;

  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x08968984u);
  if (site[0] != 0xd101c3ffu || site[1] != 0xa90557feu ||
      site[2] != 0xa9064ff4u || site[3] != 0xf000f755u) {
    p_task_get_completed = NULL;
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_costume_preload_completed & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_costume_preload_completed >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}

#if AB2_SKIP_CRASH_TRACKER_BOOTSTRAP

static void *nx_crash_trackers_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void nx_skip_crash_tracker_bootstrap(uintptr_t ib) {
  if (!p_task_get_completed) {
    return;
  }

  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x089E5448u);
  if (site[0] != 0xd101c3ffu || site[1] != 0xa90557feu ||
      site[2] != 0xa9064ff4u || site[3] != 0xf000f375u) {
    return;
  }

  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_crash_trackers_completed & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_crash_trackers_completed >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_SYNC_LOCAL_ADDRESSABLE_BUNDLES

typedef void *(*ab2_load_bundle_sync_fn)(void *path, uint32_t crc, uint64_t offset,
                                         void *method);
typedef void (*ab2_complete_bundle_load_fn)(void *self, void *bundle, void *method);
static ab2_load_bundle_sync_fn p_ab2_load_bundle_sync;
static ab2_complete_bundle_load_fn p_ab2_complete_bundle_load;

static void nx_addressables_load_local_sync(void *self, void *method) {
  (void)method;
  if (!self || !p_ab2_load_bundle_sync || !p_ab2_complete_bundle_load) {
    return;
  }

  void *options = __atomic_load_n((void **)((char *)self + 0x40), __ATOMIC_ACQUIRE);
  void *path = __atomic_load_n((void **)((char *)self + 0x78), __ATOMIC_ACQUIRE);
  uint32_t crc = options ? *(uint32_t *)((char *)options + 0x18) : 0;
  *(int32_t *)((char *)self + 0x54) = 1; /* BundleSource.Local */

  void *bundle = p_ab2_load_bundle_sync(path, crc, 0, NULL);
  p_ab2_complete_bundle_load(self, bundle, NULL);
}

static void nx_patch_sync_local_addressables(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x09682938u);
  static const uint32_t expected[4] = {
    0xf81d0ffeu, 0xa90157f6u, 0xa9024ff4u, 0xb0008ed4u,
  };
  if (site[0] != expected[0] || site[1] != expected[1] ||
      site[2] != expected[2] || site[3] != expected[3]) {
    return;
  }

  p_ab2_load_bundle_sync =
      (ab2_load_bundle_sync_fn)(ib + 0x0975EC48u);
  p_ab2_complete_bundle_load =
      (ab2_complete_bundle_load_fn)(ib + 0x09682E68u);
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_addressables_load_local_sync & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_addressables_load_local_sync >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_SYNC_BUNDLED_ADDRESSABLE_ASSETS

typedef void *(*ab2_load_asset_sync_fn)(void *bundle, void *name, void *type,
                                        void *method);
typedef void (*ab2_get_asset_result_fn)(void *self, void *asset, void *method);
typedef void (*ab2_complete_asset_op_fn)(void *self, void *method);
static ab2_load_asset_sync_fn p_ab2_load_asset_sync;
static ab2_get_asset_result_fn p_ab2_get_asset_result;
static ab2_complete_asset_op_fn p_ab2_complete_asset_op;

__attribute__((noinline, used))
void nx_addressables_load_asset_sync(void *self, void *bundle, void *name,
                                     void *type) {
  if (!self || !bundle || !name || !type || !p_ab2_load_asset_sync ||
      !p_ab2_get_asset_result || !p_ab2_complete_asset_op) {
    return;
  }

  void *asset = p_ab2_load_asset_sync(bundle, name, type, NULL);
  p_ab2_get_asset_result(self, asset, NULL);
  p_ab2_complete_asset_op(self, NULL);
}

/* This is entered with a plain branch from the middle of BeginAssetLoad. Keep
 * its stack frame intact while calling C, then perform the exact stock epilogue
 * at il2cpp+0x09685DE0 and return to BeginAssetLoad's original caller. */
__asm__(
    ".text\n"
    ".align 2\n"
    ".global nx_addressables_asset_sync_bridge\n"
    ".type nx_addressables_asset_sync_bridge, %function\n"
    "nx_addressables_asset_sync_bridge:\n"
    "mov x3, x0\n"
    "mov x2, x20\n"
    "mov x1, x21\n"
    "mov x0, x19\n"
    "bl nx_addressables_load_asset_sync\n"
    "ldp x20, x19, [sp, #48]\n"
    "ldr x30, [sp, #16]\n"
    "ldp x22, x21, [sp, #32]\n"
    "add sp, sp, #64\n"
    "ret\n"
    ".size nx_addressables_asset_sync_bridge, .-nx_addressables_asset_sync_bridge\n");
extern void nx_addressables_asset_sync_bridge(void);

static void nx_patch_sync_bundled_addressable_assets(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x09685E04u);
  static const uint32_t expected[4] = {
    0xaa0003e2u, /* mov x2, x0  */
    0xaa1503e0u, /* mov x0, x21 */
    0xaa1403e1u, /* mov x1, x20 */
    0xaa1f03e3u, /* mov x3, xzr */
  };
  if (site[0] != expected[0] || site[1] != expected[1] ||
      site[2] != expected[2] || site[3] != expected[3]) {
    return;
  }

  p_ab2_load_asset_sync =
      (ab2_load_asset_sync_fn)(ib + 0x0975F174u);
  p_ab2_get_asset_result =
      (ab2_get_asset_result_fn)(ib + 0x09686104u);
  p_ab2_complete_asset_op =
      (ab2_complete_asset_op_fn)(ib + 0x0968629Cu);
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_addressables_asset_sync_bridge & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_addressables_asset_sync_bridge >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_SKIP_FACEBOOK_INIT

static void *nx_facebook_init_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void nx_skip_facebook_init(uintptr_t ib) {
  if (!p_task_get_completed) {
    volatile uint32_t *completed = (volatile uint32_t *)(ib + 0x087B3FFCu);
    if (completed[0] != 0xf81e0ffeu || completed[1] != 0xa9014ff4u) {
      return;
    }
    p_task_get_completed = (task_get_completed_fn)completed;
  }
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04BC1598u);
  if (site[0] != 0xd101c3ffu || site[1] != 0xa90557feu ||
      site[2] != 0xa9064ff4u || site[3] != 0x9002e435u) {
    return;
  }

  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_facebook_init_completed & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_facebook_init_completed >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}

/* CleanObsoleteAssetsInitializer.InitializeAsync is a blocking group-3 wrapper
 * around ObsoleteAssetsCleaner. With the still-incomplete PlayerPrefs key bridge,
 * its stored cleanup timestamp is an empty string and DateTime.Parse throws. Cache
 * eviction is optional for this fixed offline payload, so complete this initializer
 * locally and leave all packaged Addressables/assets intact. */
static void *nx_clean_obsolete_assets_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void nx_skip_clean_obsolete_assets(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04B7A8F8u);
  const uint32_t expected[4] = {
    0xf9400800u, 0xb4000060u, 0xaa1f03e1u, 0x14006095u
  };
  if (site[0] != expected[0] || site[1] != expected[1] ||
      site[2] != expected[2] || site[3] != expected[3]) {
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_clean_obsolete_assets_completed & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_clean_obsolete_assets_completed >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif
#endif

#if AB2_BYPASS_PLAY_INTEGRITY
typedef void *(*integrity_bool_to_async_fn)(bool value, void *method);
static integrity_bool_to_async_fn p_integrity_bool_to_async;

/* IntegrityTokenProviderAdapter..ctor @il2cpp+0x08C2BB70 eagerly creates
 * Google.Play.Integrity.IntegrityManager. Its nested PlayCoreIntegrityManager
 * obtains com.google.android.play.core.integrity.IntegrityManager through Java;
 * the fake Android boundary correctly returns null, and Play Core throws before
 * VContainer can finish resolving the device-check service. Preserve the logger
 * field and leave only the unavailable manager field at its allocation-time null. */
static void nx_integrity_token_provider_offline_ctor(void *self, void *logger,
                                                      void *method) {
  (void)method;
  if (self)
    *(void **)((char *)self + 0x10) = logger;
}

/* IntegrityDeviceCheck.Check @il2cpp+0x08C2AF64 is the sole consumer of that
 * adapter in the device-check graph. The real path also requests a nonce and
 * reports attestation to the retired backend. For the offline Switch profile,
 * return the game's own cached Task<bool>(true) so the EventRun completion flow
 * can release its operation and transition back to Menu. */
static void *nx_integrity_device_check_offline(void *self, void *method) {
  (void)method;
  void *task = p_integrity_bool_to_async
      ? p_integrity_bool_to_async(true, NULL)
      : NULL;
  return task;
}

static void nx_bypass_play_integrity(uintptr_t ib) {
  volatile uint32_t *to_async = (volatile uint32_t *)(ib + 0x08809580u);
  volatile uint32_t *adapter_ctor = (volatile uint32_t *)(ib + 0x08C2BB70u);
  volatile uint32_t *check = (volatile uint32_t *)(ib + 0x08C2AF64u);

  if (to_async[0] != 0xa9be57feu || to_async[1] != 0xa9014ff4u ||
      to_async[2] != 0xd0010255u || to_async[3] != 0x9000cb14u) {
    return;
  }
  if (adapter_ctor[0] != 0xf81d0ffeu || adapter_ctor[1] != 0xa90157f6u ||
      adapter_ctor[2] != 0xa9024ff4u || adapter_ctor[3] != 0xd000e156u) {
    return;
  }
  if (check[0] != 0xd10243ffu || check[1] != 0xf90033feu ||
      check[2] != 0xa90757f6u || check[3] != 0xa9084ff4u) {
    return;
  }

  p_integrity_bool_to_async = (integrity_bool_to_async_fn)to_async;
  const uint32_t ctor_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_integrity_token_provider_offline_ctor & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_integrity_token_provider_offline_ctor >> 32),
  };
  const uint32_t check_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_integrity_device_check_offline & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_integrity_device_check_offline >> 32),
  };
  so_patch_code((void *)adapter_ctor, ctor_stub, sizeof ctor_stub);
  so_patch_code((void *)check, check_stub, sizeof check_stub);
}
#endif

#if AB2_DISABLE_ANDROID_HEADPHONE_BRIDGE

static void nx_headphone_detector_init_disabled(void *self, void *event_manager,
                                                void *method) {
  (void)event_manager;
  (void)method;
}

static void nx_disable_android_headphone_bridge(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04C5370Cu);
  if (site[0] != 0xa9bc67feu || site[1] != 0xa9015ff8u ||
      site[2] != 0xa90257f6u || site[3] != 0xa9034ff4u) {
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_headphone_detector_init_disabled & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_headphone_detector_init_disabled >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_DISABLE_ANDROID_PLATFORM_DETECTION

static void nx_android_platform_detection_disabled(void *method) {
  (void)method;
}

static void nx_disable_android_platform_detection(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x08B188D8u);
  if (site[0] != 0xf81c0ffeu || site[1] != 0xa9015ff8u ||
      site[2] != 0xa90257f6u || site[3] != 0xa9034ff4u) {
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_android_platform_detection_disabled & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_android_platform_detection_disabled >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_SKIP_BEACON_LOGIN && AB2_SKIP_COSTUME_PRELOAD

static void *nx_beacon_login_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void nx_skip_beacon_login(uintptr_t ib) {
  if (!p_task_get_completed) {
    volatile uint32_t *completed = (volatile uint32_t *)(ib + 0x087B3FFCu);
    if (completed[0] != 0xf81e0ffeu || completed[1] != 0xa9014ff4u) {
      return;
    }
    p_task_get_completed = (task_get_completed_fn)completed;
  }
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04C94758u);
  if (site[0] != 0xd10243ffu || site[1] != 0xa90757feu ||
      site[2] != 0xa9084ff4u || site[3] != 0xb002dd95u) {
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_beacon_login_completed & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_beacon_login_completed >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_SKIP_ANDROID_NOTIFICATIONS && AB2_SKIP_COSTUME_PRELOAD

static void *nx_notifications_init_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void nx_skip_android_notifications(uintptr_t ib) {
  if (!p_task_get_completed) {
    volatile uint32_t *completed = (volatile uint32_t *)(ib + 0x087B3FFCu);
    if (completed[0] != 0xf81e0ffeu || completed[1] != 0xa9014ff4u) {
      return;
    }
    p_task_get_completed = (task_get_completed_fn)completed;
  }

  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04D4B7A8u);
  if (site[0] != 0xd10103ffu || site[1] != 0xa9015ffeu ||
      site[2] != 0xa90257f6u || site[3] != 0xa9034ff4u) {
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_notifications_init_completed & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_notifications_init_completed >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_SUPPRESS_OFFLINE_NETWORK_POPUPS && AB2_SKIP_COSTUME_PRELOAD

typedef void *(*bool_to_async_fn)(bool value, void *method);
static bool_to_async_fn p_bool_to_async;

static void *nx_unrecoverable_offline_popup_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void *nx_concrete_unrecoverable_popup_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void *nx_concrete_retry_popup_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void *nx_concrete_closable_popup_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void *nx_retry_popup_cancelled(void *self, void *bottom_text_override,
                                      bool can_close, void *method) {
  (void)method;
  void *task = p_bool_to_async ? p_bool_to_async(false, NULL) : NULL;
  return task;
}

#if AB2_SKIP_OFFLINE_COSTUME_VALIDATION
/* CostumesDataValidator.ValidateCostumesDataAsync is an online consistency/repair
 * pass. On StartScreen re-entry its generated MoveNext loops through the unclosable
 * retry factory while the port is intentionally offline. Because our popup result
 * is already completed, that loop runs synchronously and consumes the remaining
 * mmap/newlib heap in seconds. Preserve the lazy local costume provider and bypass
 * only this backend validator with the runtime's rooted completed task. */
static void *nx_offline_costume_validation_completed(void *self, void *method) {
  (void)method;
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}
#endif

static void *nx_backend_maintenance_popup_completed(void *self,
                                                     void *login_error_response,
                                                     void *method) {
  (void)self; (void)login_error_response; (void)method;
  return p_task_get_completed ? p_task_get_completed(NULL) : NULL;
}

static bool nx_upcoming_maintenance_hidden(void *self, void *method) {
  (void)self; (void)method;
  return false;
}

static bool nx_core_offline_coroutine_finished(void *state_machine, void *method) {
  (void)state_machine; (void)method;
  return false;
}

static bool nx_download_offline_coroutine_finished(void *state_machine, void *method) {
  (void)state_machine; (void)method;
  return false;
}

static bool nx_backend_player_offline_coroutine_finished(void *state_machine, void *method) {
  (void)state_machine; (void)method;
  return false;
}

static bool nx_third_party_offline_coroutine_finished(void *state_machine, void *method) {
  (void)state_machine; (void)method;
  return false;
}

/* InternetChecker's retry coroutine owns an Action<bool> at +0x38.  Returning
 * false without completing that callback would hide the popup but strand its
 * caller.  Mirror the exact delegate invocation emitted by IL2CPP at 0x046FAF38:
 * method_ptr(+0x18)(target(+0x40), true, MethodInfo(+0x28)), then finish the
 * iterator.  This is restricted to the Switch port's forced-offline build. */
typedef void (*nx_action_bool_invoke_fn)(void *target, bool value, void *method);
static bool nx_internet_checker_reachable(void *state_machine, void *method) {
  (void)method;
  void *callback = state_machine ? *(void **)((char *)state_machine + 0x38) : NULL;
  void *target = callback ? *(void **)((char *)callback + 0x40) : NULL;
  void *callback_method = callback ? *(void **)((char *)callback + 0x28) : NULL;
  nx_action_bool_invoke_fn invoke = callback
      ? (nx_action_bool_invoke_fn)*(void **)((char *)callback + 0x18)
      : NULL;
  if (invoke) invoke(target, true, callback_method);
  return false;
}

static bool nx_startup_connection_error_hidden(void *self, void *method) {
  (void)self; (void)method;
  return false;
}

static void nx_suppress_offline_network_popups(uintptr_t ib) {
  if (!p_task_get_completed) {
    volatile uint32_t *completed = (volatile uint32_t *)(ib + 0x087B3FFCu);
    if (completed[0] != 0xf81e0ffeu || completed[1] != 0xa9014ff4u) {
      return;
    }
    p_task_get_completed = (task_get_completed_fn)completed;
  }

  volatile uint32_t *bool_to_async = (volatile uint32_t *)(ib + 0x08809580u);
  if (bool_to_async[0] == 0xa9be57feu && bool_to_async[1] == 0xa9014ff4u &&
      bool_to_async[2] == 0xd0010255u && bool_to_async[3] == 0x9000cb14u) {
    p_bool_to_async = (bool_to_async_fn)bool_to_async;
  } else {
    p_bool_to_async = NULL;
  }

  volatile uint32_t *unrecoverable = (volatile uint32_t *)(ib + 0x089F8B54u);
  if (unrecoverable[0] == 0xd101c3ffu && unrecoverable[1] == 0xa90557feu &&
      unrecoverable[2] == 0xa9064ff4u && unrecoverable[3] == 0x9000f2f5u) {
    const uint32_t stub[4] = {
      0x58000050u, 0xd61f0200u,
      (uint32_t)((uintptr_t)&nx_unrecoverable_offline_popup_completed & 0xffffffffu),
      (uint32_t)((uintptr_t)&nx_unrecoverable_offline_popup_completed >> 32),
    };
    so_patch_code((void *)unrecoverable, stub, sizeof stub);
  }

  struct offline_factory_patch {
    uintptr_t offset;
    uintptr_t replacement;
    const char *name;
  };
  const struct offline_factory_patch factory_patches[] = {
    {0x04D6F8F0u, (uintptr_t)&nx_concrete_unrecoverable_popup_completed, "unrecoverable"},
    {0x04D6F9C8u, (uintptr_t)&nx_concrete_retry_popup_completed, "retry"},
    {0x04D6FAA0u, (uintptr_t)&nx_concrete_closable_popup_completed, "closable"},
  };
  for (size_t i = 0; i < sizeof factory_patches / sizeof factory_patches[0]; ++i) {
    const struct offline_factory_patch *patch = &factory_patches[i];
    volatile uint32_t *site = (volatile uint32_t *)(ib + patch->offset);
    if (site[0] == 0xd101c3ffu && site[1] == 0xa90557feu &&
        site[2] == 0xa9064ff4u && site[3] == 0xf002d6b5u) {
      const uint32_t stub[4] = {
        0x58000050u, 0xd61f0200u,
        (uint32_t)(patch->replacement & 0xffffffffu),
        (uint32_t)(patch->replacement >> 32),
      };
      so_patch_code((void *)site, stub, sizeof stub);
    }
  }

  if (p_bool_to_async) {
    volatile uint32_t *retry_factory = (volatile uint32_t *)(ib + 0x048BCA94u);
    if (retry_factory[0] == 0xa9bc67feu && retry_factory[1] == 0xa9015ff8u &&
        retry_factory[2] == 0xa90257f6u && retry_factory[3] == 0xa9034ff4u) {
      const uint32_t stub[4] = {
        0x58000050u, 0xd61f0200u,
        (uint32_t)((uintptr_t)&nx_retry_popup_cancelled & 0xffffffffu),
        (uint32_t)((uintptr_t)&nx_retry_popup_cancelled >> 32),
      };
      so_patch_code((void *)retry_factory, stub, sizeof stub);
    }
  }

#if AB2_SKIP_OFFLINE_COSTUME_VALIDATION
  /* CostumesDataValidator.ValidateCostumesDataAsync
   *   0x04C0504C: sub sp,sp,#0x80; stp x30,x21,[sp,#0x60];
   *               stp x20,x19,[sp,#0x70]; adrp x21,...
   * The method merely creates/starts its async state machine, so returning the
   * rooted completed Task is ABI-compatible with every caller. */
  volatile uint32_t *costume_validator = (volatile uint32_t *)(ib + 0x04C0504Cu);
  if (costume_validator[0] == 0xd10203ffu && costume_validator[1] == 0xa90657feu &&
      costume_validator[2] == 0xa9074ff4u && costume_validator[3] == 0x9002e215u) {
    const uint32_t stub[4] = {
      0x58000050u, 0xd61f0200u,
      (uint32_t)((uintptr_t)&nx_offline_costume_validation_completed & 0xffffffffu),
      (uint32_t)((uintptr_t)&nx_offline_costume_validation_completed >> 32),
    };
    so_patch_code((void *)costume_validator, stub, sizeof stub);
  }
#endif

  volatile uint32_t *backend_maintenance = (volatile uint32_t *)(ib + 0x04D51A04u);
  if (backend_maintenance[0] == 0xd10243ffu && backend_maintenance[1] == 0xf90033feu &&
      backend_maintenance[2] == 0xa90757f6u && backend_maintenance[3] == 0xa9084ff4u) {
    const uint32_t stub[4] = {
      0x58000050u, 0xd61f0200u,
      (uint32_t)((uintptr_t)&nx_backend_maintenance_popup_completed & 0xffffffffu),
      (uint32_t)((uintptr_t)&nx_backend_maintenance_popup_completed >> 32),
    };
    so_patch_code((void *)backend_maintenance, stub, sizeof stub);
  }

  volatile uint32_t *upcoming_maintenance = (volatile uint32_t *)(ib + 0x0474E6E8u);
  if (upcoming_maintenance[0] == 0xf9400c08u && upcoming_maintenance[1] == 0xb4000128u &&
      upcoming_maintenance[2] == 0x39410108u && upcoming_maintenance[3] == 0x340000a8u) {
    const uint32_t stub[4] = {
      0x58000050u, 0xd61f0200u,
      (uint32_t)((uintptr_t)&nx_upcoming_maintenance_hidden & 0xffffffffu),
      (uint32_t)((uintptr_t)&nx_upcoming_maintenance_hidden >> 32),
    };
    so_patch_code((void *)upcoming_maintenance, stub, sizeof stub);
  }

  volatile uint32_t *connection = (volatile uint32_t *)(ib + 0x0474C9B0u);
  if (connection[0] == 0xf81f0ffeu && connection[1] == 0xf9400c08u &&
      connection[2] == 0xb4000108u && connection[3] == 0xf9401908u) {
    const uint32_t stub[4] = {
      0x58000050u, 0xd61f0200u,
      (uint32_t)((uintptr_t)&nx_startup_connection_error_hidden & 0xffffffffu),
      (uint32_t)((uintptr_t)&nx_startup_connection_error_hidden >> 32),
    };
    so_patch_code((void *)connection, stub, sizeof stub);
  }

  struct offline_coroutine_patch {
    uintptr_t offset;
    uintptr_t replacement;
    uint32_t expected[4];
    const char *name;
  };
  const struct offline_coroutine_patch coroutine_patches[] = {
    {0x043ADE94u, (uintptr_t)&nx_core_offline_coroutine_finished,
     {0xa9bd5ffeu, 0xa90157f6u, 0xa9024ff4u, 0xf0032494u}, "GUIOfflinePopup core"},
    {0x04574A0Cu, (uintptr_t)&nx_download_offline_coroutine_finished,
     {0xf81c0ffeu, 0xa9015ff8u, 0xa90257f6u, 0xa9034ff4u}, "DownloadHelper"},
    {0x04B4AA08u, (uintptr_t)&nx_backend_player_offline_coroutine_finished,
     {0xa9bd5ffeu, 0xa90157f6u, 0xa9024ff4u, 0xf002e7d4u}, "BackendPlayer"},
    {0x045FD648u, (uintptr_t)&nx_third_party_offline_coroutine_finished,
     {0xa9bd5ffeu, 0xa90157f6u, 0xa9024ff4u, 0x90031234u}, "ThirdPartyAuth"},
    {0x046FA8A0u, (uintptr_t)&nx_internet_checker_reachable,
     {0xd10143ffu, 0xa9025ffeu, 0xa90357f6u, 0xa9044ff4u}, "InternetChecker retry"},
  };
  for (size_t i = 0; i < sizeof coroutine_patches / sizeof coroutine_patches[0]; ++i) {
    const struct offline_coroutine_patch *patch = &coroutine_patches[i];
    volatile uint32_t *site = (volatile uint32_t *)(ib + patch->offset);
    if (site[0] == patch->expected[0] && site[1] == patch->expected[1] &&
        site[2] == patch->expected[2] && site[3] == patch->expected[3]) {
      const uint32_t stub[4] = {
        0x58000050u, 0xd61f0200u,
        (uint32_t)(patch->replacement & 0xffffffffu),
        (uint32_t)(patch->replacement >> 32),
      };
      so_patch_code((void *)site, stub, sizeof stub);
    }
  }
}
#endif

#if AB2_FORCE_OFFLINE
/* Force OfflineModeConfigurationProvider.IsOfflineModeEnabled (il2cpp+0x04D79E0C) to always
 * return true. Original prologue (objdump-verified):
 *   str x30,[sp,#-16]!   (0xf81f0ffe)
 *   bl  Get()            (0x94000006)
 *   cbz x0, <NRE>        ; ldrb w0,[x0,#16] (return OfflineModeEnabled) ; ret
 * Patch the first two words to `mov w0,#1 ; ret`. This flips the whole offline subsystem ON
 * (DetectOfflineModeStatus/IsOfflineMode + BeaconLoginInitializer's skip path) regardless of the
 * backend/staged config, so the launch scheduler's BeaconAuthentication + GameServerAuthentication
 * groups no longer block on a live session and boot can reach StartScreen. Self-verifying. */
static void nx_force_offline_mode(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04D79E0Cu);
  if (site[0] != 0xf81f0ffeu || site[1] != 0x94000006u) {
    return;
  }
  const uint32_t patch[2] = { 0x52800020u /* mov w0,#1 */, 0xd65f03c0u /* ret */ };
  so_patch_code((void *)site, patch, sizeof patch);
}

#if AB2_ALLOW_OFFLINE_FTUE_BOOT

static void nx_allow_offline_ftue_boot(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04D26F44u);
  if (site[0] != 0xf81e0ffeu || site[1] != 0xa9014ff4u) {
    return;
  }
  const uint32_t patch[2] = {
    0x52800000u, /* mov w0,#0 */
    0xd65f03c0u, /* ret       */
  };
  so_patch_code((void *)site, patch, sizeof patch);
}
#endif

#if AB2_SKIP_OFFLINE_BLUEPRINT_ERASE

static void nx_skip_offline_blueprint_erase(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x089367C0u);
  if (site[0] != 0xf81e0ffeu || site[1] != 0xa9014ff4u) {
    return;
  }
  const uint32_t patch[2] = { 0x52800020u /* mov w0,#1 */, 0xd65f03c0u /* ret */ };
  so_patch_code((void *)site, patch, sizeof patch);
}
#endif

#if AB2_SKIP_ONLINE_SERVER_COMPATIBILITY

static void nx_skip_online_server_compatibility(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04AE72F8u);
  if (*site != 0x34fffae8u) { /* cbz w8,0x04AE7254 */
    return;
  }
  const uint32_t always_complete = 0x17ffffd7u; /* b 0x04AE7254 */
  so_patch_code((void *)site, &always_complete, sizeof always_complete);
}
#endif

/* Neutralize ForceUpdateController: in ForceUpdateCheckAsync's MoveNext (il2cpp+0x04BBCE94) the
 * decision branch `b.eq 0x04BBCF04` (0x54000380, no-update convergence point) is made UNCONDITIONAL
 * (`b 0x04BBCF04` = 0x1400001C) so the version check never constructs/shows the blocking GUIForceUpdate
 * screen for a retired build. Self-verifying. */
static void nx_skip_force_update(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04BBCE94u);
  if (*site != 0x54000380u) {          /* b.eq 0x04BBCF04 */
    return;
  }
  const uint32_t B = 0x1400001Cu;      /* b 0x04BBCF04 (unconditional no-update) */
  so_patch_code((void *)site, &B, sizeof B);
}

/* Beacon init wall -> menu (RE: workflow ab2-beacon-init-fix). Forcing offline mode cleared the
 * online-auth groups, but boot then hit the game's OWN error popup ("something went wrong") from
 * independent throw-gates in the Beacon SDK bring-up. All sites are objdump-verified + self-checking:
 *
 * Gate B (the FATAL popup): Beacon.Session..ctor @il2cpp+0x08D9E40C does `cbz w8,0x08D9E4F8` where
 *   w8 = BeaconSDK._Initialized (static+0x08). When false it constructs+throws InvalidOperationException
 *   "Initialize the SDK with BeaconSDK.Initialize() before setting up Session." The flag is false purely
 *   from INITIALIZER ORDERING (an EarlyInitialization member builds a Session before BeaconSDK.Initialize
 *   runs) -- NOT because DeviceInfo failed (BeaconSDK.InitializeInternal sets the flag at 0x08D389F0
 *   BEFORE it ever touches DeviceInfo at 0x08D38A24). NOP the cbz so the ctor always takes the normal path.
 *
 * Gate A (DeviceInfo TypeInitializationException, defensive): DeviceInfoImpl.GetOSNameAndVersion
 *   @il2cpp+0x08D44380 does `cbz x20,0x08D4451C` (throw) where x20 = SystemInfo.operatingSystem. On the
 *   "{}" JNI path _OsName is null so InitializeCommon fetches operatingSystem; if it is null this NREs.
 *   Retarget the branch to the graceful default-version exit @0x08D445BC instead of the throw. Inert if
 *   operatingSystem is ever non-null on target. */
typedef void *(*il2cpp_domain_get_fn)(void);
typedef void **(*il2cpp_get_assemblies_fn)(void *domain, size_t *count);
typedef void *(*il2cpp_assembly_image_fn)(void *assembly);
typedef const char *(*il2cpp_image_name_fn)(void *image);
typedef void *(*il2cpp_class_from_name_fn)(void *image, const char *namespaze, const char *name);
typedef void *(*il2cpp_object_new_fn)(void *klass);
typedef void (*il2cpp_runtime_object_init_fn)(void *object);

static il2cpp_domain_get_fn p_di_domain_get;
static il2cpp_get_assemblies_fn p_di_get_assemblies;
static il2cpp_assembly_image_fn p_di_assembly_image;
static il2cpp_image_name_fn p_di_image_name;
static il2cpp_class_from_name_fn p_di_class_from_name;
static il2cpp_object_new_fn p_di_object_new;
static il2cpp_runtime_object_init_fn p_di_object_init;
static void *g_device_info_dummy;

/* Beacon ships DeviceInfoImpl_Dummy for platforms without its Android Java SDK.
 * The Android build hard-wires GetImpl() to DeviceInfoImpl_Android, whose constructor
 * is the current TypeInitializationException source. Return the SDK's own Dummy object
 * instead of maintaining an ever-growing imitation of com.rovio.beacon.DeviceInfo. */
static void *nx_device_info_get_dummy(void) {
  if (g_device_info_dummy) return g_device_info_dummy;
  void *domain = p_di_domain_get ? p_di_domain_get() : NULL;
  size_t count = 0;
  void **assemblies = (domain && p_di_get_assemblies) ? p_di_get_assemblies(domain, &count) : NULL;
  void *image = NULL;
  for (size_t i = 0; assemblies && i < count; i++) {
    void *candidate = p_di_assembly_image(assemblies[i]);
    const char *name = candidate ? p_di_image_name(candidate) : NULL;
    if (name && !strcmp(name, "Rovio.Beacon.UnitySDK.dll")) { image = candidate; break; }
  }
  void *klass = image ? p_di_class_from_name(image, "Beacon", "DeviceInfoImpl_Dummy") : NULL;
  void *object = klass ? p_di_object_new(klass) : NULL;
  if (!object) {
    return NULL;
  }
  p_di_object_init(object);
  g_device_info_dummy = object;
  return object;
}

static int nx_patch_device_info_dummy(uintptr_t ib) {
  #define DI_RESOLVE(name) so_try_find_addr_rx(&il2cpp_mod, name)
  p_di_domain_get      = (il2cpp_domain_get_fn)DI_RESOLVE("il2cpp_domain_get");
  p_di_get_assemblies  = (il2cpp_get_assemblies_fn)DI_RESOLVE("il2cpp_domain_get_assemblies");
  p_di_assembly_image  = (il2cpp_assembly_image_fn)DI_RESOLVE("il2cpp_assembly_get_image");
  p_di_image_name      = (il2cpp_image_name_fn)DI_RESOLVE("il2cpp_image_get_name");
  p_di_class_from_name = (il2cpp_class_from_name_fn)DI_RESOLVE("il2cpp_class_from_name");
  p_di_object_new      = (il2cpp_object_new_fn)DI_RESOLVE("il2cpp_object_new");
  p_di_object_init     = (il2cpp_runtime_object_init_fn)DI_RESOLVE("il2cpp_runtime_object_init");
  #undef DI_RESOLVE
  if (!p_di_domain_get || !p_di_get_assemblies || !p_di_assembly_image || !p_di_image_name ||
      !p_di_class_from_name || !p_di_object_new || !p_di_object_init) {
    return 0;
  }

  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x08D3E074u);
  if (site[0] != 0xf81e0ffeu || site[1] != 0xa9014ff4u ||
      site[2] != 0x9000d8d3u || site[3] != 0xb000a394u) {
    return 0;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_device_info_get_dummy & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_device_info_get_dummy >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
  return 1;
}

static void nx_patch_beacon_wall(uintptr_t ib) {
  /* Gate B: NOP Session..ctor's BeaconSDK-initialized check. */
  volatile uint32_t *sB = (volatile uint32_t *)(ib + 0x08D9E40Cu);
  if (*sB == 0x34000768u) {                 /* cbz w8, 0x08D9E4F8 */
    const uint32_t NOP = 0xd503201fu;
    so_patch_code((void *)sB, &NOP, sizeof NOP);
  }
  /* Gate A: retarget the null-OS throw to the graceful default-version exit. */
  volatile uint32_t *sA = (volatile uint32_t *)(ib + 0x08D44380u);
  if (*sA == 0xb4000cf4u) {                 /* cbz x20, 0x08D4451C (throw) */
    const uint32_t RETARGET = 0xb40011f4u;  /* cbz x20, 0x08D445BC (default-version exit) */
    so_patch_code((void *)sA, &RETARGET, sizeof RETARGET);
  }

  /* Gate C (THE ROBUST FIX): make BeaconSDK._Initialized DEFAULT TRUE by patching BeaconSDK's static
   * cctor (0x08D3855C). It ends by zero-initing the flag: `strb wzr,[static_fields,#8]` @0x08D385E8.
   * The instruction just before it, `bl 0x0425FB90` @0x08D385D8, is the GC write-barrier for the
   * preceding Version-ref store -- INERT here because the port runs with the GC disabled. So repurpose
   * it to load 1 into a scratch reg, then store that instead of wzr:
   *   0x08D385D8: bl 0x0425FB90 (0x96D49D6E) -> mov w9,#1     (0x52800029)
   *   0x08D385E8: strb wzr,[x8,#8] (0x3900211F) -> strb w9,[x8,#8] (0x39002109)
   * Now every reader (inlined field OR get_Initialized) sees true the moment BeaconSDK is first touched
   * -- clears "Initialize the SDK first!" / the Session throw at the source, no runtime-API calls (the
   * il2cpp_runtime_class_init route crashed). Patched as a pair; both verified before writing. */
  volatile uint32_t *cB = (volatile uint32_t *)(ib + 0x08D385D8u);
  volatile uint32_t *cF = (volatile uint32_t *)(ib + 0x08D385E8u);
  if (*cB == 0x96d49d6eu && *cF == 0x3900211fu) {
    const uint32_t MOV_W9_1  = 0x52800029u; /* mov w9,#1 (replaces the GC write-barrier bl) */
    const uint32_t STRB_W9   = 0x39002109u; /* strb w9,[x8,#8] */
    so_patch_code((void *)cB, &MOV_W9_1, sizeof MOV_W9_1);
    so_patch_code((void *)cF, &STRB_W9,  sizeof STRB_W9);
  }

  nx_patch_device_info_dummy(ib);
}


static void *g_costume_cache_fallback;
void *g_costume_ctor_return;

__attribute__((used)) void *nx_ensure_costume_cache(void *cache) {
  if (cache) return cache;
  if (g_costume_cache_fallback) return g_costume_cache_fallback;
  void *domain = p_di_domain_get ? p_di_domain_get() : NULL;
  size_t count = 0;
  void **assemblies = (domain && p_di_get_assemblies) ? p_di_get_assemblies(domain, &count) : NULL;
  void *klass = NULL;
  for (size_t i = 0; assemblies && i < count && !klass; i++) {
    void *candidate = p_di_assembly_image(assemblies[i]);
    if (!candidate) continue;
    void *candidate_class = p_di_class_from_name(candidate, "", "CostumeCache");
    if (candidate_class) klass = candidate_class;
  }
  void *object = klass ? p_di_object_new(klass) : NULL;
  if (!object) {
    return NULL;
  }
  p_di_object_init(object);
  g_costume_cache_fallback = object;
  return object;
}

__asm__(
".text\n.align 2\n.global ab2_costume_ctor_trampoline\n.type ab2_costume_ctor_trampoline,%function\n"
"ab2_costume_ctor_trampoline:\n"
"  stp x0, x30, [sp, #-0x20]!\n"
"  str x2, [sp, #0x10]\n"
"  mov x0, x1\n"
"  bl  nx_ensure_costume_cache\n"
"  mov x1, x0\n"
"  ldr x2, [sp, #0x10]\n"
"  ldp x0, x30, [sp], #0x20\n"
"  sub sp, sp, #0x70\n"
"  str x30, [sp, #0x30]\n"
"  stp x24, x23, [sp, #0x40]\n"
"  stp x22, x21, [sp, #0x50]\n"
"  adrp x16, g_costume_ctor_return\n"
"  add  x16, x16, :lo12:g_costume_ctor_return\n"
"  ldr  x16, [x16]\n"
"  br   x16\n");
extern void ab2_costume_ctor_trampoline(void);

static void nx_patch_null_costume_cache(uintptr_t ib) {
  volatile uint32_t *entry = (volatile uint32_t *)(ib + 0x04564FB4u);
  const uint32_t expected[4] = { 0xd101c3ffu, 0xf9001bfeu, 0xa9045ff8u, 0xa90557f6u };
  if (entry[0] != expected[0] || entry[1] != expected[1] ||
      entry[2] != expected[2] || entry[3] != expected[3]) {
    return;
  }
  if (!p_di_domain_get || !p_di_get_assemblies || !p_di_assembly_image ||
      !p_di_class_from_name || !p_di_object_new || !p_di_object_init) {
    return;
  }
  g_costume_ctor_return = (void *)(ib + 0x04564FC4u);
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_costume_ctor_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_costume_ctor_trampoline >> 32),
  };
  so_patch_code((void *)entry, stub, sizeof stub);

  /* CostumeCache.LoadCostumes() is allowed to return null when its local payload
   * is absent/invalid, but this client constructor immediately dereferences the
   * result. x22 still holds the empty List<Costume> allocated a few instructions
   * earlier. Select it only when the loaded x0 is null, store it in
   * m_startingCostumes, and omit the GC write barrier (the port runs with managed
   * GC barriers disabled, as do the existing Beacon patches). The following stock
   * cbz now sees a valid list and the rest of the constructor is untouched. */
  volatile uint32_t *loaded = (volatile uint32_t *)(ib + 0x04565128u);
  const uint32_t loaded_expected[4] = {
    0xaa0003e1u, /* mov x1,x0 */
    0xf90002a0u, /* str x0,[x21] */
    0xaa1503e0u, /* mov x0,x21 */
    0x97f3ea97u, /* bl GC write barrier */
  };
  if (loaded[0] != loaded_expected[0] || loaded[1] != loaded_expected[1] ||
      loaded[2] != loaded_expected[2] || loaded[3] != loaded_expected[3]) {
    return;
  }
  const uint32_t loaded_fix[4] = {
    0xf100001fu, /* cmp x0,#0 */
    0x9a961008u, /* csel x8,x0,x22,ne -- loaded list or prebuilt empty list */
    0xf90002a8u, /* str x8,[x21] */
    0xd503201fu, /* nop -- GC write barrier is inert in this port */
  };
  so_patch_code((void *)loaded, loaded_fix, sizeof loaded_fix);
}

/* ServerEventProvider loads active and disabled Event collections from IObjectStore,
 * then calls List<Event>(IEnumerable<Event>) unconditionally. Missing offline keys
 * return null, so both sites throw ArgumentNullException("collection"). Preserve the
 * collection constructor when data exists; otherwise initialize the already allocated
 * List<Event> with its default constructor. Both calls use the same closed List<Event>
 * RGCTX, so the call site's MethodInfo supplies T to either generic constructor body. */
typedef void (*managed_list_default_fn)(void *list, void *method_info);
typedef void (*managed_list_collection_fn)(void *list, void *collection, void *method_info);
static managed_list_default_fn g_event_list_default;
static managed_list_collection_fn g_event_list_collection;
void *g_server_event_list_ret1;
void *g_server_event_list_ret2;

__attribute__((used)) void nx_init_server_event_list(void *list, void *collection,
                                                     void *method_info) {
  if (collection) {
    g_event_list_collection(list, collection, method_info);
    return;
  }
  g_event_list_default(list, method_info);
}

__asm__(
".text\n.align 2\n.global ab2_server_event_list1_trampoline\n.type ab2_server_event_list1_trampoline,%function\n"
"ab2_server_event_list1_trampoline:\n"
"  stp x30, x19, [sp, #-0x10]!\n"
"  mov x22, x0\n"
"  mov x1, x21\n"
"  ldr x2, [x23]\n"
"  bl  nx_init_server_event_list\n"
"  ldp x30, x19, [sp], #0x10\n"
"  adrp x16, g_server_event_list_ret1\n"
"  add  x16, x16, :lo12:g_server_event_list_ret1\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
".align 2\n.global ab2_server_event_list2_trampoline\n.type ab2_server_event_list2_trampoline,%function\n"
"ab2_server_event_list2_trampoline:\n"
"  stp x30, x19, [sp, #-0x10]!\n"
"  mov x22, x0\n"
"  mov x1, x21\n"
"  ldr x2, [x23]\n"
"  bl  nx_init_server_event_list\n"
"  ldp x30, x19, [sp], #0x10\n"
"  adrp x16, g_server_event_list_ret2\n"
"  add  x16, x16, :lo12:g_server_event_list_ret2\n"
"  ldr  x16, [x16]\n"
"  br   x16\n");
extern void ab2_server_event_list1_trampoline(void);
extern void ab2_server_event_list2_trampoline(void);

static int nx_patch_server_event_null_collections(uintptr_t ib) {
  volatile uint32_t *site1 = (volatile uint32_t *)(ib + 0x0458A5CCu);
  volatile uint32_t *site2 = (volatile uint32_t *)(ib + 0x0458A684u);
  const uint32_t common[3] = { 0xf94002e2u, 0xaa1503e1u, 0xaa0003f6u };
  if (site1[0] != common[0] || site1[1] != common[1] || site1[2] != common[2] ||
      site1[3] != 0x948c391bu || site2[0] != common[0] || site2[1] != common[1] ||
      site2[2] != common[2] || site2[3] != 0x948c38edu) {
    return 0;
  }
  g_event_list_default = (managed_list_default_fn)(ib + 0x06898908u);
  g_event_list_collection = (managed_list_collection_fn)(ib + 0x06898A44u);
  g_server_event_list_ret1 = (void *)(ib + 0x0458A5DCu);
  g_server_event_list_ret2 = (void *)(ib + 0x0458A694u);
  const uint32_t stub1[4] = { 0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_server_event_list1_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_server_event_list1_trampoline >> 32) };
  const uint32_t stub2[4] = { 0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_server_event_list2_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_server_event_list2_trampoline >> 32) };
  so_patch_code((void *)site1, stub1, sizeof stub1);
  so_patch_code((void *)site2, stub2, sizeof stub2);
  return 1;
}

#endif

#if AB2_OFFLINE_GEM_PURCHASES
/* Rovio.Abba.Payment.PaymentController.PurchaseProductInternal
 *   (this, id, onDone, properties, provider)
 * normally enters Beacon/Google Play, then consumes a store voucher before it
 * raises PurchaseSucceeded.  Switch has no Android store.  Resolve the game's
 * own PaymentProduct, accept only ABBAIAPType.GemsPurchase with a positive gem
 * amount, and mirror IL2CPP's delegate ABI:
 *
 *   Action<PaymentProduct,Provider>: method(+0x18)(target(+0x40),product,provider,MethodInfo(+0x28))
 *   Action<PurchaseStatus>:          method(+0x18)(target(+0x40),status,MethodInfo(+0x28))
 *
 * Raising the controller event (rather than editing the wallet here) preserves
 * GUIShopBase/GUIShopScreen's normal reward, history/UI, animation and overlay
 * cleanup path. PurchaseStatus.Success and PurchaseInfo.Succeeded are both 0. */
typedef void *(*nx_payment_get_product_fn)(void *self, void *id, int provider,
                                            uint64_t group_filter_nullable,
                                            bool should_log_error, void *method);
typedef void (*nx_action_status_fn)(void *target, int status, void *method);
typedef void (*nx_action_product_fn)(void *target, void *product, int provider,
                                     void *method);
static nx_payment_get_product_fn p_payment_get_product;
static void *g_fake_payment_service;
void *g_offline_payment_ctor_resume;
void *g_shop_focus_id_resume;
void *g_payment_product_id_getter;
void *g_shop_local_catalogue_resume;

typedef void *(*nx_gui_shop_buy_delegate_fn)(void *target, void *item,
                                               void *method);
typedef void *(*nx_purchase_flow_buy_entry_fn)(void *flow, void *item,
                                                 int entry_point, void *method);
typedef void *(*nx_purchase_flow_buy_lower_fn)(void *flow, void *item,
                                                void *op,
                                                void *purchase_properties,
                                                uint64_t provider_nullable,
                                                void *method);
typedef void (*nx_shop_purchase_teardown_fn)(void *self, void *method);
typedef void *(*nx_func_task_invoke_fn)(void *target, void *method);
static nx_purchase_flow_buy_entry_fn g_purchase_flow_buy_entry;
static nx_purchase_flow_buy_lower_fn g_purchase_flow_buy_lower;
static nx_shop_purchase_teardown_fn g_shop_purchase_teardown;

static void *nx_gui_shop_item_buy(void *self, void *method) {
  (void)method;
  void *delegate = self ? *(void **)((char *)self + 0x130) : NULL;
  void *invoke = delegate ? *(void **)((char *)delegate + 0x18) : NULL;
  void *target = delegate ? *(void **)((char *)delegate + 0x40) : NULL;
  void *delegate_method = delegate ? *(void **)((char *)delegate + 0x28) : NULL;
  return invoke ? ((nx_gui_shop_buy_delegate_fn)invoke)(target, self,
                                                        delegate_method) : NULL;
}

static void *nx_gui_shop_base_buy(void *self, void *item, void *method) {
  (void)method;
  void *entry_config = self ? *(void **)((char *)self + 0x120) : NULL;
  void *flow = self ? *(void **)((char *)self + 0xF0) : NULL;
  int entry_point = entry_config ? *(int *)((char *)entry_config + 0x1C) : -1;
  return (flow && item && entry_config && g_purchase_flow_buy_entry)
      ? g_purchase_flow_buy_entry(flow, item, entry_point, NULL) : NULL;
}

static void *nx_shop_teardown_execute(void *self, void *task_delegate,
                                      void *method) {
  (void)method;
  void *invoke = task_delegate
      ? *(void **)((char *)task_delegate + 0x18) : NULL;
  void *target = task_delegate
      ? *(void **)((char *)task_delegate + 0x40) : NULL;
  void *delegate_method = task_delegate
      ? *(void **)((char *)task_delegate + 0x28) : NULL;
  if (g_shop_purchase_teardown) g_shop_purchase_teardown(self, NULL);
  return invoke ? ((nx_func_task_invoke_fn)invoke)(target, delegate_method) : NULL;
}


static void *nx_offline_direct_regular_purchase(void *flow, void *item,
                                                 void *op,
                                                 uint64_t provider_nullable,
                                                 void *method) {
  (void)method;
  if (flow && item && g_purchase_flow_buy_lower)
    g_purchase_flow_buy_lower(flow, item, op, NULL, provider_nullable, NULL);
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

/* AB2 ships a complete FakePaymentService for local/debug play.  Its Init builds
 * InAppPurchaseProduct entries without Beacon or Google Play, after which the
 * stock PaymentController combines them with the packaged ProductMetaData.  Use
 * that existing implementation instead of fabricating PaymentProduct layouts in
 * native code.  The controller's gem-only purchase hook below remains the final
 * grant gate. */
__attribute__((used)) void *nx_offline_payment_service(void *original_service) {
  if (g_fake_payment_service) return g_fake_payment_service;
  void *domain = p_di_domain_get ? p_di_domain_get() : NULL;
  size_t count = 0;
  void **assemblies = (domain && p_di_get_assemblies)
      ? p_di_get_assemblies(domain, &count) : NULL;
  void *klass = NULL;
  for (size_t i = 0; assemblies && i < count && !klass; i++) {
    void *image = p_di_assembly_image(assemblies[i]);
    if (image)
      klass = p_di_class_from_name(image, "", "FakePaymentService");
  }
  void *service = klass ? p_di_object_new(klass) : NULL;
  if (!service) {
    return original_service;
  }
  p_di_object_init(service);
  g_fake_payment_service = service;
  return service;
}

/* PaymentController ctor: preserve all managed arguments, replace only x2
 * (IPaymentService), replay its verified four-word prologue, then resume. */
__asm__(
".text\n.align 2\n.global ab2_offline_payment_ctor_trampoline\n"
".type ab2_offline_payment_ctor_trampoline,%function\n"
"ab2_offline_payment_ctor_trampoline:\n"
"  stp  x0, x1, [sp, #-0x50]!\n"
"  stp  x3, x4, [sp, #0x10]\n"
"  stp  x5, x6, [sp, #0x20]\n"
"  stp  x7, x30, [sp, #0x30]\n"
"  str  x2, [sp, #0x40]\n"
"  mov  x0, x2\n"
"  bl   nx_offline_payment_service\n"
"  mov  x9, x0\n"
"  ldp  x0, x1, [sp]\n"
"  ldp  x3, x4, [sp, #0x10]\n"
"  ldp  x5, x6, [sp, #0x20]\n"
"  ldp  x7, x30, [sp, #0x30]\n"
"  mov  x2, x9\n"
"  add  sp, sp, #0x50\n"
"  sub  sp, sp, #0x80\n"
"  stp  x29, x30, [sp, #0x20]\n"
"  stp  x28, x27, [sp, #0x30]\n"
"  stp  x26, x25, [sp, #0x40]\n"
"  adrp x16, g_offline_payment_ctor_resume\n"
"  add  x16, x16, :lo12:g_offline_payment_ctor_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
".align 2\n.global ab2_shop_focus_id_trampoline\n"
".type ab2_shop_focus_id_trampoline,%function\n"
"ab2_shop_focus_id_trampoline:\n"
"  cbz  x0, 1f\n"
"  mov  x20, x0\n"
"  mov  x1, xzr\n"
"  adrp x16, g_payment_product_id_getter\n"
"  add  x16, x16, :lo12:g_payment_product_id_getter\n"
"  ldr  x16, [x16]\n"
"  blr  x16\n"
"  b    2f\n"
"1:\n"
"  mov  x20, xzr\n"
"  mov  x0, xzr\n"
"2:\n"
"  adrp x16, g_shop_focus_id_resume\n"
"  add  x16, x16, :lo12:g_shop_focus_id_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
".align 2\n.global ab2_local_shop_catalogue_trampoline\n"
".type ab2_local_shop_catalogue_trampoline,%function\n"
"ab2_local_shop_catalogue_trampoline:\n"
"  mov  x0, x21\n"
"  adrp x16, g_shop_local_catalogue_resume\n"
"  add  x16, x16, :lo12:g_shop_local_catalogue_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n");
extern void ab2_offline_payment_ctor_trampoline(void);
extern void ab2_shop_focus_id_trampoline(void);
extern void ab2_local_shop_catalogue_trampoline(void);

static void nx_invoke_purchase_status(void *action, int status) {
  if (!action) return;
  nx_action_status_fn invoke =
      (nx_action_status_fn)*(void **)((char *)action + 0x18);
  void *target = *(void **)((char *)action + 0x40);
  void *method = *(void **)((char *)action + 0x28);
  if (invoke) invoke(target, status, method);
}

static void nx_offline_purchase_product(void *self, void *id, void *on_done,
                                        void *purchase_properties, int provider,
                                        void *method) {
  (void)purchase_properties;
  (void)method;
  void *product = (self && p_payment_get_product)
      ? p_payment_get_product(self, id, provider, 0, true, NULL)
      : NULL;
  int type = product ? *(int *)((char *)product + 0x10) : 0;
  int gems = product ? *(int *)((char *)product + 0x14) : 0;
  if (!product || type != 1 /* ABBAIAPType.GemsPurchase */ || gems <= 0) {
    nx_invoke_purchase_status(on_done, 1 /* Failed */);
    return;
  }

  void *succeeded = *(void **)((char *)self + 0x18);
  if (succeeded) {
    nx_action_product_fn invoke =
        (nx_action_product_fn)*(void **)((char *)succeeded + 0x18);
    void *target = *(void **)((char *)succeeded + 0x40);
    void *event_method = *(void **)((char *)succeeded + 0x28);
    if (invoke) invoke(target, product, provider, event_method);
  }
  nx_invoke_purchase_status(on_done, 0 /* Success */);
}


static void *nx_offline_buy_shop_item_impl(void *flow, void *item_id,
                                            void *purchase_properties,
                                            int provider, void *method) {
  (void)method;
  void *controller = flow ? *(void **)((char *)flow + 0x18) : NULL;
  if (controller && item_id)
    nx_offline_purchase_product(controller, item_id, NULL,
                                purchase_properties, provider, NULL);
  void *task = p_task_get_completed ? p_task_get_completed(NULL) : NULL;
  return task;
}

static void nx_patch_offline_gem_purchases(uintptr_t ib) {
  /* PaymentController.GetProduct(string,Provider,Nullable<Group>,bool). */
  volatile uint32_t *get_product = (volatile uint32_t *)(ib + 0x04C9DE88u);
  if (get_product[0] != 0xd102c3ffu || get_product[1] != 0xa9057bfdu) {
    return;
  }
  p_payment_get_product = (nx_payment_get_product_fn)get_product;

  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x04C9ED60u);
  static const uint32_t expected[4] = {
    0xa9bc67feu, 0xa9015ff8u, 0xa90257f6u, 0xa9034ff4u
  };
  if (site[0] != expected[0] || site[1] != expected[1] ||
      site[2] != expected[2] || site[3] != expected[3]) {
    return;
  }

  /* GUIShopScreen.OpenGemShopAsync already has a final skipConnectivityChecks
   * argument. Normal entry points pass false, so its async state machine opens
   * the InternetRequired UI before any local product can reach the purchase hook.
   * Force only the local register that captures that argument to true. */
  volatile uint32_t *shop_connectivity =
      (volatile uint32_t *)(ib + 0x0444A5D8u);
  if (*shop_connectivity != 0x2a0503f7u) { /* mov w23,w5 */
    return;
  }

  /* ShopBundles.PurchaseFlow.<BuyShopItemImplAsync>d__39.MoveNext performs a
   * second, purchase-time InternetRequired check after the item button is
   * pressed.  The offline shop can therefore render and remain responsive but
   * still show the connection dialog without ever reaching PaymentController.
   * Replace only this call's boolean result with false; the stock direct path at
   * +0x047BC9A8 then calls PurchaseProduct, whose internal hook below still
   * accepts only GemsPurchase products with a positive Gems value. */
  volatile uint32_t *purchase_connectivity =
      (volatile uint32_t *)(ib + 0x047BC8D0u);
  if (*purchase_connectivity != 0x94138210u) { /* bl PaymentController.InternetRequired */
    return;
  }

  
  volatile uint32_t *payment_services_confirmation =
      (volatile uint32_t *)(ib + 0x047BD06Cu);
  if (*payment_services_confirmation != 0x97fff7f4u) { /* bl IsPaymentServiceActValid */
    return;
  }

  
  volatile uint32_t *fastspring_eligibility =
      (volatile uint32_t *)(ib + 0x08C36C70u);
  if (*fastspring_eligibility != 0x97fffc90u) { /* bl ShopPurchaseHandler.IsFastSpringEligible */
    return;
  }

  
  volatile uint32_t *payment_service_async =
      (volatile uint32_t *)(ib + 0x047BAF20u);
  static const uint32_t expected_payment_service_async[4] = {
    0xd102c3ffu, 0xf9003bfeu, 0xa9085ff8u, 0xa90957f6u
  };
  for (unsigned i = 0; i < 4; i++) {
    if (payment_service_async[i] != expected_payment_service_async[i]) {
      return;
    }
  }

  volatile uint32_t *buy_shop_item_impl =
      (volatile uint32_t *)(ib + 0x047BA4B4u);
  static const uint32_t expected_buy_shop_item_impl[4] = {
    0xd10303ffu, 0xf90043feu, 0xa9095ff8u, 0xa90a57f6u
  };
  for (unsigned i = 0; i < 4; i++) {
    if (buy_shop_item_impl[i] != expected_buy_shop_item_impl[i]) {
      return;
    }
  }

  volatile uint32_t *shop_item_buy =
      (volatile uint32_t *)(ib + 0x0446DF20u);
  static const uint32_t expected_shop_item_buy[4] = {
    0xf9409808u, 0xb40000c8u, 0xaa0003e1u, 0xf9402100u
  };
  volatile uint32_t *shop_base_buy =
      (volatile uint32_t *)(ib + 0x04444850u);
  static const uint32_t expected_shop_base_buy[4] = {
    0xf81f0ffeu, 0xf9409008u, 0xb40000e8u, 0xf9407800u
  };
  volatile uint32_t *shop_regular_dispatch =
      (volatile uint32_t *)(ib + 0x08C360F4u);
  static const uint32_t expected_shop_regular_dispatch[4] = {
    0xa9bf4ffeu, 0xaa0103f3u, 0x94000008u, 0xb40000d3u
  };
  for (unsigned i = 0; i < 4; i++) {
    if (shop_item_buy[i] != expected_shop_item_buy[i] ||
        shop_base_buy[i] != expected_shop_base_buy[i] ||
        shop_regular_dispatch[i] != expected_shop_regular_dispatch[i]) {
      return;
    }
  }

  /* GUIShopScreen.GetProductToFocus uses Enumerable.First on a remotely
   * personalized gem-pack sequence.  That sequence is legitimately empty in
   * offline mode, faulting shop initialization before its presenters and
   * bundle reward list are assigned.  GUIShopBase.GetProductToFocus performs
   * the same selection through the local PaymentController catalogue and ends
   * with FirstOrDefault, while its virtual display-group/IAP-type calls retain
   * the gem-screen filters.  Enter the base implementation directly. */
  volatile uint32_t *product_focus =
      (volatile uint32_t *)(ib + 0x0444916Cu);
  static const uint32_t expected_product_focus[4] = {
    0xf81b0ffeu, 0xa90167fau, 0xa9025ff8u, 0xa90357f6u
  };
  if (product_focus[0] != expected_product_focus[0] ||
      product_focus[1] != expected_product_focus[1] ||
      product_focus[2] != expected_product_focus[2] ||
      product_focus[3] != expected_product_focus[3]) {
    return;
  }

  /* If a future optional initializer still exits early, do not turn its null
   * presenter/reward list into two exceptions per rendered frame.  Both branch
   * targets are the stock no-work exits of their respective Update methods. */
  volatile uint32_t *shop_update_guard =
      (volatile uint32_t *)(ib + 0x04448E70u);
  if (*shop_update_guard != 0xb4000434u) {
    return;
  }
  volatile uint32_t *bundle_update_guard =
      (volatile uint32_t *)(ib + 0x047BFEA8u);
  if (*bundle_update_guard != 0xb40002c0u) {
    return;
  }

  volatile uint32_t *payment_ctor =
      (volatile uint32_t *)(ib + 0x04C9D1C4u);
  static const uint32_t expected_payment_ctor[4] = {
    0xd10203ffu, 0xa9027bfdu, 0xa9036ffcu, 0xa90467fau
  };
  if (payment_ctor[0] != expected_payment_ctor[0] ||
      payment_ctor[1] != expected_payment_ctor[1] ||
      payment_ctor[2] != expected_payment_ctor[2] ||
      payment_ctor[3] != expected_payment_ctor[3]) {
    return;
  }

  /* This installer runs before nx_patch_device_info_dummy, which historically
   * populated the shared reflection exports as a side effect.  Resolve the
   * dependencies here so install order cannot disable every offline-IAP patch. */
  if (!p_di_domain_get)
    p_di_domain_get = (il2cpp_domain_get_fn)
        so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get");
  if (!p_di_get_assemblies)
    p_di_get_assemblies = (il2cpp_get_assemblies_fn)
        so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get_assemblies");
  if (!p_di_assembly_image)
    p_di_assembly_image = (il2cpp_assembly_image_fn)
        so_try_find_addr_rx(&il2cpp_mod, "il2cpp_assembly_get_image");
  if (!p_di_class_from_name)
    p_di_class_from_name = (il2cpp_class_from_name_fn)
        so_try_find_addr_rx(&il2cpp_mod, "il2cpp_class_from_name");
  if (!p_di_object_new)
    p_di_object_new = (il2cpp_object_new_fn)
        so_try_find_addr_rx(&il2cpp_mod, "il2cpp_object_new");
  if (!p_di_object_init)
    p_di_object_init = (il2cpp_runtime_object_init_fn)
        so_try_find_addr_rx(&il2cpp_mod, "il2cpp_runtime_object_init");
  if (!p_di_domain_get || !p_di_get_assemblies || !p_di_assembly_image ||
      !p_di_class_from_name || !p_di_object_new || !p_di_object_init) {
    return;
  }

  /* GUIShopBase.CreateShopItems normally assumes GetProductToFocus cannot
   * return null and calls PaymentProduct.Id immediately.  A local catalogue
   * mismatch must instead pass a null focusedProductId to GetShopProducts, so
   * the stock empty-shop/close path can finish and release the interaction
   * layer.  The trampoline preserves the original non-null ID getter path. */
  volatile uint32_t *focus_id =
      (volatile uint32_t *)(ib + 0x0444454Cu);
  static const uint32_t expected_focus_id[4] = {
    0xb4001800u, 0xaa1f03e1u, 0xaa0003f4u, 0x951aec29u
  };
  if (focus_id[0] != expected_focus_id[0] ||
      focus_id[1] != expected_focus_id[1] ||
      focus_id[2] != expected_focus_id[2] ||
      focus_id[3] != expected_focus_id[3]) {
    return;
  }

  /* CreateShopItems reads the focused PaymentProduct.Id a second time after
   * GetShopProducts and layout construction.  The first null guard intentionally
   * leaves x20/focusedProduct null when the offline focus selector has no match,
   * but stock then executes the leaf Id getter on x20 unconditionally at
   * +0x044447FC, producing a native null load with no managed exception log.
   * x0 is already set from x20 by +0x044447F4.  Replace only the MethodInfo setup
   * at +0x044447F8 with cbz x20,+8, skipping the getter while preserving x0=0. */
  volatile uint32_t *focus_presenter_id =
      (volatile uint32_t *)(ib + 0x044447F8u);
  if (*focus_presenter_id != 0xaa1f03e1u) { /* mov x1,xzr */
    return;
  }

  /* FakePaymentService.Init creates its 69 packaged products with
   * Billing.Provider.Default (0).  The no-argument shop lookup asks the Android
   * BillingProvider for the current provider instead; on Switch that selects a
   * different inner-dictionary key.  PaymentController.GetProducts does not
   * preserve its newly allocated empty list when that key is absent and returns
   * null.  Pass an explicit Nullable<Provider> { HasValue=true, Value=Default }
   * for this one lookup.  IL2CPP packs HasValue in the low byte of x3 and Value
   * in its high word, so the required value is simply x3=1. */
  volatile uint32_t *shop_default_provider =
      (volatile uint32_t *)(ib + 0x04449760u);
  if (shop_default_provider[0] != 0xaa1f03e3u ||
      shop_default_provider[1] != 0xaa1f03e4u) {
    return;
  }

  
  volatile uint32_t *bundle_default_provider =
      (volatile uint32_t *)(ib + 0x04AE5218u);
  if (*bundle_default_provider != 0xaa1f03e3u) { /* mov x3,xzr */
    return;
  }

  
  volatile uint32_t *bundle_items_default_provider =
      (volatile uint32_t *)(ib + 0x047BEFD8u);
  if (*bundle_items_default_provider != 0xaa1f03e3u) { /* mov x3,xzr */
    return;
  }

  /* BundlePaymentProductProvider.Get constructs its bundle source from live
   * offer configuration before it combines that source with PaymentController
   * products.  The offline provider returns a null IEnumerable and its internal
   * Enumerable.Where faults before the later personalization bypass can run.
   * Bundle results are ignored by that bypass, so preserve the real local gem
   * list already held in x21 as a harmless placeholder and skip only the
   * provider Get + ToList calls.  Execution resumes at +0x044497A0, where stock
   * code copies x0 to x22 and continues its remaining shop setup. */
  volatile uint32_t *shop_bundle_provider =
      (volatile uint32_t *)(ib + 0x04449788u);
  static const uint32_t expected_shop_bundle_provider[6] = {
    0xaa0803e0u, 0xaa1f03e1u, 0xaa1f03e2u,
    0x941a6e4fu, 0xf94002c1u, 0x9469e44du
  };
  for (unsigned i = 0; i < 6; i++) {
    if (shop_bundle_provider[i] != expected_shop_bundle_provider[i]) {
      return;
    }
  }

  /* GUIShopScreen.GetShopProducts next passes the local PaymentController
   * catalogue through ShopProductFilter.GetShopProducts and FilterProducts.
   * Both are personalization pipelines backed by remote configuration.  With
   * no backend, one of their IEnumerable sources is null; Enumerable.Where
   * throws ArgumentNullException and the async shop opener never releases its
   * modal interaction layer.  x21 is the non-remote catalogue returned by
   * PaymentController.GetProducts for this screen's virtual display group.
   * Return it directly and leave all subsequent presenter/purchase handling
   * stock.  The four replaced instructions are the personalization call and
   * the following FilterProducts(this, result) call setup/invocation. */
  volatile uint32_t *shop_catalogue_filter =
      (volatile uint32_t *)(ib + 0x04449818u);
  static const uint32_t expected_shop_catalogue_filter[4] = {
    0x9471f38fu, 0xaa0003e1u, 0xaa1403e0u, 0x97ffff78u
  };
  if (shop_catalogue_filter[0] != expected_shop_catalogue_filter[0] ||
      shop_catalogue_filter[1] != expected_shop_catalogue_filter[1] ||
      shop_catalogue_filter[2] != expected_shop_catalogue_filter[2] ||
      shop_catalogue_filter[3] != expected_shop_catalogue_filter[3]) {
    return;
  }

  /* GUIShopItem.SpawnItemArt conditionally calls
   * BundleShopItem.SetCustomGemLayout for products whose prefab enables
   * CustomGemLayout.  The Android assets selected by the offline Default-provider
   * catalogue carry partially serialized optional BundleShopItem components:
   * custom-layout items miss m_priceLabel, while non-custom items can miss another
   * collection later consumed by Enumerable.Where in BundleShopItem.Init.  Both
   * faults leave the shop opener's modal interaction layer held.  BundleShopItem is
   * unusable for the packaged ProductDisplayGroup.Shop (1) catalogue: base
   * GUIShopItem art, price, input, and purchase setup is already complete, while its
   * 12 products mix direct gems, a subscription with null RewardsData, and bundles
   * whose optional prefab collections do not match this Android asset revision.
   * Retarget the CustomGemLayout=false
   * branch to the common bundle-object check, then use the former cosmetic-call and
   * redundant null-check slots to skip the trailing bundle-specific Init for every
   * general-shop product. Products from other display groups still call stock Init. */
  volatile uint32_t *custom_gem_layout_branch =
      (volatile uint32_t *)(ib + 0x0446E3D0u);
  volatile uint32_t *custom_gem_layout_call =
      (volatile uint32_t *)(ib + 0x0446E3ECu);
  volatile uint32_t *bundle_item_init_guard =
      (volatile uint32_t *)(ib + 0x0446E3F0u);
  if (*custom_gem_layout_branch != 0x34000148u ||
      *custom_gem_layout_call != 0x97ffe6aeu ||
      bundle_item_init_guard[0] != 0xf94007e0u ||
      bundle_item_init_guard[1] != 0xb4000140u) {
    return;
  }

  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_offline_purchase_product & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_offline_purchase_product >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
  const uint32_t force_skip_connectivity = 0x52800037u; /* mov w23,#1 */
  so_patch_code((void *)shop_connectivity, &force_skip_connectivity,
                sizeof force_skip_connectivity);
  const uint32_t force_purchase_offline_path = 0x2a1f03e0u; /* mov w0,wzr */
  so_patch_code((void *)purchase_connectivity, &force_purchase_offline_path,
                sizeof force_purchase_offline_path);
  const uint32_t skip_payment_services_confirmation = 0x2a1f03e0u; /* mov w0,wzr */
  so_patch_code((void *)payment_services_confirmation,
                &skip_payment_services_confirmation,
                sizeof skip_payment_services_confirmation);
  const uint32_t force_regular_shop_purchase = 0x2a1f03e0u; /* mov w0,wzr */
  so_patch_code((void *)fastspring_eligibility,
                 &force_regular_shop_purchase,
                 sizeof force_regular_shop_purchase);
  g_purchase_flow_buy_lower =
      (nx_purchase_flow_buy_lower_fn)(ib + 0x047B9F84u);
  const uint32_t direct_regular_purchase_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_offline_direct_regular_purchase & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_offline_direct_regular_purchase >> 32),
  };
  so_patch_code((void *)payment_service_async, direct_regular_purchase_stub,
                sizeof direct_regular_purchase_stub);
  const uint32_t direct_buy_impl_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_offline_buy_shop_item_impl & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_offline_buy_shop_item_impl >> 32),
  };
  so_patch_code((void *)buy_shop_item_impl, direct_buy_impl_stub,
                sizeof direct_buy_impl_stub);
  g_purchase_flow_buy_entry =
      (nx_purchase_flow_buy_entry_fn)(ib + 0x047BACA0u);
  g_shop_purchase_teardown =
      (nx_shop_purchase_teardown_fn)(ib + 0x08C3611Cu);
  const uint32_t shop_item_route_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_gui_shop_item_buy & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_gui_shop_item_buy >> 32),
  };
  so_patch_code((void *)shop_item_buy, shop_item_route_stub,
                sizeof shop_item_route_stub);
  const uint32_t shop_base_route_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_gui_shop_base_buy & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_gui_shop_base_buy >> 32),
  };
  so_patch_code((void *)shop_base_buy, shop_base_route_stub,
                sizeof shop_base_route_stub);
  const uint32_t regular_dispatch_route_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_shop_teardown_execute & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_shop_teardown_execute >> 32),
  };
  so_patch_code((void *)shop_regular_dispatch,
                regular_dispatch_route_stub,
                sizeof regular_dispatch_route_stub);
  const uintptr_t base_product_focus = ib + 0x04444328u;
  const uint32_t product_focus_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)(base_product_focus & 0xffffffffu),
    (uint32_t)(base_product_focus >> 32),
  };
  so_patch_code((void *)product_focus, product_focus_stub,
                sizeof product_focus_stub);
  const uint32_t skip_null_shop_presenter = 0xb4000354u;
  so_patch_code((void *)shop_update_guard, &skip_null_shop_presenter,
                sizeof skip_null_shop_presenter);
  const uint32_t skip_null_bundle_rewards = 0xb40000c0u;
  so_patch_code((void *)bundle_update_guard, &skip_null_bundle_rewards,
                sizeof skip_null_bundle_rewards);
  g_offline_payment_ctor_resume = (void *)(ib + 0x04C9D1D4u);
  const uint32_t payment_ctor_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_offline_payment_ctor_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_offline_payment_ctor_trampoline >> 32),
  };
  so_patch_code((void *)payment_ctor, payment_ctor_stub,
                sizeof payment_ctor_stub);
  g_payment_product_id_getter = (void *)(ib + 0x08AFF5FCu);
  g_shop_focus_id_resume = (void *)(ib + 0x0444455Cu);
  const uint32_t focus_id_stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_shop_focus_id_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_shop_focus_id_trampoline >> 32),
  };
  so_patch_code((void *)focus_id, focus_id_stub, sizeof focus_id_stub);
  const uint32_t skip_null_presenter_focus_id = 0xb4000054u;
  so_patch_code((void *)focus_presenter_id, &skip_null_presenter_focus_id,
                sizeof skip_null_presenter_focus_id);
  const uint32_t force_default_shop_provider = 0xd2800023u; /* mov x3,#1 */
  so_patch_code((void *)shop_default_provider, &force_default_shop_provider,
                sizeof force_default_shop_provider);
  const uint32_t force_default_bundle_provider = 0xd2800023u; /* mov x3,#1 */
  so_patch_code((void *)bundle_default_provider,
                &force_default_bundle_provider,
                sizeof force_default_bundle_provider);
  const uint32_t force_default_bundle_items_provider = 0xd2800023u; /* mov x3,#1 */
  so_patch_code((void *)bundle_items_default_provider,
                &force_default_bundle_items_provider,
                sizeof force_default_bundle_items_provider);
  const uint32_t skip_offline_bundle_provider[2] = {
    0xaa1503e0u, /* mov x0,x21: valid local list placeholder */
    0x14000005u, /* b +0x14 -> il2cpp+0x044497A0 */
  };
  so_patch_code((void *)shop_bundle_provider, skip_offline_bundle_provider,
                sizeof skip_offline_bundle_provider);
  g_shop_local_catalogue_resume = (void *)(ib + 0x04449828u);
  const uint32_t use_local_shop_catalogue[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_local_shop_catalogue_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_local_shop_catalogue_trampoline >> 32),
  };
  so_patch_code((void *)shop_catalogue_filter, use_local_shop_catalogue,
                 sizeof use_local_shop_catalogue);
  const uint32_t route_all_items_through_gem_guard = 0x340000a8u;
  so_patch_code((void *)custom_gem_layout_branch,
                &route_all_items_through_gem_guard,
                sizeof route_all_items_through_gem_guard);
  const uint32_t skip_gem_bundle_init[3] = {
    0xb9403268u, /* ldr w8,[x19,#0x30]: PaymentProduct.DisplayGroup */
    0x7100051fu, /* cmp w8,#1: ProductDisplayGroup.Shop */
    0x54000080u, /* b.eq +0x10 -> SpawnItemArt epilogue @+0x0446E404 */
  };
  so_patch_code((void *)custom_gem_layout_call,
                skip_gem_bundle_init,
                sizeof skip_gem_bundle_init);
}
#endif

#if AB2_GUARD_NULL_FILE_WRITES
/* System.IO.File.WriteAllBytes(string,byte[]) @il2cpp+0x086F050C validates its
 * arguments and tail-calls InternalWriteAllBytes @+0x086F0608. The offline chest
 * flow can still receive null from an absent Android serializer; the resulting
 * ArgumentNullException faults its task before the black interaction layer is
 * released. Preserve every valid write byte-for-byte, but replace only a null
 * payload with a real zero-length managed System.Byte[] before entering the stock
 * internal writer. The public method's third MethodInfo argument is already what
 * its original tail branch forwards unchanged. */
typedef void *(*nx_il2cpp_get_corlib_fn)(void);
typedef void *(*nx_il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef void *(*nx_il2cpp_array_new_fn)(void *, uintptr_t);
typedef void (*nx_internal_write_all_bytes_fn)(void *, void *, void *);

static nx_il2cpp_get_corlib_fn       p_write_get_corlib;
static nx_il2cpp_class_from_name_fn  p_write_class_from_name;
static nx_il2cpp_array_new_fn        p_write_array_new;
static nx_internal_write_all_bytes_fn p_internal_write_all_bytes;
static void *g_write_byte_class;

static void *nx_new_empty_managed_bytes(void) {
  if (!g_write_byte_class && p_write_get_corlib && p_write_class_from_name) {
    void *corlib = p_write_get_corlib();
    g_write_byte_class = corlib
        ? p_write_class_from_name(corlib, "System", "Byte") : NULL;
  }
  return (g_write_byte_class && p_write_array_new)
      ? p_write_array_new(g_write_byte_class, 0) : NULL;
}

static void nx_write_all_bytes_guard(void *path, void *bytes, void *method) {
  if (!path || *(uint32_t *)((char *)path + 0x10) == 0) {
    return;
  }
  if (!bytes) {
    bytes = nx_new_empty_managed_bytes();
    if (!bytes) {
      return;
    }
  }
  p_internal_write_all_bytes(path, bytes, method);
}

static void nx_patch_null_file_writes(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x086F050Cu);
  static const uint32_t expected[4] = {
    0xf81e0ffeu, 0xa9014ff4u, 0xb40000e0u, 0xb9401008u
  };
  if (site[0] != expected[0] || site[1] != expected[1] ||
      site[2] != expected[2] || site[3] != expected[3]) {
    return;
  }
  p_write_get_corlib = (nx_il2cpp_get_corlib_fn)
      so_try_find_addr_rx(&il2cpp_mod, "il2cpp_get_corlib");
  p_write_class_from_name = (nx_il2cpp_class_from_name_fn)
      so_try_find_addr_rx(&il2cpp_mod, "il2cpp_class_from_name");
  p_write_array_new = (nx_il2cpp_array_new_fn)
      so_try_find_addr_rx(&il2cpp_mod, "il2cpp_array_new");
  p_internal_write_all_bytes =
      (nx_internal_write_all_bytes_fn)(ib + 0x086F0608u);
  if (!p_write_get_corlib || !p_write_class_from_name || !p_write_array_new) {
    return;
  }
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&nx_write_all_bytes_guard & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_write_all_bytes_guard >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

#if AB2_JNI_UNKNOWN_TYPE_AS_OBJECT
/* UnityEngine.AndroidJNIHelper.GetSignature(object) @il2cpp+0x09736DB0 has a
 * verified terminal for argument types it cannot represent in JNI.  The block at
 * +0x097378D0 constructs and throws; a completed guide/event run reaches it while
 * DefaultGameRunChangeHandler is cleaning up, so its Task faults before LeaveRun.
 *
 * Redirect only that terminal to +0x097375B0, the method's stock null-object path.
 * That path returns Unity's existing "Ljava/lang/Object;" string.  Known primitive,
 * array, String, AndroidJavaObject, and AndroidJavaProxy signatures are untouched.
 * This matches fake JNI's intentionally collapsed Java-reference model.
 *
 * Chuck's bird-unlock tutorial exposes a second input to the same fallback logic:
 * reflection returns a null intermediate type at +0x09737768.  Stock branches from
 * there to the common null-reference throw at +0x09737A28, faulting EventRunState's
 * completion task after RunCompleted.  Retarget only that cbz to the same verified
 * object-signature return. */
static void nx_patch_jni_unknown_type_signature(uintptr_t ib) {
  volatile uint32_t *unknown_site =
      (volatile uint32_t *)(ib + 0x097378D0u);
  volatile uint32_t *null_type_site =
      (volatile uint32_t *)(ib + 0x09737768u);
  const uint32_t expected_unknown = 0xd0004dc0u; /* adrp x0,#0xa0ef000 */
  const uint32_t expected_null_type = 0xb4001615u; /* cbz x21,+0x2c0 -> throw */
  const uint32_t to_object = 0x17ffff38u; /* b +0x097375B0 */
  const uint32_t null_type_to_object =
      0xb4fff255u; /* cbz x21,-0x1b8 -> +0x097375B0 */
  if (*unknown_site != expected_unknown) {
    return;
  }
  if (*null_type_site != expected_null_type) {
    return;
  }
  so_patch_code((void *)unknown_site, &to_object, sizeof to_object);
  so_patch_code((void *)null_type_site, &null_type_to_object,
                sizeof null_type_to_object);
}
#endif

#if AB2_LOCAL_KEYSTORE_PASSTHROUGH
/* Rovio.Frameworks.Security.Cryptography.AndroidAesKeyStore has no portable
 * implementation in this Android player: both methods call the absent Java
 * AesKeyStorePlugin and receive null.  Their IL2CPP ABI is
 *   x0=this, x1=keyAlias, x2=input byte[], x3=MethodInfo -> x0=result byte[].
 * Return x2 unchanged for both directions.  This is intentionally limited to
 * the two verified platform-keystore methods; serializers, streams, and other
 * cryptography remain stock. */
static void nx_patch_local_keystore(uintptr_t ib) {
  static const uintptr_t offsets[2] = { 0x08E1BC94u, 0x08E1BE20u };
  static const uint32_t expected[2] = {
    0xf81d0ffeu, /* str x30,[sp,#-0x30]! */
    0xa90157f6u, /* stp x22,x21,[sp,#0x10] */
  };
  static const uint32_t passthrough[2] = {
    0xaa0203e0u, /* mov x0,x2 */
    0xd65f03c0u, /* ret       */
  };
  for (unsigned i = 0; i < 2; i++) {
    volatile uint32_t *site = (volatile uint32_t *)(ib + offsets[i]);
    if (site[0] != expected[0] || site[1] != expected[1]) {
      continue;
    }
    so_patch_code((void *)site, passthrough, sizeof passthrough);
  }
}
#endif

#if AB2_NORMALIZE_OFFLINE_QUEST_STATE
/* OfflineQuestStateHandler.LoadStateFromFile treats the repository's boolean
 * success as sufficient and calls OfflineQuestArgumentConverter.Convert even
 * when its out OfflineQuestSaveState is null.  This occurs for the zero-byte
 * season-pass-quest-state.txt produced by the unavailable Android persistence
 * backend and faults the chest reward task before its modal overlay is released.
 *
 * Hook the four-instruction success tail.  Valid states still run the stock
 * converter.  A null state, or one whose OfflineQuestActions field (+0x10) is
 * null, is replaced with a normal managed OfflineQuestSaveState constructed by
 * the game's own .ctor (which creates the required List<OfflineQuestAction>).
 * The false/read-failed path and all non-quest persistence remain untouched. */
typedef void (*nx_offline_quest_managed_fn)(void *object, void *method);
static nx_offline_quest_managed_fn p_offline_quest_ctor;
static nx_offline_quest_managed_fn p_offline_quest_convert;
static void *g_offline_quest_state_class;
void *g_offline_quest_load_resume;

static void *nx_find_offline_quest_state_class(void) {
  if (g_offline_quest_state_class) return g_offline_quest_state_class;
  void *domain = p_di_domain_get ? p_di_domain_get() : NULL;
  size_t count = 0;
  void **assemblies = (domain && p_di_get_assemblies)
      ? p_di_get_assemblies(domain, &count) : NULL;
  for (size_t i = 0; assemblies && i < count; i++) {
    void *image = p_di_assembly_image(assemblies[i]);
    if (!image) continue;
    void *klass = p_di_class_from_name(
        image, "Rovio.Abba.SeasonPass.Client.Offline", "OfflineQuestSaveState");
    if (klass) {
      g_offline_quest_state_class = klass;
      return klass;
    }
  }
  return NULL;
}

__attribute__((used)) void *nx_offline_quest_state_normalize(void *state) {
  /* A decoded object is only usable when its sole required collection exists. */
  if (state && *(void **)((char *)state + 0x10)) {
    p_offline_quest_convert(state, NULL);
    return state;
  }

  void *klass = nx_find_offline_quest_state_class();
  void *replacement = (klass && p_di_object_new) ? p_di_object_new(klass) : NULL;
  if (!replacement) {
    return state;
  }
  p_offline_quest_ctor(replacement, NULL);
  return replacement;
}

__asm__(
".text\n.align 2\n.global ab2_offline_quest_load_trampoline\n"
".type ab2_offline_quest_load_trampoline,%function\n"
"ab2_offline_quest_load_trampoline:\n"
"  ldr  x0, [sp, #0x18]\n"
"  bl   nx_offline_quest_state_normalize\n"
"  mov  x19, x0\n"
"  adrp x16, g_offline_quest_load_resume\n"
"  add  x16, x16, :lo12:g_offline_quest_load_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n");
extern void ab2_offline_quest_load_trampoline(void);

static void nx_patch_offline_quest_state(uintptr_t ib) {
  volatile uint32_t *site = (volatile uint32_t *)(ib + 0x08C1EF68u);
  static const uint32_t expected[4] = {
    0xf9400fe0u, /* ldr x0,[sp,#0x18] */
    0x97fffaacu, /* bl OfflineQuestArgumentConverter.Convert */
    0xf9400ff3u, /* ldr x19,[sp,#0x18] */
    0x14000007u, /* b cleanup/return @+0x08C1EF90 */
  };
  if (site[0] != expected[0] || site[1] != expected[1] ||
      site[2] != expected[2] || site[3] != expected[3]) {
    return;
  }
  if (!p_di_domain_get || !p_di_get_assemblies || !p_di_assembly_image ||
      !p_di_class_from_name || !p_di_object_new) {
    return;
  }
  p_offline_quest_convert = (nx_offline_quest_managed_fn)(ib + 0x08C1DA1Cu);
  p_offline_quest_ctor = (nx_offline_quest_managed_fn)(ib + 0x08C1DCC8u);
  g_offline_quest_load_resume = (void *)(ib + 0x08C1EF90u);
  const uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ab2_offline_quest_load_trampoline & 0xffffffffu),
    (uint32_t)((uintptr_t)&ab2_offline_quest_load_trampoline >> 32),
  };
  so_patch_code((void *)site, stub, sizeof stub);
}
#endif

extern void nx_net_init(void);   /* bsd_bridge.c: nifm status + socketInitialize (enlarged CDN buffers) */

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
#if AB2_BOOST_INITIAL_LOADING
  nx_boot_boost_start();
#endif
  nx_net_init();

  /* chdir into DATA_ROOT: hbloader leaves cwd elsewhere and the engine reads many
   * files through relative paths ("assets/bin/..."). AB2 ships flat data.unity3d
   * bundles (base data.unity3d + UnityDataAssetPack datapack.unity3d) -- no OBB. */
  chdir(DATA_ROOT);

  /* A plain overlay of the base APK and UnityDataAssetPack leaves two UnityFS
   * archives. Android presents them as one asset namespace; prepare the equivalent
   * merged view once on the SD before Unity opens its data file. */
  if (unityfs_prepare(DATA_ROOT) != 0)
    fatal_error("Could not prepare Unity data. See unityfs_merge.log.");

  /* Load config.txt (language, optional rotation) from the game folder; create it
   * with defaults on first run so the options are discoverable/editable on the SD.
   * AB2 is LANDSCAPE -> portrait defaults 0 (identity, native panel orientation). */
  {
    const char *cfg = DATA_ROOT "/" CONFIG_NAME;
    int rc = read_config(cfg);
    if (rc != 0) write_config(cfg);   /* -1 missing or 1 stale keys -> (re)write */
    if (config.portrait != 1 && config.portrait != 2) config.portrait = 0;  /* 0 = landscape */
  }

  check_syscalls();
  {
    extern char *fake_heap_start, *fake_heap_end;
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
  }

  /* Arm the stack-region overcommit arena (aliases heap pages into the stack region
   * for Unity's big PROT_NONE reservations). Any failure falls back to heap-backed. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    /* Pinned OC reservations can consume at most the window size, so never expose
     * more virtual window than the physical source pool can back. */
    const size_t oc_want = OC_WINDOW_BYTES < OC_POOL_BYTES
                             ? OC_WINDOW_BYTES : OC_POOL_BYTES;
    void *win = oc_find_stack_window(oc_want, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      
      const size_t pool_bytes = winsz < OC_POOL_BYTES ? winsz : OC_POOL_BYTES;
      pool = memalign(0x1000, pool_bytes);
      if (pool) oc_arena_init(win, winsz, pool, pool_bytes);
    }
  }

  /* Landscape game: report a landscape surface (W>H) matching the native panel, so
   * the engine renders directly with correct aspect and no compositor rotation.
   * Handheld 1280x720 / docked 1920x1080 (see android_native_unity.c). */
  if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1920; screen_height = 1080; }
  else                                                         { screen_width = 1280; screen_height = 720;  }

  SDL_SetMainReady();
  SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);

  check_data();

  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);

#if AB2_BOOST_INITIAL_LOADING
  nx_boot_boost_install_scene_hook(g_il2cpp_base);
#endif
#if AB2_DIRECT_LOCALE
  nx_patch_direct_locale(g_il2cpp_base);
#endif
#if AB2_SKIP_COSTUME_PRELOAD
  nx_skip_costume_preload(g_il2cpp_base);
#if AB2_SKIP_CRASH_TRACKER_BOOTSTRAP
  nx_skip_crash_tracker_bootstrap(g_il2cpp_base);
#endif
#if AB2_SKIP_FACEBOOK_INIT
  nx_skip_facebook_init(g_il2cpp_base);
  nx_skip_clean_obsolete_assets(g_il2cpp_base);
#endif
#endif
#if AB2_BYPASS_PLAY_INTEGRITY
  nx_bypass_play_integrity(g_il2cpp_base);
#endif
#if AB2_SYNC_LOCAL_ADDRESSABLE_BUNDLES
  nx_patch_sync_local_addressables(g_il2cpp_base);
#endif
#if AB2_SYNC_BUNDLED_ADDRESSABLE_ASSETS
  nx_patch_sync_bundled_addressable_assets(g_il2cpp_base);
#endif
#if AB2_DISABLE_ANDROID_HEADPHONE_BRIDGE
  nx_disable_android_headphone_bridge(g_il2cpp_base);
#endif
#if AB2_DISABLE_ANDROID_PLATFORM_DETECTION
  nx_disable_android_platform_detection(g_il2cpp_base);
#endif
#if AB2_SKIP_BEACON_LOGIN && AB2_SKIP_COSTUME_PRELOAD
  nx_skip_beacon_login(g_il2cpp_base);
#endif
#if AB2_SKIP_ANDROID_NOTIFICATIONS && AB2_SKIP_COSTUME_PRELOAD
  nx_skip_android_notifications(g_il2cpp_base);
#endif
#if AB2_SUPPRESS_OFFLINE_NETWORK_POPUPS && AB2_SKIP_COSTUME_PRELOAD
  nx_suppress_offline_network_popups(g_il2cpp_base);
#endif
#if AB2_SKIP_GPGS_ACTIVATE
  nx_skip_gpgs_activate(g_il2cpp_base);
#endif
#if AB2_FAKE_ANDROID_PERMISSIONS
  nx_patch_android_permissions(g_il2cpp_base);
#endif
#if AB2_OFFLINE_GEM_PURCHASES
  nx_patch_offline_gem_purchases(g_il2cpp_base);
#endif
#if AB2_GUARD_NULL_FILE_WRITES
  nx_patch_null_file_writes(g_il2cpp_base);
#endif
#if AB2_JNI_UNKNOWN_TYPE_AS_OBJECT
  nx_patch_jni_unknown_type_signature(g_il2cpp_base);
#endif
#if AB2_LOCAL_KEYSTORE_PASSTHROUGH
  nx_patch_local_keystore(g_il2cpp_base);
#endif
#if AB2_FORCE_OFFLINE
  nx_force_offline_mode(g_il2cpp_base);   /* skip the BeaconAuth/GameServerAuth online gate -> menu */
#if AB2_ALLOW_OFFLINE_FTUE_BOOT
  nx_allow_offline_ftue_boot(g_il2cpp_base); /* let bundled first-run tutorial start while offline */
#endif
#if AB2_SKIP_OFFLINE_BLUEPRINT_ERASE
  nx_skip_offline_blueprint_erase(g_il2cpp_base); /* keep local repo; let API client factory complete */
#endif
#if AB2_SKIP_ONLINE_SERVER_COMPATIBILITY
  nx_skip_online_server_compatibility(g_il2cpp_base); /* keep API client setup; skip live version GET */
#endif
  nx_skip_force_update(g_il2cpp_base);    /* protect against a retired-version blocking screen */
  nx_patch_beacon_wall(g_il2cpp_base);    /* seed Beacon init + guard Session order and null-OS */
#if AB2_NORMALIZE_OFFLINE_QUEST_STATE
  nx_patch_offline_quest_state(g_il2cpp_base); /* zero-byte quest save -> stock initialized state */
#endif
  nx_patch_null_costume_cache(g_il2cpp_base); /* restore missing local cache before DI graph build */
  nx_patch_server_event_null_collections(g_il2cpp_base); /* absent offline event caches -> empty lists */
#endif

  /* Region-granularity shrink in-memory before any allocator/init runs (4GB support).
   * No-op until AB2_REGION_PATCH is enabled + P[] derived (see nx_patch_unity_regions). */
  nx_patch_unity_regions((uintptr_t)unity_mod.load_virtbase);

  /* Main thread runs init_array + the engine lifecycle; give it its own stable
   * bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);

  /* fake JNI + our environment, then HID */
  jni_init();
#if AB2_FMOD_DIRECT_PUMP
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    fmod_audio_bind((void *)(ub + OFF_fmodGetInfo),
                    (void *)(ub + OFF_fmodProcess),
                    (void *)(ub + OFF_fmodProcessMicData));
  }
#endif
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();

  /* resolve UnityPlayer natives (load_virtbase + recovered offsets) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) first: it caches the JavaVM into
   * libunity's JNI manager, without which initJni's ScopedJNI gets a NULL env. */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    Unity_JNI_OnLoad(fake_vm, NULL);
  }

  /* Register the JavaVM with il2cpp so managed AndroidJNI/AndroidJavaObject calls
   * resolve. We can't call libil2cpp's JNI_OnLoad (a GOT-indirect PLT call our loader
   * mis-binds -> Instruction Abort), so replicate its two essential global stores
   * directly: cache the VM and store the JNI-handler fn-ptr. */
  if (OFF_IL2CPP_JAVAVM_CACHE && OFF_IL2CPP_JNI_HANDLER_PTR && OFF_IL2CPP_JNI_HANDLER_BODY) {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    *(void **)(b + OFF_IL2CPP_JAVAVM_CACHE)    = fake_vm;                              /* JavaVM cache */
    *(void **)(b + OFF_IL2CPP_JNI_HANDLER_PTR) = (void *)(b + OFF_IL2CPP_JNI_HANDLER_BODY); /* JNI handler ptr */
  }

  /* Unity 6 initJni uses (env, thiz, Context, jint, jstring). */
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj, 0, NULL);
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);

  /* The player loop only advances Update when the app is RESUMED and FOCUSED;
   * issue those transitions before the loop or it renders a frozen frame forever. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
#if AB2_FMOD_DIRECT_PUMP
  fmod_audio_start();
#endif

  /* Install the GC-disable and clock fix before the first nativeRender: Unity allocates
   * on its first frame, triggering a Boehm GC that stops the world with POSIX signals
   * Switch never delivers, so nativeRender would never return. */
  {
    /* Disable the Boehm GC entirely (its signal-based stop-the-world is broken on
     * Switch); set_mode(DISABLED) also turns off the incremental collector. */
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (il2cpp_gc_set_mode) il2cpp_gc_set_mode(1);
    if (il2cpp_gc_disable)  il2cpp_gc_disable();

    /* Install the native engine-clock fix (see nx_install_time_fix). */
    nx_install_time_fix();

    /* Frame-2 black-screen fix: nativeRender's UnityChoreographer ctor blocks in a
     * spin-loop waiting for a vsync-ready flag a Java Choreographer would set. Let the
     * object construct but skip the WAIT by rewriting the loop-exit test. Self-verifying
     * (patch only if the original word matches). Offsets: see unity_entrypoints.h. */
    if (OFF_Choreographer_wait_site) {
      uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
      volatile uint32_t *site = (volatile uint32_t *)(ub + OFF_Choreographer_wait_site);
      if (*site == AB2_CHOREO_FROM) {
        uint32_t to = AB2_CHOREO_TO;
        so_patch_code((void *)site, &to, sizeof to);
      }
    }
  }
  /* keep the engine clock advancing while nativeRender blocks -- see nx_clock_thread. */
  nx_start_clock_thread();

  while (appletMainLoop() && !jni_quit_requested) {
    g_frame_count++;
    android_native_update_mode();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
  }

#if AB2_BOOST_INITIAL_LOADING
  nx_boot_boost_stop();
#endif

  /* apply()/commit() normally flush every preference transaction. Also flush at
   * orderly applet shutdown so HOME/Close Software cannot strand a dirty final
   * transaction before Unity tears its runtime down. */
  unity_jni_flush();

#if AB2_FMOD_DIRECT_PUMP
  fmod_audio_shutdown();
#endif
  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
