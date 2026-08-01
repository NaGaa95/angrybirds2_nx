/* Runtime preparation for split UnityFS data copied from the Android APKs. */

#ifndef __UNITYFS_MERGE_H__
#define __UNITYFS_MERGE_H__

/* Build or validate the on-SD merged cache when datapack.unity3d is present.
 * Returns zero when the data is ready (including an already-merged layout). */
int unityfs_prepare(const char *data_root);

/* Redirect requests for the base data.unity3d to the prepared cache. */
const char *unityfs_redirect_path(const char *path);

#endif
