/* android_media_stubs.h -- graceful Android NDK media fallbacks for Switch.
 *
 * Unity 6 links its Android video/camera backend unconditionally.  The Switch
 * port has no MediaCodec/ImageReader implementation, but leaving those imports
 * unresolved poisons their PLT slots and turns an optional video into a native
 * instruction abort.  These symbols make the backend report "unsupported"
 * instead, so Unity can take its normal no-decoder path.
 */

#ifndef __ANDROID_MEDIA_STUBS_H__
#define __ANDROID_MEDIA_STUBS_H__

#include <stddef.h>
#include "so_util.h"

extern DynLibFunction android_media_functions[];
extern size_t android_media_numfunctions;

#endif
