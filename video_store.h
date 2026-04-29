#ifndef VIDEO_STORE_H
#define VIDEO_STORE_H

#include <stdint.h>
#include <stdio.h>

#include "local_store.h"

#define VIDEO_STORE_DEFAULT_MAX_BYTES            (3LL * 1024 * 1024 * 1024)
#define VIDEO_STORE_DEFAULT_HIGH_WATER_BYTES     (2800LL * 1024 * 1024)
#define VIDEO_STORE_DEFAULT_LOW_WATER_BYTES      (2500LL * 1024 * 1024)
#define VIDEO_STORE_DEFAULT_SEGMENT_DURATION_MS  30000LL

typedef struct {
    LocalStore *store;
    char root_dir[PATH_MAX];
    char video_root[PATH_MAX];
    int64_t max_bytes;
    int64_t high_water_bytes;
    int64_t low_water_bytes;
    int64_t segment_duration_ms;
} VideoStore;

int video_store_init(VideoStore *video_store,
                     LocalStore *store,
                     const char *root_dir);
int video_store_build_segment_path(VideoStore *video_store,
                                   int64_t start_ms,
                                   char *path,
                                   size_t path_size);
int video_store_begin_segment(VideoStore *video_store,
                              int64_t start_ms,
                              const char *file_path,
                              int64_t *segment_id_out);
int video_store_finish_segment(VideoStore *video_store,
                               int64_t segment_id,
                               int64_t end_ms,
                               int64_t size_bytes);
int video_store_mark_segment_broken(VideoStore *video_store,
                                    int64_t segment_id,
                                    int64_t end_ms,
                                    int64_t size_bytes);
int video_store_prune(VideoStore *video_store,
                      int64_t *bytes_after_out,
                      int *deleted_count_out);
int video_store_get_file_size(const char *path, int64_t *size_out);

#endif
