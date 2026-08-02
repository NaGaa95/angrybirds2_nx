/* Angry Birds 2 26.4.3 / Unity 6000.3.6f1 wrapper configuration. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/* Memory layout. Unity's 12-bit region table needs a wider granule in a
 * 39-bit hbloader process than it does in Sphaira's 36-bit process. */
#define MEMORY_MB          768
#define MMAP_ARENA_ALIGN_36 ((size_t)32 * 1024 * 1024)
#define MMAP_ARENA_ALIGN_39 ((size_t)128 * 1024 * 1024)
#define MMAP_ARENA_RESERVE ((size_t)640 * 1024 * 1024)
#define OC_WINDOW_BYTES    ((size_t)768 * 1024 * 1024)
#define OC_POOL_BYTES      ((size_t)384 * 1024 * 1024)

/* Android identity exposed by the compatibility layer. */
#define AB2_PACKAGE      "com.rovio.baba"
#define AB2_VERSION_CODE 50297008
#define AB2_VERSION_NAME "26.4.3"

#define CONFIG_NAME "config.txt"
#define GAME_HOME   "sdmc:/switch/angrybird2_nx"

/* Release profile. */
#define AB2_BOOST_INITIAL_LOADING 1

/* Platform and offline compatibility patches. */
#define AB2_DIRECT_LOCALE 1
#define AB2_SKIP_COSTUME_PRELOAD 1
#define AB2_SKIP_CRASH_TRACKER_BOOTSTRAP 1
#define AB2_BYPASS_PLAY_INTEGRITY 1
#define AB2_SKIP_OFFLINE_COSTUME_VALIDATION 1
#define AB2_GUARD_NULL_FILE_WRITES 1
#define AB2_JNI_UNKNOWN_TYPE_AS_OBJECT 1
#define AB2_LOCAL_KEYSTORE_PASSTHROUGH 1
#define AB2_NORMALIZE_OFFLINE_QUEST_STATE 1
#define AB2_SYNC_LOCAL_ADDRESSABLE_BUNDLES 1
#define AB2_SYNC_BUNDLED_ADDRESSABLE_ASSETS 1
#define AB2_SKIP_FACEBOOK_INIT 1
#define AB2_DISABLE_ANDROID_HEADPHONE_BRIDGE 1
#define AB2_DISABLE_ANDROID_PLATFORM_DETECTION 1
#define AB2_SKIP_BEACON_LOGIN 1
#define AB2_SKIP_ANDROID_NOTIFICATIONS 1
#define AB2_SKIP_GPGS_ACTIVATE 1
#define AB2_FAKE_ANDROID_PERMISSIONS 1
#define AB2_FORCE_OFFLINE 1
#define AB2_OFFLINE_GEM_PURCHASES 1
#define AB2_ALLOW_OFFLINE_FTUE_BOOT 1
#define AB2_SUPPRESS_OFFLINE_NETWORK_POPUPS 1
#define AB2_SKIP_OFFLINE_BLUEPRINT_ERASE 1
#define AB2_SKIP_ONLINE_SERVER_COMPATIBILITY 1

extern int screen_width;
extern int screen_height;

#define LANG_AUTO 0
#define LANG_JA   1
#define LANG_EN   2

typedef struct {
  int screen_width;
  int screen_height;
  int language;
  int portrait;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
