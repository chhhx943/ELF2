#define _POSIX_C_SOURCE 200809L

#include "video_store.h"
#include "video_uploader.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char upload_root[PATH_MAX];
    int64_t fail_once_segment_id;
    int fail_once_triggered;
    int uploaded_count;
} VideoUploaderDebugCtx;

typedef enum {
    VIDEO_UPLOADER_TEST_MODE_MOCK = 0,
    VIDEO_UPLOADER_TEST_MODE_HTTP = 1
} VideoUploaderTestMode;

typedef struct {
    VideoUploaderTestMode mode;
    VideoUploaderDebugCtx mock;
} VideoUploaderDebugUploadCtx;

static int debug_mkdirs(const char *path) {
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

static int create_dummy_segment_file(const char *path, size_t bytes, unsigned char fill) {
    FILE *fp;
    unsigned char buf[1024];
    size_t remaining = bytes;

    memset(buf, fill, sizeof(buf));
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return errno;
    }

    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        if (fwrite(buf, 1, chunk, fp) != chunk) {
            fclose(fp);
            return EIO;
        }
        remaining -= chunk;
    }

    fclose(fp);
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    FILE *fin;
    FILE *fout;
    unsigned char buf[4096];
    size_t nread;

    fin = fopen(src, "rb");
    if (fin == NULL) {
        return errno;
    }

    fout = fopen(dst, "wb");
    if (fout == NULL) {
        fclose(fin);
        return errno;
    }

    while ((nread = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, nread, fout) != nread) {
            fclose(fin);
            fclose(fout);
            return EIO;
        }
    }

    fclose(fin);
    fclose(fout);
    return 0;
}

static int mock_upload_callback(const LocalVideoSegmentRecord *segment,
                                char *remote_path,
                                size_t remote_path_size,
                                char *error_msg,
                                size_t error_msg_size,
                                void *user_data) {
    VideoUploaderDebugCtx *ctx = (VideoUploaderDebugCtx *)user_data;
    const char *base_name;
    char target_path[PATH_MAX];
    int rc;

    if (ctx == NULL) {
        snprintf(error_msg, error_msg_size, "debug ctx is null");
        return EINVAL;
    }

    if (segment->id == ctx->fail_once_segment_id && !ctx->fail_once_triggered) {
        ctx->fail_once_triggered = 1;
        snprintf(error_msg, error_msg_size, "simulated transient upload failure");
        return EIO;
    }

    base_name = strrchr(segment->file_path, '/');
    base_name = (base_name == NULL) ? segment->file_path : base_name + 1;

    rc = snprintf(target_path, sizeof(target_path), "%s/%s", ctx->upload_root, base_name);
    if (rc < 0 || (size_t)rc >= sizeof(target_path)) {
        snprintf(error_msg, error_msg_size, "target path too long");
        return ENAMETOOLONG;
    }

    rc = copy_file(segment->file_path, target_path);
    if (rc != 0) {
        snprintf(error_msg, error_msg_size, "copy_file failed: %d", rc);
        return rc;
    }

    ctx->uploaded_count++;
    snprintf(remote_path, remote_path_size, "%s", target_path);
    return 0;
}

static int wait_until_uploaded(const char *root_dir,
                               int64_t id1,
                               int64_t id2,
                               int64_t id3,
                               int timeout_ms) {
    LocalStore store;
    LocalVideoSegmentRecord rec1;
    LocalVideoSegmentRecord rec2;
    LocalVideoSegmentRecord rec3;
    int found1;
    int found2;
    int found3;
    int rc;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        memset(&store, 0, sizeof(store));
        rc = local_store_open(&store, root_dir);
        if (rc != 0) {
            return rc;
        }

        rc = local_store_debug_fetch_video_segment_by_id(&store, id1, &rec1, &found1);
        if (rc == 0) {
            rc = local_store_debug_fetch_video_segment_by_id(&store, id2, &rec2, &found2);
        }
        if (rc == 0) {
            rc = local_store_debug_fetch_video_segment_by_id(&store, id3, &rec3, &found3);
        }
        local_store_close(&store);
        if (rc != 0) {
            return rc;
        }

        if (found1 && found2 && found3 &&
            strcmp(rec1.state, "uploaded") == 0 &&
            strcmp(rec2.state, "uploaded") == 0 &&
            strcmp(rec3.state, "uploaded") == 0) {
            printf("[VideoUploaderTest] All segments uploaded.\n");
            printf("[VideoUploaderTest] seg2 retry_count=%d, last_error=%s\n",
                   rec2.retry_count, rec2.last_error);
            return 0;
        }

        poll(NULL, 0, 200);
        elapsed += 200;
    }

    return ETIMEDOUT;
}

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s [mock|http] [root_dir]\n", prog);
    printf("\n");
    printf("Modes:\n");
    printf("  mock : use local copy as upload backend (default)\n");
    printf("  http : use real HTTP upload callback\n");
}

static int dispatch_upload_callback(const LocalVideoSegmentRecord *segment,
                                    char *remote_path,
                                    size_t remote_path_size,
                                    char *error_msg,
                                    size_t error_msg_size,
                                    void *user_data) {
    VideoUploaderDebugUploadCtx *ctx = (VideoUploaderDebugUploadCtx *)user_data;

    if (ctx == NULL) {
        snprintf(error_msg, error_msg_size, "debug upload ctx is null");
        return EINVAL;
    }

    if (ctx->mode == VIDEO_UPLOADER_TEST_MODE_HTTP) {
        return video_uploader_http_upload_callback(segment,
                                                   remote_path,
                                                   remote_path_size,
                                                   error_msg,
                                                   error_msg_size,
                                                   NULL);
    }

    return mock_upload_callback(segment,
                                remote_path,
                                remote_path_size,
                                error_msg,
                                error_msg_size,
                                &ctx->mock);
}

int main(int argc, char *argv[]) {
    char default_root_template[] = "/tmp/video_uploader_debug_XXXXXX";
    const char *mode_arg = "mock";
    const char *root_dir = NULL;
    char *owned_root = NULL;
    char upload_root[PATH_MAX];
    LocalStore store;
    VideoStore video_store;
    VideoUploader uploader;
    VideoUploaderDebugUploadCtx upload_ctx;
    int64_t seg_ids[3] = {0, 0, 0};
    int64_t start_ms = 1777500000000LL;
    int rc;

    if (argc >= 2) {
        mode_arg = argv[1];
        if (strcmp(mode_arg, "mock") != 0 && strcmp(mode_arg, "http") != 0) {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argc >= 3) {
        root_dir = argv[2];
    } else {
        owned_root = mkdtemp(default_root_template);
        if (owned_root == NULL) {
            perror("mkdtemp");
            return 1;
        }
        root_dir = owned_root;
    }

    printf("[VideoUploaderTest] root_dir=%s\n", root_dir);
    printf("[VideoUploaderTest] mode=%s\n", mode_arg);
    setenv("CAMERA_FLOW_STORE_DIR", root_dir, 1);
    setenv("CAMERA_FLOW_VIDEO_UPLOADER_IDLE_MS", "200", 1);
    setenv("CAMERA_FLOW_VIDEO_UPLOADER_RETRY_BASE_MS", "300", 1);
    setenv("CAMERA_FLOW_VIDEO_UPLOADER_RETRY_MAX_MS", "1000", 1);

    memset(&store, 0, sizeof(store));
    memset(&video_store, 0, sizeof(video_store));
    memset(&uploader, 0, sizeof(uploader));
    memset(&upload_ctx, 0, sizeof(upload_ctx));

    upload_ctx.mode = strcmp(mode_arg, "http") == 0
                    ? VIDEO_UPLOADER_TEST_MODE_HTTP
                    : VIDEO_UPLOADER_TEST_MODE_MOCK;

    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        printf("[VideoUploaderTest] local_store_open failed: %d\n", rc);
        return 1;
    }

    rc = video_store_init(&video_store, &store, root_dir);
    if (rc != 0) {
        printf("[VideoUploaderTest] video_store_init failed: %d\n", rc);
        local_store_close(&store);
        return 1;
    }

    rc = snprintf(upload_root, sizeof(upload_root), "%s/uploaded_mock", root_dir);
    if (rc < 0 || (size_t)rc >= sizeof(upload_root)) {
        local_store_close(&store);
        return 1;
    }
    rc = debug_mkdirs(upload_root);
    if (rc != 0) {
        printf("[VideoUploaderTest] debug_mkdirs failed: %d\n", rc);
        local_store_close(&store);
        return 1;
    }
    snprintf(upload_ctx.mock.upload_root, sizeof(upload_ctx.mock.upload_root), "%s", upload_root);

    for (int i = 0; i < 3; ++i) {
        char path[PATH_MAX];
        int64_t segment_id = 0;
        int64_t segment_start = start_ms + i * 30000LL;
        int64_t segment_end = segment_start + 5000LL;
        int64_t size_bytes;

        rc = video_store_build_segment_path(&video_store, segment_start, path, sizeof(path));
        if (rc != 0) {
            printf("[VideoUploaderTest] build path failed: %d\n", rc);
            local_store_close(&store);
            return 1;
        }

        rc = create_dummy_segment_file(path, 1024 + i * 128, (unsigned char)(0x41 + i));
        if (rc != 0) {
            printf("[VideoUploaderTest] create file failed: %d\n", rc);
            local_store_close(&store);
            return 1;
        }

        rc = video_store_get_file_size(path, &size_bytes);
        if (rc != 0) {
            printf("[VideoUploaderTest] stat file failed: %d\n", rc);
            local_store_close(&store);
            return 1;
        }

        rc = video_store_begin_segment(&video_store, segment_start, path, &segment_id);
        if (rc != 0) {
            printf("[VideoUploaderTest] begin segment failed: %d\n", rc);
            local_store_close(&store);
            return 1;
        }

        rc = video_store_finish_segment(&video_store, segment_id, segment_end, size_bytes);
        if (rc != 0) {
            printf("[VideoUploaderTest] finish segment failed: %d\n", rc);
            local_store_close(&store);
            return 1;
        }

        seg_ids[i] = segment_id;
    }

    rc = local_store_mark_video_segment_uploading(&store, seg_ids[2]);
    if (rc != 0) {
        printf("[VideoUploaderTest] mark uploading failed: %d\n", rc);
        local_store_close(&store);
        return 1;
    }

    if (upload_ctx.mode == VIDEO_UPLOADER_TEST_MODE_MOCK) {
        upload_ctx.mock.fail_once_segment_id = seg_ids[1];
    }
    local_store_close(&store);

    rc = video_uploader_start(&uploader, root_dir, dispatch_upload_callback, &upload_ctx);
    if (rc != 0) {
        printf("[VideoUploaderTest] video_uploader_start failed: %d\n", rc);
        return 1;
    }

    rc = wait_until_uploaded(root_dir, seg_ids[0], seg_ids[1], seg_ids[2], 10000);
    video_uploader_stop(&uploader);
    if (rc != 0) {
        printf("[VideoUploaderTest] wait_until_uploaded failed: %d\n", rc);
        local_store_debug_dump_video_segments(root_dir, 10);
        return 1;
    }

    local_store_debug_dump_video_segments(root_dir, 10);
    printf("[VideoUploaderTest] uploaded_count=%d, fail_once_triggered=%d\n",
           upload_ctx.mock.uploaded_count, upload_ctx.mock.fail_once_triggered);
    printf("[VideoUploaderTest] success\n");
    return 0;
}
