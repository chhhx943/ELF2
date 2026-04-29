#define _POSIX_C_SOURCE 200809L

#include "video_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * video_store.c
 *
 * Purpose: manage local video segment files and coordinate metadata with
 * `LocalStore` (SQLite). Responsibilities include building safe paths for
 * segments, creating date directories, registering/finishing/marking segments
 * in the DB, querying file sizes, and pruning old segments using a
 * high/low-water strategy.
 *
 * Notes / :
 * - The delete/prune flow currently performs: unlink(file) -> delete DB row.
 *   This is simple and fast but can lead to inconsistency if the process
 *   crashes between these steps. Consider a two-phase approach: mark
 *   record as "deleting" in DB, unlink file, then remove DB row.
 * - Operations that interact with `LocalStore` assume `LocalStore` is
 *   already opened and healthy. Callers must ensure `local_store_open()`
 *   succeeded before using these APIs.
 * - When writing segment files, prefer writing to a temporary filename and
 *   `rename()` to the final path after the file is fully written. This
 *   prevents consumers from reading partial files.
 */

static int64_t video_store_env_i64(const char *name, int64_t fallback) {
    const char *value = getenv(name);
    char *endptr = NULL;
    long long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    errno = 0;
    parsed = strtoll(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' || parsed <= 0) {
        return fallback;
    }

    return (int64_t)parsed;
}

/*
 * Parse an integer environment variable with a positive fallback.
 * Returns `fallback` if the variable is missing, invalid, or non-positive.
 */

static int video_store_mkdirs(const char *path) {
    char tmp[PATH_MAX];
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return EINVAL;
    }

    len = strlen(path);
    if (len >= sizeof(tmp)) {
        return ENAMETOOLONG;
    }

    memcpy(tmp, path, len + 1);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
            return errno;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
        return errno;
    }

    return 0;
}

/*
 * Recursively create directories like `mkdir -p`.
 * Returns 0 on success or an errno-style error code.
 * ע�⣺Ȩ��ʹ�� 0775��������Ӧע�� SD ������ص��Ȩ�����ơ�
 */

static int video_store_copy_string(char *dst, size_t dst_size, const char *src) {
    size_t len;


/*
 * Build a filesystem path for a segment based on `start_ms`.
 * The path will be: <video_root>/<YYYY-MM-DD>/seg_YYYYMMDD_HHMMSS.ts
 * This function will create the date directory if it does not exist.
 * ע�⣺���÷��贫���㹻��� `path` ��������PATH_MAX �Ƽ�����
 */
    if (dst == NULL || dst_size == 0) {
        return EINVAL;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    len = strlen(src);
    if (len >= dst_size) {
        return ENAMETOOLONG;
    }

    memcpy(dst, src, len + 1);
    return 0;
}

int video_store_init(VideoStore *video_store,
                     LocalStore *store,
                     const char *root_dir) {
    int rc;
    const char *effective_root = root_dir;

    if (video_store == NULL || store == NULL) {
        return EINVAL;
    }

    memset(video_store, 0, sizeof(*video_store));
    video_store->store = store;
    video_store->max_bytes = VIDEO_STORE_DEFAULT_MAX_BYTES;
    video_store->high_water_bytes = VIDEO_STORE_DEFAULT_HIGH_WATER_BYTES;
    video_store->low_water_bytes = VIDEO_STORE_DEFAULT_LOW_WATER_BYTES;
    video_store->segment_duration_ms = VIDEO_STORE_DEFAULT_SEGMENT_DURATION_MS;
    video_store->max_bytes = video_store_env_i64("CAMERA_FLOW_VIDEO_MAX_BYTES", video_store->max_bytes);
    video_store->high_water_bytes = video_store_env_i64("CAMERA_FLOW_VIDEO_HIGH_WATER_BYTES", video_store->high_water_bytes);
    video_store->low_water_bytes = video_store_env_i64("CAMERA_FLOW_VIDEO_LOW_WATER_BYTES", video_store->low_water_bytes);
    video_store->segment_duration_ms = video_store_env_i64("CAMERA_FLOW_VIDEO_SEGMENT_MS", video_store->segment_duration_ms);

    if (effective_root == NULL || effective_root[0] == '\0') {
        effective_root = getenv("CAMERA_FLOW_STORE_DIR");
    }
    if (effective_root == NULL || effective_root[0] == '\0') {
        effective_root = LOCAL_STORE_DEFAULT_ROOT;
    }

    rc = video_store_copy_string(video_store->root_dir, sizeof(video_store->root_dir), effective_root);
    if (rc != 0) {
        return rc;
    }

    rc = snprintf(video_store->video_root,
                  sizeof(video_store->video_root),
                  "%s/video",
                  video_store->root_dir);
    if (rc < 0 || (size_t)rc >= sizeof(video_store->video_root)) {
        return ENAMETOOLONG;
    }

    return video_store_mkdirs(video_store->video_root);
}

/*
 * Build a filesystem path for a segment based on `start_ms`.
 * The path will be: <video_root>/<YYYY-MM-DD>/seg_YYYYMMDD_HHMMSS.ts
 * This function will create the date directory if it does not exist.
 * Note: callers should provide a buffer of size PATH_MAX to `path`.
 */
int video_store_build_segment_path(VideoStore *video_store,
                                   int64_t start_ms,
                                   char *path,
                                   size_t path_size) {
    time_t sec;
    struct tm tm_local;
    char date_dir[32];
    char file_name[64];
    char day_root[PATH_MAX];
    int rc;

    if (video_store == NULL || path == NULL || path_size == 0) {
        return EINVAL;
    }

    sec = (time_t)(start_ms / 1000LL);
    if (localtime_r(&sec, &tm_local) == NULL) {
        return EINVAL;
    }

    if (strftime(date_dir, sizeof(date_dir), "%Y-%m-%d", &tm_local) == 0) {
        return ENOSPC;
    }
    if (strftime(file_name, sizeof(file_name), "seg_%Y%m%d_%H%M%S.ts", &tm_local) == 0) {
        return ENOSPC;
    }

    rc = snprintf(day_root, sizeof(day_root), "%s/%s", video_store->video_root, date_dir);
    if (rc < 0 || (size_t)rc >= sizeof(day_root)) {
        return ENAMETOOLONG;
    }

    rc = video_store_mkdirs(day_root);
    if (rc != 0) {
        return rc;
    }

    rc = snprintf(path, path_size, "%s/%s", day_root, file_name);
    if (rc < 0 || (size_t)rc >= path_size) {
        return ENAMETOOLONG;
    }

    return 0;
}

int video_store_begin_segment(VideoStore *video_store,
                              int64_t start_ms,
                              const char *file_path,
                              int64_t *segment_id_out) {
    if (video_store == NULL || video_store->store == NULL || file_path == NULL) {
        return EINVAL;
    }

    /*
     * Register the new segment in LocalStore with status "recording".
     * The DB entry allows background uploader/pruner to discover the file.
     * Ensure `LocalStore` is open before calling this function.
     */
    return local_store_register_video_segment(video_store->store,
                                              start_ms,
                                              file_path,
                                              "recording",
                                              segment_id_out);
}

int video_store_finish_segment(VideoStore *video_store,
                               int64_t segment_id,
                               int64_t end_ms,
                               int64_t size_bytes) {
    if (video_store == NULL || video_store->store == NULL) {
        return EINVAL;
    }

    /*
     * Mark the segment as finished and ready to be uploaded/processed.
     * `size_bytes` should reflect the final file size. Status "pending"
     * indicates it is available but not yet uploaded.
     */
    return local_store_update_video_segment(video_store->store,
                                            segment_id,
                                            end_ms,
                                            size_bytes,
                                            "pending");
}

int video_store_mark_segment_broken(VideoStore *video_store,
                                    int64_t segment_id,
                                    int64_t end_ms,
                                    int64_t size_bytes) {
    if (video_store == NULL || video_store->store == NULL) {
        return EINVAL;
    }

    /*
     * Mark segment as broken/corrupted. Callers should use this when the
     * file is incomplete or writing failed. Broken segments are not
     * considered for upload but may be pruned.
     */
    return local_store_update_video_segment(video_store->store,
                                            segment_id,
                                            end_ms,
                                            size_bytes,
                                            "broken");
}

int video_store_get_file_size(const char *path, int64_t *size_out) {
    struct stat st;

    if (path == NULL || size_out == NULL) {
        return EINVAL;
    }

    /* Quick wrapper around stat() to return file size as int64_t. */
    if (stat(path, &st) < 0) {
        return errno;
    }

    *size_out = (int64_t)st.st_size;
    return 0;
}

int video_store_prune(VideoStore *video_store,
                      int64_t *bytes_after_out,
                      int *deleted_count_out) {
    int64_t total_bytes = 0;
    int deleted_count = 0;
    int rc;

    if (video_store == NULL || video_store->store == NULL) {
        return EINVAL;
    }

    /*
     * Get current total bytes tracked in DB. This should reflect files
     * present on disk, but may be stale if files were removed outside
     * the DB. `local_store_video_total_size` returns an errno-style code.
     */
    rc = local_store_video_total_size(video_store->store, &total_bytes);
    if (rc != 0) {
        return rc;
    }

    /*
     * 采用高低水位策略：总大小超过高水位才开始删，删到低水位以下为止�?
     * 避免每写一个新片段就触发一次删除�?
     */
    while (total_bytes > video_store->high_water_bytes) {
        LocalVideoSegmentRecord record;
        int found = 0;

        rc = local_store_fetch_oldest_prunable_video_segment(video_store->store, &record, &found);
        if (rc != 0) {
            return rc;
        }
        if (!found) {
            break;
        }

        /* Try to remove the file; ignore if it does not exist. */
        if (unlink(record.file_path) < 0 && errno != ENOENT) {
            return errno;
        }

        /* Remove DB record for the segment. If this fails, caller should
         * handle/report the error; we return it here to surface failures. */
        rc = local_store_delete_video_segment(video_store->store, record.id);
        if (rc != 0) {
            return rc;
        }

        deleted_count++;
        total_bytes -= record.size_bytes;
        if (total_bytes <= video_store->low_water_bytes) {
            break;
        }
    }

    if (bytes_after_out != NULL) {
        *bytes_after_out = total_bytes;
    }
    if (deleted_count_out != NULL) {
        *deleted_count_out = deleted_count;
    }

    return 0;
}
