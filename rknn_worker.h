#ifndef RKNN_WORKER_H
#define RKNN_WORKER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include "detect_shared.h"
#include "rknn_api.h"

#define RKNN_WORKER_DEFAULT_MODEL_PATH "./22/model/best.rknn"

typedef struct {
    const char *model_path;
    rknn_core_mask core_mask;
    uint32_t init_flags;
    int want_float_outputs;
    float conf_threshold;
    float nms_threshold;
    int camera_width;
    int camera_height;
    DetectSharedState *detect_state;
} RknnWorkerConfig;

typedef struct {
    uint32_t n_dims;
    uint32_t dims[RKNN_MAX_DIMS];
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t size;
    rknn_tensor_format fmt;
    rknn_tensor_type type;
} RknnWorkerInputInfo;

typedef struct {
    uint64_t submitted;
    uint64_t processed;
    uint64_t dropped;
    uint64_t failed;
    uint64_t decoded_candidates;
    int64_t last_frame_seq;
    int64_t last_timestamp_ms;
    int64_t last_infer_us;
    int last_error;
} RknnWorkerStats;

typedef struct {
    int64_t frame_seq;
    int64_t timestamp_ms;
    size_t size;
} RknnWorkerTaskMeta;

typedef struct {
    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int lock_ready;
    int cond_ready;
    int thread_started;
    volatile sig_atomic_t stopping;
    int ready;

    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    size_t input_size;

    unsigned char *pending_input;
    unsigned char *work_input;
    int pending_valid;
    RknnWorkerTaskMeta pending_meta;

    int want_float_outputs;
    float conf_threshold;
    float nms_threshold;
    int camera_width;
    int camera_height;
    DetectSharedState *detect_state;
    RknnWorkerInputInfo input_info;
    RknnWorkerStats stats;
} RknnWorker;

void rknn_worker_config_defaults(RknnWorkerConfig *config);
int rknn_worker_start(RknnWorker *worker, const RknnWorkerConfig *config);
void rknn_worker_request_stop(RknnWorker *worker);
void rknn_worker_stop(RknnWorker *worker);
int rknn_worker_submit(RknnWorker *worker,
                       const void *input_data,
                       size_t input_size,
                       int64_t frame_seq,
                       int64_t timestamp_ms);
int rknn_worker_get_input_info(RknnWorker *worker, RknnWorkerInputInfo *info);
void rknn_worker_get_stats(RknnWorker *worker, RknnWorkerStats *stats);
int rknn_worker_is_ready(RknnWorker *worker);

#endif
