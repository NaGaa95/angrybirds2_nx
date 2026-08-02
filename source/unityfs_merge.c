/* UnityFS split-archive preparation.
 *
 * Android exposes the base APK and Play Asset Delivery pack through one virtual
 * AssetManager namespace. On the SD card they are ordinary files, so Unity sees
 * only the six serialized files in data.unity3d and cannot resolve the 3,308
 * serialized files in datapack.unity3d. Build a single UnityFS cache on first
 * boot while preserving every compressed data block byte-for-byte.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "error.h"
#include "unityfs_merge.h"

#define UNITYFS_PATH_MAX       768
#define UNITYFS_STRING_MAX      64
#define UNITYFS_INFO_MAX  (16u * 1024u * 1024u)
#define UNITYFS_COPY_CHUNK (1024u * 1024u)
#define UNITYFS_SAMPLE_SIZE (64u * 1024u)

typedef struct {
  char path[UNITYFS_PATH_MAX];
  char unity_version[UNITYFS_STRING_MAX];
  char unity_revision[UNITYFS_STRING_MAX];
  uint32_t version;
  uint32_t flags;
  uint64_t file_size;
  uint64_t declared_size;
  uint64_t data_offset;
  uint64_t compressed_data_size;
  uint64_t uncompressed_data_size;
  uint32_t block_count;
  uint32_t node_count;
  uint8_t hash[16];
  uint8_t *info;
  size_t info_size;
  size_t block_records_offset;
  size_t node_records_offset;
  size_t node_records_size;
} UnityFSArchive;

static int g_redirect_ready;
static char g_cache_path[UNITYFS_PATH_MAX];
static FILE *g_log;

typedef struct {
  uint64_t copied;
  uint64_t total;
  unsigned last_percent;
} UnityFSCopyProgress;

static void merge_log(const char *fmt, ...) {
  if (!g_log) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

static int merge_error(const char *fmt, ...) {
  if (g_log) {
    fputs("ERROR: ", g_log);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
  }
  return -1;
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) {
  return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
  put_be32(p, (uint32_t)(v >> 32));
  put_be32(p + 4, (uint32_t)v);
}

static uint64_t align16(uint64_t v) {
  return (v + 15u) & ~(uint64_t)15u;
}

static int read_exact(FILE *f, void *dst, size_t size) {
  return size == 0 || fread(dst, 1, size, f) == size ? 0 : -1;
}

static int write_exact(FILE *f, const void *src, size_t size) {
  return size == 0 || fwrite(src, 1, size, f) == size ? 0 : -1;
}

static int read_cstring(FILE *f, char *dst, size_t capacity) {
  if (!capacity) return -1;
  for (size_t i = 0; i < capacity; ++i) {
    int c = fgetc(f);
    if (c == EOF) return -1;
    dst[i] = (char)c;
    if (!c) return 0;
  }
  dst[capacity - 1] = 0;
  return -1;
}

/* Unity uses both LZ4 and LZ4HC for the blocks-info record. Their on-disk
 * decoder is the same raw LZ4 block format. */
static int lz4_decompress_safe(const uint8_t *src, size_t src_size,
                               uint8_t *dst, size_t dst_size) {
  const uint8_t *s = src, *send = src + src_size;
  uint8_t *d = dst, *dend = dst + dst_size;

  while (s < send) {
    uint8_t token = *s++;
    size_t literal = token >> 4;
    if (literal == 15) {
      uint8_t extra;
      do {
        if (s >= send || literal > SIZE_MAX - 255) return -1;
        extra = *s++;
        literal += extra;
      } while (extra == 255);
    }
    if (literal > (size_t)(send - s) || literal > (size_t)(dend - d)) return -1;
    memcpy(d, s, literal);
    s += literal;
    d += literal;
    if (s == send) return d == dend ? 0 : -1;
    if ((size_t)(send - s) < 2) return -1;

    size_t distance = (size_t)s[0] | ((size_t)s[1] << 8);
    s += 2;
    if (!distance || distance > (size_t)(d - dst)) return -1;

    size_t match = token & 15;
    if (match == 15) {
      uint8_t extra;
      do {
        if (s >= send || match > SIZE_MAX - 255) return -1;
        extra = *s++;
        match += extra;
      } while (extra == 255);
    }
    if (match > SIZE_MAX - 4) return -1;
    match += 4;
    if (match > (size_t)(dend - d)) return -1;
    uint8_t *from = d - distance;
    while (match--) *d++ = *from++;
  }
  return d == dend ? 0 : -1;
}

static void archive_free(UnityFSArchive *a) {
  free(a->info);
  memset(a, 0, sizeof(*a));
}

static int archive_parse(const char *path, UnityFSArchive *a) {
  uint8_t fields[20];
  uint8_t *compressed = NULL;
  FILE *f = NULL;
  memset(a, 0, sizeof(*a));
  snprintf(a->path, sizeof(a->path), "%s", path);

  f = fopen(path, "rb");
  if (!f) return merge_error("cannot open %s (%d)", path, errno);
  if (fseeko(f, 0, SEEK_END) != 0) goto invalid;
  off_t end = ftello(f);
  if (end <= 0 || fseeko(f, 0, SEEK_SET) != 0) goto invalid;
  a->file_size = (uint64_t)end;

  char magic[16];
  if (read_cstring(f, magic, sizeof(magic)) != 0 || strcmp(magic, "UnityFS"))
    goto invalid;
  uint8_t version_bytes[4];
  if (read_exact(f, version_bytes, sizeof(version_bytes)) != 0 ||
      read_cstring(f, a->unity_version, sizeof(a->unity_version)) != 0 ||
      read_cstring(f, a->unity_revision, sizeof(a->unity_revision)) != 0 ||
      read_exact(f, fields, sizeof(fields)) != 0)
    goto invalid;

  a->version = be32(version_bytes);
  a->declared_size = be64(fields);
  uint32_t compressed_info_size = be32(fields + 8);
  uint32_t info_size = be32(fields + 12);
  a->flags = be32(fields + 16);
  off_t header_end = ftello(f);
  if (header_end < 0 || !compressed_info_size || !info_size ||
      compressed_info_size > UNITYFS_INFO_MAX || info_size > UNITYFS_INFO_MAX)
    goto invalid;

  uint64_t header_aligned = align16((uint64_t)header_end);
  uint64_t info_offset = (a->flags & 0x80u)
                           ? a->file_size - compressed_info_size : header_aligned;
  if (info_offset > a->file_size ||
      compressed_info_size > a->file_size - info_offset ||
      fseeko(f, (off_t)info_offset, SEEK_SET) != 0)
    goto invalid;

  compressed = malloc(compressed_info_size);
  a->info = malloc(info_size);
  if (!compressed || !a->info) goto invalid;
  if (read_exact(f, compressed, compressed_info_size) != 0) goto invalid;

  unsigned compression = a->flags & 0x3fu;
  if (compression == 0) {
    if (compressed_info_size != info_size) goto invalid;
    memcpy(a->info, compressed, info_size);
  } else if (compression == 2 || compression == 3) {
    if (lz4_decompress_safe(compressed, compressed_info_size,
                            a->info, info_size) != 0)
      goto invalid;
  } else {
    merge_error("unsupported UnityFS blocks-info compression %u in %s",
                compression, path);
    goto fail;
  }
  free(compressed);
  compressed = NULL;
  a->info_size = info_size;

  const uint8_t *p = a->info, *info_end = a->info + a->info_size;
  if ((size_t)(info_end - p) < 20) goto invalid;
  memcpy(a->hash, p, sizeof(a->hash));
  p += 16;
  a->block_count = be32(p);
  p += 4;
  if (a->block_count > (uint32_t)((info_end - p) / 10)) goto invalid;
  a->block_records_offset = (size_t)(p - a->info);
  for (uint32_t i = 0; i < a->block_count; ++i, p += 10) {
    uint32_t uncompressed = be32(p);
    uint32_t compressed_size = be32(p + 4);
    if (a->uncompressed_data_size > UINT64_MAX - uncompressed ||
        a->compressed_data_size > UINT64_MAX - compressed_size)
      goto invalid;
    a->uncompressed_data_size += uncompressed;
    a->compressed_data_size += compressed_size;
  }
  if ((size_t)(info_end - p) < 4) goto invalid;
  a->node_count = be32(p);
  p += 4;
  a->node_records_offset = (size_t)(p - a->info);
  for (uint32_t i = 0; i < a->node_count; ++i) {
    if ((size_t)(info_end - p) < 21) goto invalid;
    p += 20;
    const uint8_t *nul = memchr(p, 0, (size_t)(info_end - p));
    if (!nul) goto invalid;
    p = nul + 1;
  }
  if (p != info_end) goto invalid;
  a->node_records_size = (size_t)(p - a->info) - a->node_records_offset;

  a->data_offset = (a->flags & 0x80u)
                     ? header_aligned : align16(info_offset + compressed_info_size);
  uint64_t data_limit = (a->flags & 0x80u) ? info_offset : a->file_size;
  if (a->declared_size != a->file_size || a->data_offset > data_limit ||
      a->compressed_data_size > data_limit - a->data_offset)
    goto invalid;

  fclose(f);
  return 0;

invalid:
  merge_error("invalid or truncated UnityFS archive: %s", path);
fail:
  if (f) fclose(f);
  free(compressed);
  archive_free(a);
  return -1;
}

static int build_merged_info(const UnityFSArchive *base,
                             const UnityFSArchive *pack,
                             uint8_t **out_info, size_t *out_size) {
  if (base->block_count > UINT32_MAX - pack->block_count ||
      base->node_count > UINT32_MAX - pack->node_count ||
      base->node_records_size > SIZE_MAX - pack->node_records_size)
    return merge_error("UnityFS table size overflow");

  uint32_t blocks = base->block_count + pack->block_count;
  uint32_t nodes = base->node_count + pack->node_count;
  size_t node_bytes = base->node_records_size + pack->node_records_size;
  if (blocks > (SIZE_MAX - 24u - node_bytes) / 10u)
    return merge_error("UnityFS blocks-info size overflow");
  size_t size = 24u + (size_t)blocks * 10u + node_bytes;
  uint8_t *info = malloc(size);
  if (!info) return merge_error("not enough memory for merged UnityFS table");

  uint8_t *d = info;
  memcpy(d, base->hash, 16);
  d += 16;
  put_be32(d, blocks);
  d += 4;
  size_t base_block_bytes = (size_t)base->block_count * 10u;
  size_t pack_block_bytes = (size_t)pack->block_count * 10u;
  memcpy(d, base->info + base->block_records_offset, base_block_bytes);
  d += base_block_bytes;
  memcpy(d, pack->info + pack->block_records_offset, pack_block_bytes);
  d += pack_block_bytes;
  put_be32(d, nodes);
  d += 4;
  memcpy(d, base->info + base->node_records_offset, base->node_records_size);
  d += base->node_records_size;

  const uint8_t *p = pack->info + pack->node_records_offset;
  const uint8_t *pend = p + pack->node_records_size;
  while (p < pend) {
    uint64_t old_offset = be64(p);
    if (old_offset > UINT64_MAX - base->uncompressed_data_size) {
      free(info);
      return merge_error("UnityFS node offset overflow");
    }
    put_be64(d, old_offset + base->uncompressed_data_size);
    memcpy(d + 8, p + 8, 12);
    p += 20;
    d += 20;
    const uint8_t *nul = memchr(p, 0, (size_t)(pend - p));
    if (!nul) {
      free(info);
      return merge_error("invalid UnityFS node path");
    }
    size_t path_size = (size_t)(nul - p) + 1;
    memcpy(d, p, path_size);
    p += path_size;
    d += path_size;
  }
  if ((size_t)(d - info) != size) {
    free(info);
    return merge_error("merged UnityFS table length mismatch");
  }
  *out_info = info;
  *out_size = size;
  return 0;
}

static int write_padding(FILE *f, uint64_t count) {
  static const uint8_t zero[16];
  while (count) {
    size_t n = count > sizeof(zero) ? sizeof(zero) : (size_t)count;
    if (write_exact(f, zero, n) != 0) return -1;
    count -= n;
  }
  return 0;
}

static void copy_progress_update(UnityFSCopyProgress *progress, uint64_t amount) {
  progress->copied += amount;
  unsigned percent = progress->total
                       ? (unsigned)((progress->copied * 100u) / progress->total)
                       : 100u;
  if (percent == progress->last_percent) return;
  char status[96];
  snprintf(status, sizeof status,
           "Optimizing game assets (first boot)\n\n  %u%%", percent);
  startup_status_update(status);
  progress->last_percent = percent;
}

static int copy_archive_data(FILE *out, const UnityFSArchive *a, uint8_t *buffer,
                             UnityFSCopyProgress *progress) {
  FILE *in = fopen(a->path, "rb");
  if (!in) return merge_error("cannot reopen %s (%d)", a->path, errno);
  if (fseeko(in, (off_t)a->data_offset, SEEK_SET) != 0) {
    fclose(in);
    return merge_error("cannot seek in %s", a->path);
  }
  uint64_t left = a->compressed_data_size;
  while (left) {
    size_t n = left > UNITYFS_COPY_CHUNK ? UNITYFS_COPY_CHUNK : (size_t)left;
    if (read_exact(in, buffer, n) != 0 || write_exact(out, buffer, n) != 0) {
      fclose(in);
      return merge_error("I/O error while copying %s", a->path);
    }
    left -= n;
    copy_progress_update(progress, n);
  }
  fclose(in);
  return 0;
}

static int write_merged_archive(const char *path, const UnityFSArchive *base,
                                const UnityFSArchive *pack,
                                const uint8_t *info, size_t info_size) {
  uint8_t header[256];
  uint8_t *p = header;
  const char magic[] = "UnityFS";
  size_t uv_len = strlen(base->unity_version) + 1;
  size_t rev_len = strlen(base->unity_revision) + 1;
  size_t header_size = sizeof(magic) + 4u + uv_len + rev_len + 20u;
  if (header_size > sizeof(header) || info_size > UINT32_MAX)
    return merge_error("UnityFS output header is too large");

  uint64_t header_aligned = align16(header_size);
  uint64_t data_offset = align16(header_aligned + info_size);
  if (base->compressed_data_size > UINT64_MAX - pack->compressed_data_size)
    return merge_error("UnityFS output data size overflow");
  uint64_t compressed_data = base->compressed_data_size + pack->compressed_data_size;
  if (data_offset > UINT64_MAX - compressed_data)
    return merge_error("UnityFS output file size overflow");
  uint64_t output_size = data_offset + compressed_data;

  memcpy(p, magic, sizeof(magic));
  p += sizeof(magic);
  put_be32(p, base->version);
  p += 4;
  memcpy(p, base->unity_version, uv_len);
  p += uv_len;
  memcpy(p, base->unity_revision, rev_len);
  p += rev_len;
  put_be64(p, output_size);
  p += 8;
  put_be32(p, (uint32_t)info_size);
  p += 4;
  put_be32(p, (uint32_t)info_size);
  p += 4;
  /* Store the small blocks-info table uncompressed and always at the front. */
  put_be32(p, base->flags & ~(uint32_t)(0x3fu | 0x80u));
  p += 4;
  if ((size_t)(p - header) != header_size)
    return merge_error("UnityFS output header length mismatch");

  FILE *out = fopen(path, "wb");
  if (!out) return merge_error("cannot create %s (%d)", path, errno);
  uint8_t *buffer = malloc(UNITYFS_COPY_CHUNK);
  UnityFSCopyProgress progress = {
    .copied = 0,
    .total = compressed_data,
    .last_percent = 101,
  };
  int ok = buffer &&
           write_exact(out, header, header_size) == 0 &&
           write_padding(out, header_aligned - header_size) == 0 &&
           write_exact(out, info, info_size) == 0 &&
           write_padding(out, data_offset - header_aligned - info_size) == 0 &&
           copy_archive_data(out, base, buffer, &progress) == 0 &&
           copy_archive_data(out, pack, buffer, &progress) == 0 &&
           fflush(out) == 0 && !ferror(out);
  free(buffer);
  if (fclose(out) != 0) ok = 0;
  if (!ok) return merge_error("failed to write complete UnityFS cache");
  return 0;
}

static int compare_bytes(const char *a_path, uint64_t a_offset,
                         const char *b_path, uint64_t b_offset, size_t size) {
  FILE *a = fopen(a_path, "rb");
  FILE *b = fopen(b_path, "rb");
  uint8_t *abuf = NULL, *bbuf = NULL;
  int equal = 0;
  if (!a || !b || fseeko(a, (off_t)a_offset, SEEK_SET) != 0 ||
      fseeko(b, (off_t)b_offset, SEEK_SET) != 0)
    goto done;
  abuf = malloc(size ? size : 1);
  bbuf = malloc(size ? size : 1);
  if (!abuf || !bbuf || read_exact(a, abuf, size) != 0 ||
      read_exact(b, bbuf, size) != 0)
    goto done;
  equal = memcmp(abuf, bbuf, size) == 0;
done:
  free(abuf);
  free(bbuf);
  if (a) fclose(a);
  if (b) fclose(b);
  return equal;
}

static int compare_archive_samples(const UnityFSArchive *source,
                                   const UnityFSArchive *cache,
                                   uint64_t cache_relative_offset) {
  size_t sample = source->compressed_data_size > UNITYFS_SAMPLE_SIZE
                    ? UNITYFS_SAMPLE_SIZE : (size_t)source->compressed_data_size;
  if (!compare_bytes(source->path, source->data_offset,
                     cache->path, cache->data_offset + cache_relative_offset,
                     sample))
    return 0;
  if (source->compressed_data_size > sample &&
      !compare_bytes(source->path,
                     source->data_offset + source->compressed_data_size - sample,
                     cache->path,
                     cache->data_offset + cache_relative_offset +
                       source->compressed_data_size - sample,
                     sample))
    return 0;
  return 1;
}

static int cache_matches(const char *path, const UnityFSArchive *base,
                         const UnityFSArchive *pack,
                         const uint8_t *expected_info, size_t expected_info_size) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  UnityFSArchive cache;
  if (archive_parse(path, &cache) != 0) return 0;
  int match = cache.info_size == expected_info_size &&
              memcmp(cache.info, expected_info, expected_info_size) == 0 &&
              cache.block_count == base->block_count + pack->block_count &&
              cache.node_count == base->node_count + pack->node_count &&
              cache.uncompressed_data_size ==
                base->uncompressed_data_size + pack->uncompressed_data_size &&
              cache.compressed_data_size ==
                base->compressed_data_size + pack->compressed_data_size &&
              compare_archive_samples(base, &cache, 0) &&
              compare_archive_samples(pack, &cache, base->compressed_data_size);
  archive_free(&cache);
  return match;
}

static int same_format(const UnityFSArchive *a, const UnityFSArchive *b) {
  return a->version == b->version &&
         strcmp(a->unity_version, b->unity_version) == 0 &&
         strcmp(a->unity_revision, b->unity_revision) == 0 &&
         ((a->flags ^ b->flags) & ~(uint32_t)0x3fu) == 0;
}

int unityfs_prepare(const char *data_root) {
  char base_path[UNITYFS_PATH_MAX], pack_path[UNITYFS_PATH_MAX];
  char temp_path[UNITYFS_PATH_MAX], log_path[UNITYFS_PATH_MAX];
  struct stat st;
  g_redirect_ready = 0;
  g_cache_path[0] = 0;
  snprintf(base_path, sizeof(base_path), "%s/assets/bin/Data/data.unity3d", data_root);
  snprintf(pack_path, sizeof(pack_path), "%s/assets/bin/Data/datapack.unity3d", data_root);
  if (stat(pack_path, &st) != 0) return 0; /* staging utility already merged it */

  snprintf(g_cache_path, sizeof(g_cache_path),
           "%s/assets/bin/Data/data.nx.unity3d", data_root);
  snprintf(temp_path, sizeof(temp_path), "%s.tmp", g_cache_path);
  snprintf(log_path, sizeof(log_path), "%s/unityfs_merge.log", data_root);
  g_log = fopen(log_path, "wb");
  merge_log("Angry Birds 2 UnityFS preparation");
  merge_log("base: %s", base_path);
  merge_log("pack: %s", pack_path);

  UnityFSArchive base, pack;
  uint8_t *merged_info = NULL;
  size_t merged_info_size = 0;
  int result = -1;
  int status_started = 0;
  if (archive_parse(base_path, &base) != 0) goto done_base;
  if (archive_parse(pack_path, &pack) != 0) goto done_pack;
  merge_log("base: %llu bytes, %u blocks, %u nodes",
            (unsigned long long)base.file_size, base.block_count, base.node_count);
  merge_log("pack: %llu bytes, %u blocks, %u nodes",
            (unsigned long long)pack.file_size, pack.block_count, pack.node_count);

  /* A user may overlay the loose pack onto an output previously made by the
   * desktop stager. In that case data.unity3d already contains the pack. */
  if (base.block_count > pack.block_count && base.node_count > pack.node_count) {
    merge_log("base archive is already merged; standalone pack is redundant");
    result = 0;
    goto done;
  }
  if (!same_format(&base, &pack)) {
    merge_error("base and asset-pack UnityFS formats do not match");
    goto done;
  }
  if (build_merged_info(&base, &pack, &merged_info, &merged_info_size) != 0)
    goto done;

  if (cache_matches(g_cache_path, &base, &pack, merged_info, merged_info_size)) {
    merge_log("validated existing cache: %s", g_cache_path);
    g_redirect_ready = 1;
    result = 0;
    goto done;
  }

  merge_log("building first-boot cache (%u blocks, %u nodes)",
            base.block_count + pack.block_count,
            base.node_count + pack.node_count);
  startup_status_begin("Optimizing game assets (first boot)\n\n  0%");
  status_started = 1;
  remove(temp_path);
  if (write_merged_archive(temp_path, &base, &pack,
                           merged_info, merged_info_size) != 0)
    goto done;
  startup_status_update("Validating game data");
  if (!cache_matches(temp_path, &base, &pack, merged_info, merged_info_size)) {
    merge_error("new UnityFS cache failed validation");
    goto done;
  }
  if (remove(g_cache_path) != 0 && errno != ENOENT) {
    merge_error("cannot replace old UnityFS cache (%d)", errno);
    goto done;
  }
  if (rename(temp_path, g_cache_path) != 0) {
    merge_error("cannot install UnityFS cache (%d)", errno);
    goto done;
  }
  merge_log("installed validated cache: %s", g_cache_path);
  g_redirect_ready = 1;
  result = 0;

done:
  if (result != 0) remove(temp_path);
  free(merged_info);
  archive_free(&pack);
done_pack:
  archive_free(&base);
done_base:
  if (status_started) startup_status_end();
  if (g_log) {
    merge_log(result == 0 ? "ready" : "failed");
    fclose(g_log);
    g_log = NULL;
  }
  return result;
}

static int separator(char c) {
  return c == '/' || c == '\\';
}

static int path_ends_with(const char *path, const char *suffix) {
  size_t path_len = strlen(path), suffix_len = strlen(suffix);
  if (suffix_len > path_len) return 0;
  path += path_len - suffix_len;
  for (size_t i = 0; i < suffix_len; ++i) {
    if (separator(path[i]) && separator(suffix[i])) continue;
    if (path[i] != suffix[i]) return 0;
  }
  return 1;
}

const char *unityfs_redirect_path(const char *path) {
  if (!g_redirect_ready || !path) return path;
  if (!strcmp(path, "data.unity3d") ||
      path_ends_with(path, "assets/bin/Data/data.unity3d"))
    return g_cache_path;
  return path;
}
