#include "camera.h"
#include "ipc.h"
#include "encoder.h"
#include "sensor_modbus.h"
#include "aliyun_mqtt.h"
#include "video_store.h"
#include "video_uploader.h"
#include "rknn_worker.h"
#include "relay_alarm.h"
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
#include <RgaApi.h>
#include <im2d.h>

#define VIDEO_DEV "/dev/video11"
#define RTMP_URL "rtmp://192.168.31.121/live/test"
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
#define AI_ENABLE_ENV "CAMERA_FLOW_AI_ENABLE"
#define AI_MODEL_PATH_ENV "CAMERA_FLOW_AI_MODEL_PATH"
#define AI_INTERVAL_ENV "CAMERA_FLOW_AI_INTERVAL"
#define AI_CONF_ENV "CAMERA_FLOW_AI_CONF"
#define AI_NMS_ENV "CAMERA_FLOW_AI_NMS"
#define AI_STATS_INTERVAL_MS 5000
#define ALARM_FUSION_EVAL_MS 200
#define ALARM_FUSION_ON_CONFIRM 3
#define ALARM_FUSION_OFF_CONFIRM 5
#define ALARM_FUSION_AI_STALE_MS 1500

volatile sig_atomic_t is_running = 1;
static volatile sig_atomic_t g_video_uploader_signal_ready = 0;
static VideoUploader *g_video_uploader_for_signal = NULL;

typedef struct {
    int stream_online;
    pid_t child_pid;
    int parent_sock;
    DetectSharedState *detect_state;
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

    DetectSharedState *detect_state;
    DetectSharedState detect_snapshot;
} ChildOutputCtx;

typedef struct {
    RknnWorker worker;
    int started;
    int enabled;
    int frame_interval;
    uint64_t frame_seq;
    unsigned char *input_buf;
    size_t input_size;
    RknnWorkerInputInfo input_info;
    int64_t last_stats_log_ms;
    uint64_t preproc_fail_count;
    DetectSharedState *detect_state;
} AiPipeline;

typedef enum {
    ALARM_FUSION_OFF = 0,
    ALARM_FUSION_ON = 1
} AlarmFusionState;

typedef struct {
    AlarmFusionState state;
    int on_count;
    int off_count;
    int relay_level;
    int64_t last_eval_ms;
} AlarmFusion;

static void sig_handler(int sig) {
    (void)sig;
    is_running = 0;
    if (g_video_uploader_signal_ready) {
        video_uploader_request_stop(g_video_uploader_for_signal);
    }
}

static void install_signal_handlers(void) {
    struct sigaction action;
    struct sigaction ignore_action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = sig_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    memset(&ignore_action, 0, sizeof(ignore_action));
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    sigaction(SIGPIPE, &ignore_action, NULL);
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

static int env_to_bool_default(const char *name, int fallback) {
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "FALSE") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "OFF") == 0 ||
        strcmp(value, "no") == 0 ||
        strcmp(value, "NO") == 0) {
        return 0;
    }

    return 1;
}

static float env_to_float_range(const char *name, float fallback, float min_value, float max_value) {
    const char *value = getenv(name);
    char *endptr = NULL;
    float parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    errno = 0;
    parsed = strtof(value, &endptr);
    if (errno != 0 || endptr == value || *endptr != '\0' ||
        parsed < min_value || parsed > max_value) {
        return fallback;
    }

    return parsed;
}

static int ai_preprocess_dma_to_rgb(int dma_fd,
                                    const RknnWorkerInputInfo *input_info,
                                    unsigned char *dst_data) {
    rga_buffer_t src;
    rga_buffer_t dst;
    im_rect src_rect;
    im_rect dst_rect;
    IM_STATUS status;
    uint32_t dst_width;
    uint32_t dst_height;
    uint32_t resized_width;
    uint32_t resized_height;
    uint32_t left;
    uint32_t top;

    if (dma_fd < 0 || input_info == NULL || dst_data == NULL ||
        input_info->width == 0 || input_info->height == 0 ||
        input_info->channels != 3) {
        return -1;
    }

    dst_width = input_info->width;
    dst_height = input_info->height;
    if ((uint64_t)dst_width * HEIGHT <= (uint64_t)dst_height * WIDTH) {
        resized_width = dst_width;
        resized_height = (uint32_t)(((uint64_t)HEIGHT * dst_width) / WIDTH);
    } else {
        resized_height = dst_height;
        resized_width = (uint32_t)(((uint64_t)WIDTH * dst_height) / HEIGHT);
    }
    if (resized_width == 0 || resized_height == 0) {
        return -1;
    }

    memset(dst_data, 114, input_info->size);

    src = wrapbuffer_fd(dma_fd, WIDTH, HEIGHT, RK_FORMAT_YCbCr_420_SP);
    src.wstride = WIDTH;
    src.hstride = HEIGHT;

    dst = wrapbuffer_virtualaddr(dst_data,
                                 (int)dst_width,
                                 (int)dst_height,
                                 RK_FORMAT_RGB_888);
    dst.wstride = (int)dst_width;
    dst.hstride = (int)dst_height;

    src_rect = (im_rect){0, 0, WIDTH, HEIGHT};
    left = (dst_width - resized_width) / 2;
    top = (dst_height - resized_height) / 2;
    dst_rect = (im_rect){(int)left, (int)top, (int)resized_width, (int)resized_height};

    status = improcess(src, dst, (rga_buffer_t){0},
                       src_rect, dst_rect, (im_rect){0}, IM_SYNC);
    if (status != IM_STATUS_SUCCESS) {
        return -1;
    }

    return 0;
}

static int ai_pipeline_start(AiPipeline *ai, DetectSharedState *detect_state) {
    RknnWorkerConfig config;
    const char *model_path;
    int rc;

    if (ai == NULL) {
        return EINVAL;
    }

    memset(ai, 0, sizeof(*ai));
    ai->detect_state = detect_state;
    ai->enabled = env_to_bool_default(AI_ENABLE_ENV, 1);
    ai->frame_interval = env_to_positive_int(AI_INTERVAL_ENV, 1);
    ai->last_stats_log_ms = mono_now_ms();

    if (!ai->enabled) {
        printf("[Parent][AI] RKNN worker disabled by %s\n", AI_ENABLE_ENV);
        return 0;
    }

    rknn_worker_config_defaults(&config);
    model_path = getenv(AI_MODEL_PATH_ENV);
    if (model_path != NULL && model_path[0] != '\0') {
        config.model_path = model_path;
    }
    config.camera_width = WIDTH;
    config.camera_height = HEIGHT;
    config.detect_state = detect_state;
    config.conf_threshold = env_to_float_range(AI_CONF_ENV, config.conf_threshold, 0.01f, 0.99f);
    config.nms_threshold = env_to_float_range(AI_NMS_ENV, config.nms_threshold, 0.01f, 0.99f);

    rc = rknn_worker_start(&ai->worker, &config);
    if (rc != 0) {
        fprintf(stderr, "[Parent][AI] RKNN worker start failed: %s\n", strerror(rc));
        ai->enabled = 0;
        return rc;
    }

    rc = rknn_worker_get_input_info(&ai->worker, &ai->input_info);
    if (rc != 0) {
        fprintf(stderr, "[Parent][AI] RKNN input info unavailable: %s\n", strerror(rc));
        rknn_worker_stop(&ai->worker);
        ai->enabled = 0;
        return rc;
    }

    if (ai->input_info.width == 0 ||
        ai->input_info.height == 0 ||
        ai->input_info.channels != 3 ||
        ai->input_info.size == 0) {
        fprintf(stderr,
                "[Parent][AI] Unsupported RKNN input shape for RGA preprocess: %ux%ux%u size=%u\n",
                ai->input_info.width,
                ai->input_info.height,
                ai->input_info.channels,
                ai->input_info.size);
        rknn_worker_stop(&ai->worker);
        ai->enabled = 0;
        return EINVAL;
    }

    ai->input_size = ai->input_info.size;
    ai->input_buf = (unsigned char *)malloc(ai->input_size);
    if (ai->input_buf == NULL) {
        fprintf(stderr, "[Parent][AI] input buffer alloc failed, size=%zu\n", ai->input_size);
        rknn_worker_stop(&ai->worker);
        ai->enabled = 0;
        return ENOMEM;
    }

    ai->started = 1;
    printf("[Parent][AI] RKNN worker ready, input=%ux%ux%u interval=%d model=%s\n",
           ai->input_info.width,
           ai->input_info.height,
           ai->input_info.channels,
           ai->frame_interval,
           config.model_path);
    return 0;
}

static void ai_pipeline_process_frame(AiPipeline *ai, int dma_fd, int64_t frame_wall_ms) {
    int rc;

    if (ai == NULL || !ai->started || !rknn_worker_is_ready(&ai->worker)) {
        return;
    }

    ai->frame_seq++;
    if (ai->frame_interval > 1 &&
        (ai->frame_seq % (uint64_t)ai->frame_interval) != 0) {
        return;
    }

    rc = ai_preprocess_dma_to_rgb(dma_fd, &ai->input_info, ai->input_buf);
    if (rc != 0) {
        ai->preproc_fail_count++;
        if (ai->preproc_fail_count == 1 || (ai->preproc_fail_count % 100) == 0) {
            fprintf(stderr, "[Parent][AI] preprocess failed count=%llu\n",
                    (unsigned long long)ai->preproc_fail_count);
        }
        return;
    }

    rc = rknn_worker_submit(&ai->worker,
                            ai->input_buf,
                            ai->input_size,
                            (int64_t)ai->frame_seq,
                            frame_wall_ms);
    if (rc != 0 && rc != ECANCELED) {
        fprintf(stderr, "[Parent][AI] submit failed: %s\n", strerror(rc));
    }
}

static void ai_pipeline_log_stats(AiPipeline *ai, int64_t now_ms) {
    RknnWorkerStats stats;
    int box_count = 0;

    if (ai == NULL || !ai->started) {
        return;
    }
    if (now_ms - ai->last_stats_log_ms < AI_STATS_INTERVAL_MS) {
        return;
    }

    memset(&stats, 0, sizeof(stats));
    rknn_worker_get_stats(&ai->worker, &stats);
    if (ai->detect_state != NULL && ai->detect_state->valid) {
        box_count = ai->detect_state->box_count;
    }
    printf("[Parent][AI] stats submitted=%llu processed=%llu dropped=%llu failed=%llu candidates=%llu boxes=%d last_us=%lld\n",
           (unsigned long long)stats.submitted,
           (unsigned long long)stats.processed,
           (unsigned long long)stats.dropped,
           (unsigned long long)stats.failed,
           (unsigned long long)stats.decoded_candidates,
           box_count,
           (long long)stats.last_infer_us);
    ai->last_stats_log_ms = now_ms;
}

static void ai_pipeline_stop(AiPipeline *ai) {
    if (ai == NULL) {
        return;
    }

    if (ai->started) {
        rknn_worker_stop(&ai->worker);
        ai->started = 0;
    }
    free(ai->input_buf);
    ai->input_buf = NULL;
    ai->input_size = 0;
}

static int detect_snapshot_read(DetectSharedState *shared, DetectSharedState *snapshot) {
    uint32_t before;
    uint32_t after;
    int tries;

    if (shared == NULL || snapshot == NULL || !shared->valid) {
        return 0;
    }

    for (tries = 0; tries < 3; tries++) {
        before = shared->version;
        if ((before & 1U) != 0U) {
            continue;
        }
        __sync_synchronize();
        memcpy(snapshot, shared, sizeof(*snapshot));
        __sync_synchronize();
        after = shared->version;
        if (before == after && (after & 1U) == 0U && snapshot->valid) {
            return snapshot->box_count > 0;
        }
    }

    return 0;
}

static int alarm_fusion_read_sensor_alarm(void) {
    int alarm;

    pthread_mutex_lock(&g_sensor_data.lock);
    alarm = g_sensor_data.alarm_status != 0;
    pthread_mutex_unlock(&g_sensor_data.lock);
    return alarm;
}

static int alarm_fusion_read_ai_alarm(DetectSharedState *shared, int64_t now_ms) {
    DetectSharedState snapshot;

    if (!detect_snapshot_read(shared, &snapshot)) {
        return 0;
    }
    if (snapshot.timestamp_ms <= 0 ||
        now_ms - snapshot.timestamp_ms > ALARM_FUSION_AI_STALE_MS) {
        return 0;
    }
    return snapshot.box_count > 0;
}

static void alarm_fusion_set_relay(AlarmFusion *fusion, int alarm_on) {
    int rc;

    if (fusion == NULL || fusion->relay_level == alarm_on) {
        return;
    }

    rc = relay_alarm_set(alarm_on);
    if (rc == 0) {
        fusion->relay_level = alarm_on;
        printf("[Parent][Alarm] relay %s\n", alarm_on ? "ON" : "OFF");
    } else {
        fprintf(stderr, "[Parent][Alarm] relay %s failed\n", alarm_on ? "ON" : "OFF");
    }
}

static void alarm_fusion_update(AlarmFusion *fusion,
                                DetectSharedState *detect_state,
                                int64_t now_ms) {
    int sensor_alarm;
    int ai_alarm;
    int fused_alarm;

    if (fusion == NULL) {
        return;
    }
    if (fusion->last_eval_ms > 0 &&
        now_ms - fusion->last_eval_ms < ALARM_FUSION_EVAL_MS) {
        return;
    }
    fusion->last_eval_ms = now_ms;

    sensor_alarm = alarm_fusion_read_sensor_alarm();
    ai_alarm = alarm_fusion_read_ai_alarm(detect_state, now_ms);
    fused_alarm = sensor_alarm && ai_alarm;

    if (fusion->state == ALARM_FUSION_OFF) {
        if (fused_alarm) {
            fusion->on_count++;
            fusion->off_count = 0;
            if (fusion->on_count >= ALARM_FUSION_ON_CONFIRM) {
                fusion->state = ALARM_FUSION_ON;
                fusion->on_count = 0;
                alarm_fusion_set_relay(fusion, 1);
            }
        } else {
            fusion->on_count = 0;
        }
    } else {
        if (fused_alarm) {
            fusion->off_count = 0;
        } else {
            fusion->off_count++;
            fusion->on_count = 0;
            if (fusion->off_count >= ALARM_FUSION_OFF_CONFIRM) {
                fusion->state = ALARM_FUSION_OFF;
                fusion->off_count = 0;
                alarm_fusion_set_relay(fusion, 0);
            }
        }
    }
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

static int child_stream_loop(int sock, DetectSharedState *detect_state) {
    ChildOutputCtx ctx;
    int64_t frame_mono_ms;
    int64_t frame_wall_ms;
    int child_fd;
    int ret;

    install_signal_handlers();

    memset(&ctx, 0, sizeof(ctx));
    ctx.detect_state = detect_state;
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
        const DetectSharedState *overlay = NULL;

        frame_mono_ms = mono_now_ms();
        frame_wall_ms = wall_now_ms();
        if (detect_snapshot_read(ctx.detect_state, &ctx.detect_snapshot)) {
            overlay = &ctx.detect_snapshot;
        }

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
                ret = streamer_push_zerocopy_overlay(&ctx.streamer, child_fd, overlay);
            }
        } else {
            ret = streamer_push_zerocopy_overlay(&ctx.streamer, child_fd, overlay);
        }

        if (ret < 0) {
            if (ctx.mode == CHILD_OUTPUT_RTMP && ctx.video_store_ready) {
                fprintf(stderr, "[Child] RTMP push failed, switch to local file mode.\n");
                if (child_switch_to_file_mode(&ctx, frame_mono_ms, frame_wall_ms) == 0) {
                    ret = streamer_push_zerocopy_overlay(&ctx.streamer, child_fd, overlay);
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
        g_video_uploader_for_signal = NULL;
        g_video_uploader_signal_ready = 0;
        int ret = child_stream_loop(sv[1], stream->detect_state);
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
        .detect_state = NULL,
        .base_restart_ms = STREAM_RESTART_BASE_MS,
        .max_restart_ms = STREAM_RESTART_MAX_MS,
        .current_restart_ms = STREAM_RESTART_BASE_MS,
        .restart_fail_count = 0,
        .last_restart_mono_ms = 0,
    };
    int shmid = -1;
    void *shmaddr = (void *)-1;
    DetectSharedState *detect_state = NULL;
    int dma_fds[BUF_COUNT] = { -1, -1, -1, -1 };
    VideoUploader video_uploader;
    AiPipeline ai_pipeline;
    AlarmFusion alarm_fusion;
    int video_uploader_started = 0;
    char video_uploader_reason[256];

    memset(&video_uploader, 0, sizeof(video_uploader));
    memset(&ai_pipeline, 0, sizeof(ai_pipeline));
    memset(&alarm_fusion, 0, sizeof(alarm_fusion));

    install_signal_handlers();

    detect_state = mmap(NULL,
                        sizeof(*detect_state),
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS,
                        -1,
                        0);
    if (detect_state == MAP_FAILED) {
        perror("detect mmap");
        detect_state = NULL;
    } else {
        memset(detect_state, 0, sizeof(*detect_state));
        stream.detect_state = detect_state;
    }

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

    if (ai_pipeline_start(&ai_pipeline, detect_state) != 0) {
        printf("[Parent][AI] Continue without RKNN inference.\n");
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
                                      &video_uploader);
        if (rc != 0) {
            fprintf(stderr, "[Parent] Failed to start video uploader: %s\n", strerror(rc));
        } else {
            video_uploader_started = 1;
            g_video_uploader_for_signal = &video_uploader;
            g_video_uploader_signal_ready = 1;
            printf("[Parent] Video uploader started\n");
        }
    } else {
        printf("[Parent] Video uploader skipped: %s\n", video_uploader_reason);
    }

    while (is_running) {
        struct v4l2_buffer qbuf;
        struct v4l2_plane qplanes[1];
        int current_dma_fd;
        int64_t frame_wall_ms;
        int64_t frame_mono_ms;

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
        frame_wall_ms = wall_now_ms();
        frame_mono_ms = mono_now_ms();

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

        ai_pipeline_process_frame(&ai_pipeline, current_dma_fd, frame_wall_ms);
        ai_pipeline_log_stats(&ai_pipeline, frame_mono_ms);
        alarm_fusion_update(&alarm_fusion, detect_state, frame_wall_ms);

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
    if (video_uploader_started) {
        video_uploader_request_stop(&video_uploader);
    }
    g_video_uploader_signal_ready = 0;
    ai_pipeline_stop(&ai_pipeline);
    stop_mqtt_reporter();
    stop_stream_child(&stream);
    if (video_uploader_started) {
        video_uploader_stop(&video_uploader);
    }
    g_video_uploader_for_signal = NULL;
    relay_alarm_off();

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
    if (detect_state != NULL) {
        munmap(detect_state, sizeof(*detect_state));
    }

    return 0;
}
