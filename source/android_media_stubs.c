/* android_media_stubs.c -- unsupported-but-safe NDK media surface.
 *
 * This is deliberately not a decoder.  It mirrors the ABI of the 67 Android
 * media/image/hardware-buffer symbols imported by AB2's Unity 6 libunity.so,
 * returns NULL/AMEDIA_ERROR_UNSUPPORTED, and clears every output parameter.
 * That is preferable to so_resolve()'s missing-import poison: a cut-scene may
 * be skipped or rejected, but it cannot jump into a relocation offset.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "android_media_stubs.h"
#include "util.h"

typedef int32_t media_status_t;
enum {
  AMEDIA_OK = 0,
  AMEDIA_ERROR_UNSUPPORTED = -10002,
  AMEDIA_ERROR_INVALID_OBJECT = -10003,
  AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1,
};

typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaFormat AMediaFormat;
typedef struct AMediaExtractor AMediaExtractor;
typedef struct AMediaDataSource AMediaDataSource;
typedef struct AImage AImage;
typedef struct AImageReader AImageReader;
typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
} AMediaCodecBufferInfo;

/* Layout from android/hardware_buffer.h. */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t format;
  uint64_t usage;
  uint32_t stride;
  uint32_t rfu0;
  uint64_t rfu1;
} AHardwareBuffer_Desc;

static void media_unavailable(void) {
}

/* NdkMediaFormat exports these as pointer variables, not string arrays.  The
 * dynamic resolver must therefore point at a pointer-sized object (&key_*). */
static const char *key_duration       = "durationUs";
static const char *key_width          = "width";
static const char *key_height         = "height";
static const char *key_mime           = "mime";
static const char *key_channel_count  = "channel-count";
static const char *key_sample_rate    = "sample-rate";
static const char *key_color_format   = "color-format";
static const char *key_color_standard = "color-standard";
static const char *key_color_range    = "color-range";
static const char *key_frame_rate     = "frame-rate";
static const char *key_rotation       = "rotation-degrees";
static const char *key_language       = "language";
static const char *key_encoder_delay  = "encoder-delay";
static const char *key_stride         = "stride";
static const char *key_slice_height   = "slice-height";

/* AImage / AImageReader / AHardwareBuffer. */
static media_status_t image_get_hardware_buffer(const AImage *image, AHardwareBuffer **out) {
  (void)image; if (out) *out = NULL; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static void hardware_buffer_release(AHardwareBuffer *buffer) { (void)buffer; }
static void image_delete(AImage *image) { (void)image; }
static media_status_t image_reader_set_buffer_removed_listener(AImageReader *reader, const void *listener) {
  (void)reader; (void)listener; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t image_reader_new_with_usage(int32_t width, int32_t height, int32_t format,
                                                   uint64_t usage, int32_t max_images,
                                                   AImageReader **reader) {
  (void)width; (void)height; (void)format; (void)usage; (void)max_images;
  if (reader) *reader = NULL;
  media_unavailable();
  return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t image_reader_get_window(AImageReader *reader, void **window) {
  (void)reader; if (window) *window = NULL; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t image_reader_set_image_listener(AImageReader *reader, const void *listener) {
  (void)reader; (void)listener; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static void hardware_buffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *desc) {
  (void)buffer; if (desc) memset(desc, 0, sizeof(*desc)); media_unavailable();
}
static media_status_t image_reader_delete(AImageReader *reader) {
  (void)reader; return AMEDIA_OK;
}
static media_status_t image_reader_acquire_latest_image(AImageReader *reader, AImage **image) {
  (void)reader; if (image) *image = NULL; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t image_get_width(const AImage *image, int32_t *width) {
  (void)image; if (width) *width = 0; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static void image_delete_async(AImage *image, int release_fence_fd) {
  (void)image; (void)release_fence_fd;
}
static media_status_t image_get_timestamp(const AImage *image, int64_t *timestamp_ns) {
  (void)image; if (timestamp_ns) *timestamp_ns = 0; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}

/* AMediaFormat. */
static bool media_format_get_int32(AMediaFormat *format, const char *name, int32_t *out) {
  (void)format; (void)name; if (out) *out = 0; media_unavailable(); return false;
}
static bool media_format_get_int64(AMediaFormat *format, const char *name, int64_t *out) {
  (void)format; (void)name; if (out) *out = 0; media_unavailable(); return false;
}
static bool media_format_get_float(AMediaFormat *format, const char *name, float *out) {
  (void)format; (void)name; if (out) *out = 0.0f; media_unavailable(); return false;
}
static bool media_format_get_string(AMediaFormat *format, const char *name, const char **out) {
  (void)format; (void)name; if (out) *out = NULL; media_unavailable(); return false;
}
static media_status_t media_format_delete(AMediaFormat *format) {
  (void)format; return AMEDIA_OK;
}
static void media_format_set_int32(AMediaFormat *format, const char *name, int32_t value) {
  (void)format; (void)name; (void)value; media_unavailable();
}

/* AMediaExtractor / AMediaDataSource. */
static AMediaExtractor *media_extractor_new(void) { media_unavailable(); return NULL; }
static media_status_t media_extractor_delete(AMediaExtractor *extractor) {
  (void)extractor; return AMEDIA_OK;
}
static media_status_t media_extractor_set_data_source_fd(AMediaExtractor *extractor, int fd,
                                                          int64_t offset, int64_t length) {
  (void)extractor; (void)fd; (void)offset; (void)length;
  media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t media_extractor_set_data_source(AMediaExtractor *extractor, const char *location) {
  (void)extractor; (void)location; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t media_extractor_set_data_source_custom(AMediaExtractor *extractor,
                                                              AMediaDataSource *source) {
  (void)extractor; (void)source; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static size_t media_extractor_get_track_count(AMediaExtractor *extractor) {
  (void)extractor; media_unavailable(); return 0;
}
static AMediaFormat *media_extractor_get_track_format(AMediaExtractor *extractor, size_t index) {
  (void)extractor; (void)index; media_unavailable(); return NULL;
}
static media_status_t media_extractor_select_track(AMediaExtractor *extractor, size_t index) {
  (void)extractor; (void)index; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static ssize_t media_extractor_read_sample_data(AMediaExtractor *extractor, uint8_t *buffer,
                                                 size_t capacity) {
  (void)extractor; (void)buffer; (void)capacity; media_unavailable(); return -1;
}
static bool media_extractor_advance(AMediaExtractor *extractor) {
  (void)extractor; media_unavailable(); return false;
}
static int64_t media_extractor_get_sample_time(AMediaExtractor *extractor) {
  (void)extractor; media_unavailable(); return -1;
}
static ssize_t media_extractor_get_sample_track_index(AMediaExtractor *extractor) {
  (void)extractor; media_unavailable(); return -1;
}
static media_status_t media_extractor_seek_to(AMediaExtractor *extractor, int64_t seek_pos_us,
                                               int seek_mode) {
  (void)extractor; (void)seek_pos_us; (void)seek_mode;
  media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}

static AMediaDataSource *media_data_source_new(void) { media_unavailable(); return NULL; }
static void media_data_source_delete(AMediaDataSource *source) { (void)source; }
static void media_data_source_set_userdata(AMediaDataSource *source, void *userdata) {
  (void)source; (void)userdata;
}
static void media_data_source_set_read_at(AMediaDataSource *source, void *read_at) {
  (void)source; (void)read_at;
}
static void media_data_source_set_get_size(AMediaDataSource *source, void *get_size) {
  (void)source; (void)get_size;
}
static void media_data_source_set_close(AMediaDataSource *source, void *close_cb) {
  (void)source; (void)close_cb;
}

/* AMediaCodec. */
static AMediaCodec *media_codec_create_decoder_by_type(const char *mime) {
  (void)mime; media_unavailable(); return NULL;
}
static media_status_t media_codec_configure(AMediaCodec *codec, const AMediaFormat *format,
                                             void *surface, void *crypto, uint32_t flags) {
  (void)codec; (void)format; (void)surface; (void)crypto; (void)flags;
  media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t media_codec_start(AMediaCodec *codec) {
  (void)codec; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t media_codec_stop(AMediaCodec *codec) { (void)codec; return AMEDIA_OK; }
static media_status_t media_codec_flush(AMediaCodec *codec) {
  (void)codec; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}
static media_status_t media_codec_delete(AMediaCodec *codec) { (void)codec; return AMEDIA_OK; }
static ssize_t media_codec_dequeue_input_buffer(AMediaCodec *codec, int64_t timeout_us) {
  (void)codec; (void)timeout_us; media_unavailable(); return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}
static uint8_t *media_codec_get_input_buffer(AMediaCodec *codec, size_t index, size_t *out_size) {
  (void)codec; (void)index; if (out_size) *out_size = 0; media_unavailable(); return NULL;
}
static media_status_t media_codec_queue_input_buffer(AMediaCodec *codec, size_t index,
                                                      int64_t offset, size_t size,
                                                      uint64_t presentation_time_us,
                                                      uint32_t flags) {
  (void)codec; (void)index; (void)offset; (void)size; (void)presentation_time_us; (void)flags;
  media_unavailable(); return AMEDIA_ERROR_INVALID_OBJECT;
}
static ssize_t media_codec_dequeue_output_buffer(AMediaCodec *codec, AMediaCodecBufferInfo *info,
                                                  int64_t timeout_us) {
  (void)codec; (void)timeout_us; if (info) memset(info, 0, sizeof(*info));
  media_unavailable(); return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}
static uint8_t *media_codec_get_output_buffer(AMediaCodec *codec, size_t index, size_t *out_size) {
  (void)codec; (void)index; if (out_size) *out_size = 0; media_unavailable(); return NULL;
}
static AMediaFormat *media_codec_get_output_format(AMediaCodec *codec) {
  (void)codec; media_unavailable(); return NULL;
}
static media_status_t media_codec_release_output_buffer(AMediaCodec *codec, size_t index, bool render) {
  (void)codec; (void)index; (void)render; return AMEDIA_ERROR_INVALID_OBJECT;
}
static media_status_t media_codec_set_output_surface(AMediaCodec *codec, void *surface) {
  (void)codec; (void)surface; media_unavailable(); return AMEDIA_ERROR_UNSUPPORTED;
}

#define MEDIA_FN(android_name, local_name) { android_name, (uintptr_t)&local_name }
#define MEDIA_KEY(android_name, local_name) { android_name, (uintptr_t)&local_name }

DynLibFunction android_media_functions[] = {
  MEDIA_KEY("AMEDIAFORMAT_KEY_DURATION",       key_duration),
  MEDIA_KEY("AMEDIAFORMAT_KEY_WIDTH",          key_width),
  MEDIA_KEY("AMEDIAFORMAT_KEY_HEIGHT",         key_height),
  MEDIA_KEY("AMEDIAFORMAT_KEY_MIME",           key_mime),
  MEDIA_KEY("AMEDIAFORMAT_KEY_CHANNEL_COUNT",  key_channel_count),
  MEDIA_KEY("AMEDIAFORMAT_KEY_SAMPLE_RATE",    key_sample_rate),
  MEDIA_KEY("AMEDIAFORMAT_KEY_COLOR_FORMAT",   key_color_format),
  MEDIA_KEY("AMEDIAFORMAT_KEY_COLOR_STANDARD", key_color_standard),
  MEDIA_KEY("AMEDIAFORMAT_KEY_COLOR_RANGE",    key_color_range),
  MEDIA_KEY("AMEDIAFORMAT_KEY_FRAME_RATE",     key_frame_rate),
  MEDIA_KEY("AMEDIAFORMAT_KEY_ROTATION",       key_rotation),
  MEDIA_KEY("AMEDIAFORMAT_KEY_LANGUAGE",       key_language),
  MEDIA_KEY("AMEDIAFORMAT_KEY_ENCODER_DELAY",  key_encoder_delay),
  MEDIA_KEY("AMEDIAFORMAT_KEY_STRIDE",         key_stride),
  MEDIA_KEY("AMEDIAFORMAT_KEY_SLICE_HEIGHT",   key_slice_height),

  MEDIA_FN("AImage_getHardwareBuffer", image_get_hardware_buffer),
  MEDIA_FN("AHardwareBuffer_release", hardware_buffer_release),
  MEDIA_FN("AImage_delete", image_delete),
  MEDIA_FN("AImageReader_setBufferRemovedListener", image_reader_set_buffer_removed_listener),
  MEDIA_FN("AImageReader_newWithUsage", image_reader_new_with_usage),
  MEDIA_FN("AImageReader_getWindow", image_reader_get_window),
  MEDIA_FN("AImageReader_setImageListener", image_reader_set_image_listener),
  MEDIA_FN("AHardwareBuffer_describe", hardware_buffer_describe),
  MEDIA_FN("AImageReader_delete", image_reader_delete),
  MEDIA_FN("AImageReader_acquireLatestImage", image_reader_acquire_latest_image),
  MEDIA_FN("AImage_getWidth", image_get_width),
  MEDIA_FN("AImage_deleteAsync", image_delete_async),
  MEDIA_FN("AImage_getTimestamp", image_get_timestamp),

  MEDIA_FN("AMediaFormat_getInt32", media_format_get_int32),
  MEDIA_FN("AMediaFormat_getInt64", media_format_get_int64),
  MEDIA_FN("AMediaFormat_getFloat", media_format_get_float),
  MEDIA_FN("AMediaFormat_getString", media_format_get_string),
  MEDIA_FN("AMediaFormat_delete", media_format_delete),
  MEDIA_FN("AMediaFormat_setInt32", media_format_set_int32),

  MEDIA_FN("AMediaExtractor_new", media_extractor_new),
  MEDIA_FN("AMediaExtractor_delete", media_extractor_delete),
  MEDIA_FN("AMediaExtractor_setDataSourceFd", media_extractor_set_data_source_fd),
  MEDIA_FN("AMediaExtractor_setDataSource", media_extractor_set_data_source),
  MEDIA_FN("AMediaExtractor_setDataSourceCustom", media_extractor_set_data_source_custom),
  MEDIA_FN("AMediaExtractor_getTrackCount", media_extractor_get_track_count),
  MEDIA_FN("AMediaExtractor_getTrackFormat", media_extractor_get_track_format),
  MEDIA_FN("AMediaExtractor_selectTrack", media_extractor_select_track),
  MEDIA_FN("AMediaExtractor_readSampleData", media_extractor_read_sample_data),
  MEDIA_FN("AMediaExtractor_advance", media_extractor_advance),
  MEDIA_FN("AMediaExtractor_getSampleTime", media_extractor_get_sample_time),
  MEDIA_FN("AMediaExtractor_getSampleTrackIndex", media_extractor_get_sample_track_index),
  MEDIA_FN("AMediaExtractor_seekTo", media_extractor_seek_to),

  MEDIA_FN("AMediaDataSource_new", media_data_source_new),
  MEDIA_FN("AMediaDataSource_delete", media_data_source_delete),
  MEDIA_FN("AMediaDataSource_setUserdata", media_data_source_set_userdata),
  MEDIA_FN("AMediaDataSource_setReadAt", media_data_source_set_read_at),
  MEDIA_FN("AMediaDataSource_setGetSize", media_data_source_set_get_size),
  MEDIA_FN("AMediaDataSource_setClose", media_data_source_set_close),

  MEDIA_FN("AMediaCodec_createDecoderByType", media_codec_create_decoder_by_type),
  MEDIA_FN("AMediaCodec_configure", media_codec_configure),
  MEDIA_FN("AMediaCodec_start", media_codec_start),
  MEDIA_FN("AMediaCodec_stop", media_codec_stop),
  MEDIA_FN("AMediaCodec_flush", media_codec_flush),
  MEDIA_FN("AMediaCodec_delete", media_codec_delete),
  MEDIA_FN("AMediaCodec_dequeueInputBuffer", media_codec_dequeue_input_buffer),
  MEDIA_FN("AMediaCodec_getInputBuffer", media_codec_get_input_buffer),
  MEDIA_FN("AMediaCodec_queueInputBuffer", media_codec_queue_input_buffer),
  MEDIA_FN("AMediaCodec_dequeueOutputBuffer", media_codec_dequeue_output_buffer),
  MEDIA_FN("AMediaCodec_getOutputBuffer", media_codec_get_output_buffer),
  MEDIA_FN("AMediaCodec_getOutputFormat", media_codec_get_output_format),
  MEDIA_FN("AMediaCodec_releaseOutputBuffer", media_codec_release_output_buffer),
  MEDIA_FN("AMediaCodec_setOutputSurface", media_codec_set_output_surface),
};

size_t android_media_numfunctions = sizeof(android_media_functions) / sizeof(*android_media_functions);

#undef MEDIA_FN
#undef MEDIA_KEY
