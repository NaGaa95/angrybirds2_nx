/* Angry Birds 2 26.4.3 / Unity 6000.3.6f1 arm64 offsets. */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- libunity.so : UnityPlayer native methods (jnitables.py, reloc scan) ----
 * Drive-critical (initJni..nativeRender) -- the port cannot run without these.
 *
 * Unity 6 SIGNATURES (confirmed from the 6000.3.6f1 arm64 reference libunity,
 * UnityPlayer table = 35 entries; these arg counts are version-stable so they hold
 * for AB2's shipped lib too, even though the ADDRESSES differ -- AB2 ships a
 * module-stripped 17MB libunity, not the stock 20.6MB editor build):
 *   initJni            (Landroid/content/Context;ILjava/lang/String;)V  -> 5-arg native!
 *                      = initJni(env, thiz, context, jint, jstring)  (was 2-arg on 2021.3)
 *   nativeInjectEvent  (Landroid/view/InputEvent;I)Z  -> 4-arg (InputEvent,int)
 *   nativeRecreateGfxState (ILandroid/view/Surface;)V, nativeOrientationChanged (II)V,
 *   nativeFocusChanged (Z)V, nativeRender/Pause/Done ()Z, nativeResume/SendSurface/
 *   ApplicationUnload ()V -- all as before. New in U6 (unused for boot): nativeView
 *   PaddingChanged, nativeOnApplyWindowInsets, nativeGetMainWindowIndex,
 *   nativeConfigurationChanged, nativeMemoryUsageChanged, nativeUnityPlayerSetRunning. */
/* Recovered from AB2's shipped arm64 libunity.so (jnitables.py; UnityPlayer table
 * @0x15f47e0, 35 entries). */
#define OFF_JNI_OnLoad                    0x851a00 /* export; (JavaVM*,reserved)->jint  */
#define OFF_initJni                       0x850754 /* (env,thiz,Context,jint,jstring) 5-arg */
#define OFF_nativeRecreateGfxState        0x850a30 /* (env,thiz,int,Surface)             */
#define OFF_nativeSendSurfaceChangedEvent 0x850a98 /* (env,thiz)                         */
#define OFF_nativeRender                  0x850cd0 /* (env,thiz)->Z  per-frame; false=stop */
#define OFF_nativeInjectEvent             0x850d30 /* (env,thiz,InputEvent,int)->Z  4-arg */
#define OFF_nativePause                   0x8508b8 /* (env,thiz)->Z                      */
#define OFF_nativeResume                  0x85091c /* (env,thiz)->V                      */
#define OFF_nativeFocusChanged            0x8509cc /* (env,thiz,Z)                       */
#define OFF_nativeDone                    0x8507d8 /* (env,thiz)->Z  shutdown            */
#define OFF_nativeApplicationUnload       0x85097c /* (env,thiz)                         */
#define OFF_nativeLowMemory               0x850864 /* = nativeMemoryUsageChanged(I) on U6 */
#define OFF_nativeOrientationChanged      0x851844 /* (env,thiz,int,int)                 */
/* secondary / usually unused for a first boot */
#define OFF_nativeUnitySendMessage        0x851340
#define OFF_nativeMuteMasterAudio         0x85156c
#define OFF_nativeIsAutorotationOn        0x85150c
#define OFF_nativeSetLaunchURL            0x8515d0

/* ---- libunity.so : FMOD natives (audio; jni_fake.c -> opensles.c) -----------
 * Recovered from AB2's FMODAudioDevice JNI table @0x1603310 (3 entries).  The
 * Android Java playback thread does not exist on Switch, so AB2_FMOD_DIRECT_PUMP
 * calls fmodProcess with a direct ByteBuffer and queues its S16 PCM through SDL.
 * This does not require forcing FMOD_OUTPUTTYPE_OPENSL in the native manager. */
#define AB2_FMOD_DIRECT_PUMP   1
#define OFF_fmodGetInfo                   0x1220f8c /* (I)I                        */
#define OFF_fmodProcess                   0x1221054 /* (Ljava/nio/ByteBuffer;)I    */
#define OFF_fmodProcessMicData            0x12210e0 /* (Ljava/nio/ByteBuffer;I)I   */

/* ---- libunity.so : TimeManager::Update (the black-screen clock fix) ----------
 * Located by the 1e-5 deltaTime-clamp fingerprint (scan.py/func.py), cross-checked
 * against the Unity 6000.3.6f1 Release symbol TimeManager::Update. VERIFY the field
 * layout on U6 -- nx_time_update_hook in main.c assumes: frameCount u64 @0xc8, aux
 * u32 @0xd0, pause byte @0xf8, startupRef double @0xe8. If U6 moved these, update
 * both the offsets here AND the field accesses in main.c.
 *
 * AB2 (Unity 6): recovered by byte-matching the 9-insn prologue in the game libunity
 * (unique). The FIELD OFFSETS MOVED vs 2021.3/2022.3 (were 0xc8/0xd0/0xf8/0xe8):
 *   frameCount u64 @0x160, aux u32 @0x168, pause u8 @0x1a8.
 * Unity 6's TimeManager stores time as RationalTime (Update(double) converts newTime
 * via bl DoubleToRationalTime) -- there is NO simple double startupRef, so the clock
 * hook passes newTime = wall-seconds directly (delta is computed from the engine's own
 * previous newTime, so it advances correctly without a startupRef). */
#define OFF_TimeManager_Update_entry      0x692cc0 /* prologue: [+0x160]++;[+0x168]++;if([+0x1a8])ret */
#define OFF_TimeManager_Update_body       0x692ce4 /* frameless; re-reads x0, expects newTime in d0   */
#define OFF_TM_frameCount_u64             0x160  /* U6 field offsets (confirmed by disasm) */
#define OFF_TM_aux_u32                    0x168
#define OFF_TM_pause_u8                   0x1a8

/* ---- libunity.so : vsync / frame-presented counter --------------------------
 * The clock thread bumps this so libunity's per-frame VSYNC-present wait advances
 * (no real Choreographer here). Global in libunity's .bss. */
/* AB2/Unity 6 per-frame VSYNC-PRESENT counter wait (the frame-3 hang, Hitman GO v5
 * pattern): nativeRender blocks in `while (*(i64*)(libunity+0x1677120) < target)
 * pthread_cond_wait` (game libunity+0x83c494..0x83c4ac; mutex @0x16770c4, cond @0x16770ec)
 * waiting for the frame-present counter the (absent) frame source bumps each vsync. The
 * clock thread bumps this counter ~120Hz so the wait's predicate is met (and our 16ms
 * cond-wait cap re-checks it), letting the player loop advance. */
#define OFF_vsync_counter                 0x1677120 /* i64 frame-present counter */

/* ---- libunity.so : Choreographer frame-2 wait-skip --------------------------
 * The player loop constructs a Choreographer vsync helper and BLOCKS in a cond_wait
 * until a Java Choreographer signals ready. Patch the loop-exit test to skip the
 * WAIT (construct the object, don't wait). Find via the "Choreographer" string xref.
 * OFF_Choreographer_wait_site = the cbz/cbnz to rewrite; _FROM/_TO = the words. */
/* AB2/Unity 6: nativeRender pumps a native NdkLooper/Swappy message queue and blocks in
 * `while(queue@this+0xe8 empty) pthread_cond_wait` (game libunity+0x8430fc..0x843118) --
 * waiting for frame messages the (unrun) Java UnityPlayer/Choreographer would post, which
 * never come, so it hangs after frame 2. The loop-back `b 0x8430fc` @0x843118 is rewritten
 * to `b 0x84311c` (exit) so after ONE ~16ms capped cond_wait it returns instead of looping;
 * the caller ignores the return + the exit path only unlocks the mutex, so nativeRender
 * proceeds and our main loop free-runs frames. */
#define OFF_Choreographer_wait_site       0x843118
#define AB2_CHOREO_FROM                   0x17fffff9u  /* b 0x8430fc (loop back)  */
#define AB2_CHOREO_TO                     0x14000001u  /* b 0x84311c (exit wait)  */

/* ---- libil2cpp.so : JavaVM globals (replicated from JNI_OnLoad) --------------
 * We do NOT call libil2cpp JNI_OnLoad (GOT-indirect PLT risk); instead replicate
 * its two essential global stores: cache the JavaVM + store the JNI-handler fnptr.
 * Recovered from the matching libil2cpp JNI_OnLoad implementation. */
/* Recovered from AB2 libil2cpp JNI_OnLoad @0x04288E74 + registrar @0x04288230:
 *   0x04288EA0  str x19,[x8,#0x8b0]  x8=adrp 0xa865000  -> VM cache  @0xa8658b0
 *   0x04288234  str x0, [x8,#0x890]  x8=adrp 0xa865000  -> handler ptr @0xa865890
 *   0x04288E9C  adr x0, 0x04288EB8                       -> handler body 0x04288EB8 */
#define OFF_IL2CPP_JAVAVM_CACHE           0xa8658b0 /* *(il2cpp+X) = fake_vm               */
#define OFF_IL2CPP_JNI_HANDLER_PTR        0xa865890 /* *(il2cpp+Y) = il2cpp + HANDLER_BODY */
#define OFF_IL2CPP_JNI_HANDLER_BODY       0x04288EB8 /* handler fn RVA                      */

/* ---- libil2cpp.so : initial-loading scene hook -----------------------------
 * Rovio.Abba.Commons.Unity.SceneExtensions.IsBootScene/IsMainScene identify the
 * scene delivered to SceneManager.Internal_SceneLoaded. */
#define OFF_IL2CPP_SCENE_IS_BOOT          0x08951B44
#define OFF_IL2CPP_SCENE_IS_MAIN          0x08951BA0
#define AB2_SCENE_PREDICATE_ENTRY_0       0xf81e0ffeu
#define AB2_SCENE_PREDICATE_ENTRY_1       0xa9014ff4u
#define OFF_IL2CPP_INTERNAL_SCENE_LOADED  0x097FDD2C
#define AB2_SCENE_LOADED_ENTRY_0          0xf81d0ffeu
#define AB2_SCENE_LOADED_ENTRY_1          0xa90157f6u
#define AB2_SCENE_LOADED_ENTRY_2          0xa9024ff4u
#define AB2_SCENE_LOADED_ENTRY_3          0xf0008316u

/* ---- libunity.so : memory-region granularity patch (retail-4GB support) -----
 * AB2 STATUS: ENABLED. All 19 Unity 6 allocator sites are derived and
 * completeness-verified; main.c atomically transforms stock 256MB operands to the
 * 32MB budget selected by AB2_REGION_GRANULE_MB. */
#define AB2_REGION_PATCH 1

/* ---- JNI native signatures: ret (*)(JNIEnv*, jobject thiz, args...) ----------
 * We keep fn_inject 4-arg (env,thiz,ev,deviceId); if AB2's nativeInjectEvent is
 * 3-arg the extra register is simply unread (harmless on AArch64: caller-cleaned). */
typedef void     (*fn_initJni)(void*,void*,void*,int32_t,void*); /* U6: (env,thiz,ctx,jint,jstring) */
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what the Java UnityPlayer does; performed in main.c):
 *   JNI_OnLoad(fake_vm, NULL); initJni(env,thiz,ctx);
 *   nativeRecreateGfxState(env,thiz,0,surface); nativeSendSurfaceChangedEvent(env,thiz);
 *   nativeResume(env,thiz); nativeFocusChanged(env,thiz,1);
 *   for (;;) { feed_hid(nativeInjectEvent,...); if(!nativeRender(env,thiz)) break; }
 *   nativeApplicationUnload(env,thiz); nativeDone(env,thiz);
 * =========================================================================== */

#endif /* UNITY_ENTRYPOINTS_H */
