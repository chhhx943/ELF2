#include "camera.h"
#include "ipc.h"
#include "encoder.h"
#include "sensor_modbus.h"
#include "aliyun_mqtt.h"
#include "video_store.h"
#include "video_uploader.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#define VIDEO_DEV "/dev/video11"
#define RTMP_URL "rtmp://192.168.31.122/live/test"
#define WIDTH  1280
#define HEIGHT 720
#define BUF_COUNT 4
#define FRAME_SIZE (WIDTH * HEIGHT * 3 / 2)
#define SENSOR_DEVICE_ENV "SENSOR_MODBUS_DEV"
#define SENSOR_DEVICE_DEFAULT "/dev/ttyUSB0"
#define STREAM_RESTART_BASE_MS 1000
#define STREAM_RESTART_MAX_MS 30000
#define CHILD_RTMP_RETRY_BASE_MS 1000
#define CHILD_RTMP_RETRY_MAX_MS 30000

volatile sig_atomic_t is_running = 1;

typedef struct {
    int stream_online;
    pid_t child_pid;
    int parent_sock;
    int base_restart_ms;
    int max_restart_ms;
    int current_restart_ms;
    int restart_fail_count;
    int64_t last_restart_mono_ms;
} StreamState;

typedef enum {
    CHILD_OUTPUT_RTMP = 0,
    CHILD_OUTPUT_FILE = 1
} ChildOutputMode;

typedef struct {
    FFmpegStreamer streamer;
    int streamer_ready;
    ChildOutputMode mode;

    LocalStore store;
    int store_ready;
    VideoStore video_store;
    int video_store_ready;

    int64_t current_segment_id;
    int64_t current_segment_start_wall_ms;
    int64_t current_segment_start_mono_ms;
    char current_segment_path[PATH_MAX];

    int rtmp_retry_backoff_ms;
    int rtmp_retry_max_ms;
    int64_t last_rtmp_retry_mono_ms;

    int debug_rtmp_fail_after_frames;
    int debug_rtmp_frame_count;
    int debug_rtmp_fail_triggered;
} ChildOutputCtx;

static void sig_handler(int sig) {
    (void)sig;
    is_running = 0;
}

static int64_t mono_now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int64_t wall_now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void reset_restart_backoff(StreamState *stream) {
    stream->restart_fail_count = 0;
    stream->current_restart_ms = stream->base_restart_ms;
}

static void increase_restart_backoff(StreamState *stream) {
    int next_delay = stream->current_restart_ms * 2;

    stream->restart_fail_count++;
    if (next_delay > stream->max_restart_ms) {
        next_delay = stream->max_restart_ms;
    }
    stream->current_restart_ms = next_delay;
}

static void child_reset_rtmp_backoff(ChildOutputCtx *ctx) {
    ctx->rtmp_retry_backoff_ms = CHILD_RTMP_RETRY_BASE_MS;
}

static void child_increase_rtmp_backoff(ChildOutputCtx *ctx) {
    int next_delay = ctx->rtmp_retry_backoff_ms * 2;

    if (next_delay > ctx->rtmp_retry_max_ms) {
        next_delay = ctx->rtmp_retry_max_ms;
    }
    ctx->rtmp_retry_backoff_ms = next_delay;
}

static int env_to_positive_int(const char *name, int fallback) {
    const char *value = getenv(name);
    char *endptr = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    errno = 0;
    parsed = strtol(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' || parsed <= 0) {
        return fallback;
    }

    return (int)parsed;
}

static int video_uploader_macro_ready(char *reason, size_t reason_size) {
    int has_url = VIDEO_UPLOADER_HTTP_UPLOAD_URL[0] != '\0';
    int has_token = VIDEO_UPLOADER_HTTP_AUTH_TOKEN[0] != '\0';
    int has_device_id = VIDEO_UPLOADER_HTTP_DEVICE_ID[0] != '\0';
    int offset = 0;
    int rc;

    if (reason != NULL && reason_size > 0) {
        reason[0] = '\0';
    }

    if (has_url && has_token && has_device_id) {
        return 1;
    }

    if (reason == NULL || reason_size == 0) {
        return 0;
    }

    rc = snprintf(reason + offset, reason_size - (size_t)offset,
                  "missing macro:");
    if (rc < 0 || (size_t)rc >= reason_size - (size_t)offset) {
        reason[reason_size - 1] = '\0';
        return 0;
    }
    offset += rc;

    if (!has_url && offset < (int)reason_size) {
        rc = snprintf(reason + offset, reason_size - (size_t)offset,
                      " VIDEO_UPLOADER_HTTP_UPLOAD_URL");
        if (rc < 0 || (size_t)rc >= reason_size - (size_t)offset) {
            reason[reason_size - 1] = '\0';
            return 0;
        }
        offset += rc;
    }

    if (!has_token && offset < (int)reason_size) {
        rc = snprintf(reason + offset, reason_size - (size_t)offset,
                      " VIDEO_UPLOADER_HTTP_AUTH_TOKEN");
        if (rc < 0 || (size_t)rc >= reason_size - (size_t)offset) {
            reason[reason_size - 1] = '\0';
            return 0;
        }
        offset += rc;
    }

    if (!has_device_id && offset < (int)reason_size) {
        rc = snprintf(reason + offset, reason_size - (size_t)offset,
                      " VIDEO_UPLOADER_HTTP_DEVICE_ID");
        if (rc < 0 || (size_t)rc >= reason_size - (size_t)offset) {
            reason[reason_size - 1] = '\0';
            return 0;
        }
    }

    return 0;
}

static void mark_stream_offline(StreamState *stream) {
    pid_t ret = 0;

    if (stream->parent_sock >= 0) {
        close(stream->parent_sock);
        stream->parent_sock = -1;
    }

    if (stream->child_pid > 0) {
        ret = waitpid(stream->child_pid, NULL, WNOHANG);
        if (ret == stream->child_pid) {
            stream->child_pid = -1;
        }
    }

    stream->stream_online = 0;
    stream->last_restart_mono_ms = mono_now_ms();
}

static void stop_stream_child(StreamState *stream) {
    if (stream->parent_sock >= 0) {
        close(stream->parent_sock);
        stream->parent_sock = -1;
    }

    if (stream->child_pid > 0) {
        kill(stream->child_pid, SIGTERM);
        waitpid(stream->child_pid, NULL, 0);
        stream->child_pid = -1;
    }

    stream->stream_online = 0;
}

static void child_reset_streamer(ChildOutputCtx *ctx) {
    memset(&ctx->streamer, 0, sizeof(ctx->streamer));
    ctx->streamer_ready = 0;
}

static int child_open_rtmp_output(ChildOutputCtx *ctx) {
    child_reset_streamer(ctx);

    if (streamer_init(&ctx->streamer, RTMP_URL, WIDTH, HEIGHT, 30) < 0) {
        fprintf(stderr, "[Child] Streamer init failed for RTMP\n");
        child_reset_streamer(ctx);
        return -1;
    }

    ctx->streamer_ready = 1;
    ctx->mode = CHILD_OUTPUT_RTMP;
    child_reset_rtmp_backoff(ctx);
    return 0;
}

static int child_close_file_segment(ChildOutputCtx *ctx, int broken) {
    int64_t end_ms;
    int64_t size_bytes = 0;
    int64_t bytes_after = 0;
    int deleted_count = 0;
    int rc = 0;

    if (ctx->mode != CHILD_OUTPUT_FILE || !ctx->streamer_ready) {
        return 0;
    }

    end_ms = wall_now_ms();
    streamer_clean(&ctx->streamer);
    child_reset_streamer(ctx);

    rc = video_store_get_file_size(ctx->current_segment_path, &size_bytes);
    if (rc != 0) {
        fprintf(stderr, "[Child] Failed to stat segment %s: %d\n",
                ctx->current_segment_path, rc);
        size_bytes = 0;
    }

    if (ctx->current_segment_id > 0) {
        if (broken) {
            rc = video_store_mark_segment_broken(&ctx->video_store,
                                                 ctx->current_segment_id,
                                                 end_ms,
                                                 size_bytes);
        } else {
            rc = video_store_finish_segment(&ctx->video_store,
                                            ctx->current_segment_id,
                                            end_ms,
                                            size_bytes);
        }
        if (rc != 0) {
            fprintf(stderr, "[Child] Failed to update segment metadata: %d\n", rc);
        }
    }

    rc = video_store_prune(&ctx->video_store, &bytes_after, &deleted_count);
    if (rc == 0 && deleted_count > 0) {
        printf("[Child] Video prune done, deleted=%d, bytes_after=%lld\n",
               deleted_count, (long long)bytes_after);
    }

    ctx->current_segment_id = 0;
    ctx->current_segment_start_wall_ms = 0;
    ctx->current_segment_start_mono_ms = 0;
    ctx->current_segment_path[0] = '\0';
    return 0;
}

static int child_open_file_segment(ChildOutputCtx *ctx,
                                   int64_t start_wall_ms,
                                   int64_t start_mono_ms) {
    int rc;
    int64_t segment_id = 0;
    char segment_path[PATH_MAX];

    if (!ctx->video_store_ready) {
        return -1;
    }

    rc = video_store_build_segment_path(&ctx->video_store,
                                        start_wall_ms,
                                        segment_path,
                                        sizeof(segment_path));
    if (rc != 0) {
        fprintf(stderr, "[Child] Failed to build segment path: %d\n", rc);
        return -1;
    }

    rc = video_store_begin_segment(&ctx->video_store,
                                   start_wall_ms,
                                   segment_path,
                                   &segment_id);
    if (rc != 0) {
        fprintf(stderr, "[Child] Failed to register segment: %d\n", rc);
        return -1;
    }

    child_reset_streamer(ctx);
    if (streamer_init(&ctx->streamer, segment_path, WIDTH, HEIGHT, 30) < 0) {
        fprintf(stderr, "[Child] Streamer init failed for local segment\n");
        local_store_delete_video_segment(&ctx->store, segment_id);
        child_reset_streamer(ctx);
        return -1;
    }

    ctx->streamer_ready = 1;
    ctx->mode = CHILD_OUTPUT_FILE;
    ctx->current_segment_id = segment_id;
    ctx->current_segment_start_wall_ms = start_wall_ms;
    ctx->current_segment_start_mono_ms = start_mono_ms;
    snprintf(ctx->current_segment_path, sizeof(ctx->current_segment_path), "%s", segment_path);

    printf("[Child] Local segment started: %s\n", ctx->current_segment_path);
    return 0;
}

static int child_switch_to_file_mode(ChildOutputCtx *ctx,
                                     int64_t frame_mono_ms,
                                     int64_t frame_wall_ms) {
    if (ctx->streamer_ready) {
        streamer_clean(&ctx->streamer);
        child_reset_streamer(ctx);
    }

    ctx->mode = CHILD_OUTPUT_FILE;
    ctx->last_rtmp_retry_mono_ms = frame_mono_ms;
    child_reset_rtmp_backoff(ctx);

    return child_open_file_segment(ctx, frame_wall_ms, frame_mono_ms);
}

static int child_rotate_or_restore_output(ChildOutputCtx *ctx,
                                          int64_t frame_mono_ms,
                                          int64_t frame_wall_ms) {
    int rc;

    if (ctx->mode != CHILD_OUTPUT_FILE || !ctx->streamer_ready) {
        return 0;
    }

    if (frame_mono_ms - ctx->current_segment_start_mono_ms < ctx->video_store.segment_duration_ms) {
        return 0;
    }

    child_close_file_segment(ctx, 0);

    if (frame_mono_ms - ctx->last_rtmp_retry_mono_ms >= ctx->rtmp_retry_backoff_ms) {
        ctx->last_rtmp_retry_mono_ms = frame_mono_ms;
        if (child_open_rtmp_output(ctx) == 0) {
            printf("[Child] RTMP output restored.\n");
            return 0;
        }

        child_increase_rtmp_backoff(ctx);
        printf("[Child] RTMP restore failed, next backoff=%d ms.\n",
               ctx->rtmp_retry_backoff_ms);
    }

    rc = child_open_file_segment(ctx, frame_wall_ms, frame_mono_ms);
    if (rc != 0) {
        fprintf(stderr, "[Child] Failed to open next local segment.\n");
        return -1;
    }

    return 0;
}

static int child_stream_loop(int sock) {
    ChildOutputCtx ctx;
    int64_t frame_mono_ms;
    int64_t frame_wall_ms;
    int child_fd;
    int ret;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    memset(&ctx, 0, sizeof(ctx));
    ctx.rtmp_retry_backoff_ms = CHILD_RTMP_RETRY_BASE_MS;
    ctx.rtmp_retry_max_ms = CHILD_RTMP_RETRY_MAX_MS;
    ctx.debug_rtmp_fail_after_frames = env_to_positive_int("CAMERA_FLOW_DEBUG_RTMP_FAIL_AFTER_FRAMES", 0);
    if (ctx.debug_rtmp_fail_after_frames > 0) {
        printf("[Child] Debug RTMP fail will trigger after %d frames.\n",
               ctx.debug_rtmp_fail_after_frames);
    }

    if (local_store_open(&ctx.store, NULL) == 0) {
        ctx.store_ready = 1;
        if (video_store_init(&ctx.video_store, &ctx.store, NULL) == 0) {
            ctx.video_store_ready = 1;
            printf("[Child] Video store ready, segment_ms=%lld, high_water=%lld, low_water=%lld\n",
                   (long long)ctx.video_store.segment_duration_ms,
                   (long long)ctx.video_store.high_water_bytes,
                   (long long)ctx.video_store.low_water_bytes);
        } else {
            fprintf(stderr, "[Child] video_store_init failed\n");
        }
    } else {
        fprintf(stderr, "[Child] local_store_open failed, file mode unavailable\n");
    }

    if (child_open_rtmp_output(&ctx) < 0) {
        fprintf(stderr, "[Child] RTMP init failed, switch to local file mode.\n");
        if (child_switch_to_file_mode(&ctx, mono_now_ms(), wall_now_ms()) < 0) {
            return -1;
        }
    }

    while (is_running && (child_fd = recv_fd(sock)) >= 0) {
        frame_mono_ms = mono_now_ms();
        frame_wall_ms = wall_now_ms();

        if (ctx.mode == CHILD_OUTPUT_FILE) {
            ret = child_rotate_or_restore_output(&ctx, frame_mono_ms, frame_wall_ms);
            if (ret < 0) {
                close(child_fd);
                break;
            }
        }

        if (ctx.mode == CHILD_OUTPUT_RTMP &&
            ctx.debug_rtmp_fail_after_frames > 0 &&
            !ctx.debug_rtmp_fail_triggered) {
            ctx.debug_rtmp_frame_count++;
            if (ctx.debug_rtmp_frame_count >= ctx.debug_rtmp_fail_after_frames) {
                ctx.debug_rtmp_fail_triggered = 1;
                ret = -1;
                fprintf(stderr, "[Child] Debug: simulate RTMP disconnect at frame %d.\n",
                        ctx.debug_rtmp_frame_count);
            } else {
                ret = streamer_push_zerocopy(&ctx.streamer, child_fd);
            }
        } else {
            ret = streamer_push_zerocopy(&ctx.streamer, child_fd);
        }

        if (ret < 0) {
            if (ctx.mode == CHILD_OUTPUT_RTMP && ctx.video_store_ready) {
                fprintf(stderr, "[Child] RTMP push failed, switch to local file mode.\n");
                if (child_switch_to_file_mode(&ctx, frame_mono_ms, frame_wall_ms) == 0) {
                    ret = streamer_push_zerocopy(&ctx.streamer, child_fd);
                    if (ret == 0) {
                        char ack = 'k';
                        if (write(sock, &ack, 1) <= 0) {
                            close(child_fd);
                            break;
                        }
                        close(child_fd);
                        continue;
                    }
                }
            }

            close(child_fd);
            fprintf(stderr, "[Child] Push failed, exit child loop.\n");
            break;
        }

        char ack = 'k';
        if (write(sock, &ack, 1) <= 0) {
            close(child_fd);
            break;
        }

        close(child_fd);
    }

    if (ctx.mode == CHILD_OUTPUT_FILE && ctx.streamer_ready) {
        child_close_file_segment(&ctx, 0);
    } else if (ctx.streamer_ready) {
        streamer_clean(&ctx.streamer);
        child_reset_streamer(&ctx);
    }

    if (ctx.store_ready) {
        local_store_close(&ctx.store);
    }

    return 0;
}

static int spawn_stream_child(StreamState *stream) {
    int sv[2];
    pid_t pid;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    if (pid == 0) {
        close(sv[0]);
        int ret = child_stream_loop(sv[1]);
        close(sv[1]);
        exit(ret == 0 ? 0 : 1);
    }

    close(sv[1]);
    stream->parent_sock = sv[0];
    stream->child_pid = pid;
    stream->stream_online = 1;
    reset_restart_backoff(stream);
    stream->last_restart_mono_ms = mono_now_ms();

    printf("[Parent] Stream child started: pid=%d\n", pid);
    return 0;
}

static void try_restart_stream_child(StreamState *stream) {
    int status;
    int64_t now;
    pid_t ret;

    if (stream->stream_online) {
        return;
    }

    if (stream->child_pid > 0) {
        ret = waitpid(stream->child_pid, &status, WNOHANG);
        if (ret == 0) {
            return;
        }
        if (ret == stream->child_pid) {
            stream->child_pid = -1;
        }
    }

    now = mono_now_ms();
    if (now - stream->last_restart_mono_ms < stream->current_restart_ms) {
        return;
    }

    stream->last_restart_mono_ms = now;
    printf("[Parent] Try restart stream child after %d ms backoff...\n",
           stream->current_restart_ms);

    if (spawn_stream_child(stream) < 0) {
        increase_restart_backoff(stream);
        printf("[Parent] Restart stream child failed, fail_count=%d, next backoff=%d ms.\n",
               stream->restart_fail_count,
               stream->current_restart_ms);
    }
}

int main(void) {
    CameraCtx cam;
    StreamState stream = {
        .stream_online = 0,
        .child_pid = -1,
        .parent_sock = -1,
        .base_restart_ms = STREAM_RESTART_BASE_MS,
        .max_restart_ms = STREAM_RESTART_MAX_MS,
        .current_restart_ms = STREAM_RESTART_BASE_MS,
        .restart_fail_count = 0,
        .last_restart_mono_ms = 0,
    };
    int shmid = -1;
    void *shmaddr = (void *)-1;
    int dma_fds[BUF_COUNT] = { -1, -1, -1, -1 };
    VideoUploader video_uploader;
    int video_uploader_started = 0;
    char video_uploader_reason[256];

    memset(&video_uploader, 0, sizeof(video_uploader));

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (camera_init(&cam, VIDEO_DEV, WIDTH, HEIGHT) < 0) {
        fprintf(stderr, "Failed to init camera\n");
        exit(1);
    }

    /* 保留这段共享内存初始化，后面如果有别的进程要复用帧数据还能继续接。 */
    shmid = shmget(IPC_PRIVATE, FRAME_SIZE, IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget");
        camera_deinit(&cam);
        exit(1);
    }

    shmaddr = shmat(shmid, NULL, 0);
    if (shmaddr == (void *)-1) {
        perror("shmat");
        camera_deinit(&cam);
        exit(1);
    }

    for (int i = 0; i < BUF_COUNT; i++) {
        struct v4l2_exportbuffer expbuf;
        memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type = cam.buf_type;
        expbuf.index = i;

        if (ioctl(cam.fd, VIDIOC_EXPBUF, &expbuf) < 0) {
            perror("[Parent] VIDIOC_EXPBUF failed (Does your driver support DMA-BUF?)");
            goto cleanup;
        }
        dma_fds[i] = expbuf.fd;
    }

    if (camera_start(&cam) < 0) {
        perror("camera start failed");
        goto cleanup;
    }

    {
        const char *sensor_device = getenv(SENSOR_DEVICE_ENV);
        if (sensor_device == NULL || sensor_device[0] == '\0') {
            sensor_device = SENSOR_DEVICE_DEFAULT;
        }

        int rc = start_sensor_collector(sensor_device);
        if (rc != 0) {
            fprintf(stderr, "[Parent] Failed to start sensor collector on %s: %s\n",
                    sensor_device, strerror(rc));
        } else {
            printf("[Parent] Sensor collector started on %s\n", sensor_device);
        }
    }

    {
        int rc = spawn_stream_child(&stream);
        if (rc < 0) {
            printf("[Parent] Initial stream child start failed, continue in offline mode.\n");
        }
    }

    {
        int rc = start_mqtt_reporter();
        if (rc != 0) {
            fprintf(stderr, "[Parent] Failed to start MQTT reporter: %s\n", strerror(rc));
        } else {
            printf("[Parent] MQTT reporter started\n");
        }
    }

    if (video_uploader_macro_ready(video_uploader_reason, sizeof(video_uploader_reason))) {
        int rc = video_uploader_start(&video_uploader,
                                      NULL,
                                      video_uploader_http_upload_callback,
                                      NULL);
        if (rc != 0) {
            fprintf(stderr, "[Parent] Failed to start video uploader: %s\n", strerror(rc));
        } else {
            video_uploader_started = 1;
            printf("[Parent] Video uploader started\n");
        }
    } else {
        printf("[Parent] Video uploader skipped: %s\n", video_uploader_reason);
    }

    while (is_running) {
        struct v4l2_buffer qbuf;
        struct v4l2_plane qplanes[1];
        int current_dma_fd;

        memset(&qbuf, 0, sizeof(qbuf));
        memset(qplanes, 0, sizeof(qplanes));
        qbuf.type = cam.buf_type;
        qbuf.memory = V4L2_MEMORY_MMAP;
        qbuf.m.planes = qplanes;
        qbuf.length = 1;

        if (ioctl(cam.fd, VIDIOC_DQBUF, &qbuf) < 0) {
            if (errno == EINTR) {
                break;
            }
            perror("camera dequeue buf");
            break;
        }

        current_dma_fd = dma_fds[qbuf.index];

        if (stream.stream_online) {
            if (send_fd(stream.parent_sock, current_dma_fd) < 0) {
                fprintf(stderr, "[Parent] send_fd failed, mark stream offline.\n");
                mark_stream_offline(&stream);
            } else {
                char ack;
                if (read(stream.parent_sock, &ack, 1) <= 0) {
                    fprintf(stderr, "[Parent] ACK failed, mark stream offline.\n");
                    mark_stream_offline(&stream);
                }
            }
        }

        /* 推流失败不应该拖死采集主循环，这一帧必须回给驱动。 */
        if (ioctl(cam.fd, VIDIOC_QBUF, &qbuf) < 0) {
            perror("[Parent] camera queue buf");
            break;
        }

        if (!stream.stream_online) {
            try_restart_stream_child(&stream);
        }
    }

cleanup:
    is_running = 0;
    stop_mqtt_reporter();
    stop_stream_child(&stream);
    if (video_uploader_started) {
        video_uploader_stop(&video_uploader);
    }

    camera_stop(&cam);
    camera_deinit(&cam);

    for (int i = 0; i < BUF_COUNT; i++) {
        if (dma_fds[i] >= 0) {
            close(dma_fds[i]);
        }
    }

    if (shmaddr && shmaddr != (void *)-1) {
        shmdt(shmaddr);
    }
    if (shmid >= 0) {
        if (shmctl(shmid, IPC_RMID, NULL) < 0) {
            perror("shmctl IPC_RMID failed");
        }
    }

    return 0;
}
