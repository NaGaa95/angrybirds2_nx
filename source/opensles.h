/* Minimal OpenSL ES shim for Unity's Android audio backend.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * The Android libraries link against OpenSL ES, which has no devkitPro
 * equivalent. We implement the required object model
 * (Engine -> OutputMix / AudioPlayer with an Android simple buffer queue, plus
 * the Play and Volume interfaces) and back the buffer-queue players with a
 * single SDL2 audio device that software-mixes them.
 */

#ifndef __OPENSLES_H__
#define __OPENSLES_H__

#include <stdint.h>

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired);

void opensles_shutdown(void);

// FMOD (Unity native audio) output sink -- a dedicated queue-mode SDL device
// driven by the native fmodProcess pump in jni_fake.c.
int      audio_fmod_open(int rate, int channels);      // returns actual device rate (0=fail)
int      audio_fmod_write(const void *pcm, int bytes, uint32_t *queued_bytes);
uint32_t audio_fmod_queued(void);                      // currently queued bytes

// interface-id tokens referenced by libff4.so relocations. Each is a unique
// non-NULL sentinel (self-addressed); the engine passes the value to
// GetInterface and we compare pointers.
extern void *SL_IID_3DCOMMIT, *SL_IID_3DDOPPLER, *SL_IID_3DGROUPING, *SL_IID_3DLOCATION;
extern void *SL_IID_3DMACROSCOPIC, *SL_IID_3DSOURCE, *SL_IID_ANDROIDCONFIGURATION;
extern void *SL_IID_ANDROIDEFFECT, *SL_IID_ANDROIDEFFECTCAPABILITIES, *SL_IID_ANDROIDEFFECTSEND;
extern void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_AUDIODECODERCAPABILITIES, *SL_IID_AUDIOENCODER;
extern void *SL_IID_AUDIOENCODERCAPABILITIES, *SL_IID_AUDIOIODEVICECAPABILITIES, *SL_IID_BASSBOOST;
extern void *SL_IID_BUFFERQUEUE, *SL_IID_DEVICEVOLUME, *SL_IID_DYNAMICINTERFACEMANAGEMENT;
extern void *SL_IID_DYNAMICSOURCE, *SL_IID_EFFECTSEND, *SL_IID_ENGINE, *SL_IID_ENGINECAPABILITIES;
extern void *SL_IID_ENVIRONMENTALREVERB, *SL_IID_EQUALIZER, *SL_IID_LED, *SL_IID_METADATAEXTRACTION;
extern void *SL_IID_METADATATRAVERSAL, *SL_IID_MIDIMESSAGE, *SL_IID_MIDIMUTESOLO, *SL_IID_MIDITEMPO;
extern void *SL_IID_MIDITIME, *SL_IID_MUTESOLO, *SL_IID_NULL, *SL_IID_OBJECT, *SL_IID_OUTPUTMIX;
extern void *SL_IID_PITCH, *SL_IID_PLAY, *SL_IID_PLAYBACKRATE, *SL_IID_PREFETCHSTATUS;
extern void *SL_IID_PRESETREVERB, *SL_IID_RATEPITCH, *SL_IID_RECORD, *SL_IID_SEEK, *SL_IID_THREADSYNC;
extern void *SL_IID_VIBRA, *SL_IID_VIRTUALIZER, *SL_IID_VISUALIZATION, *SL_IID_VOLUME;

#endif
