#include "rknn_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int64_t rknn_worker_now_us(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static float rknn_worker_sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static float rknn_worker_dequant_value(const rknn_tensor_attr *attr, int32_t value) {
    if (attr->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
        return ((float)value - (float)attr->zp) * attr->scale;
    }
    if (attr->qnt_type == RKNN_TENSOR_QNT_DFP && attr->fl >= 0 && attr->fl < 31) {
        return (float)value / (float)(1 << attr->fl);
    }
    return (float)value;
}

static float rknn_worker_output_value(const rknn_tensor_attr *attr,
                                      const rknn_output *output,
                                      uint32_t index,
                                      int *ok) {
    if (attr == NULL || output == NULL || output->buf == NULL ||
        index >= attr->n_elems || ok == NULL) {
        if (ok != NULL) {
            *ok = 0;
        }
        return 0.0f;
    }

    *ok = 1;
    if (output->size >= attr->n_elems * sizeof(float)) {
        return ((const float *)output->buf)[index];
    }

    switch (attr->type) {
    case RKNN_TENSOR_INT8:
        return rknn_worker_dequant_value(attr, ((const int8_t *)output->buf)[index]);
    case RKNN_TENSOR_UINT8:
        return rknn_worker_dequant_value(attr, ((const uint8_t *)output->buf)[index]);
    case RKNN_TENSOR_FLOAT32:
        if (output->size == 0 || output->size >= attr->n_elems * sizeof(float)) {
            return ((const float *)output->buf)[index];
        }
        break;
    default:
        break;
    }

    *ok = 0;
    return 0.0f;
}

static float rknn_worker_box_iou(const DetectBox *a, const DetectBox *b) {
    float inter_x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    float inter_y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    float inter_x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    float inter_y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    float inter_w = inter_x2 - inter_x1;
    float inter_h = inter_y2 - inter_y1;
    float inter_area;
    float area_a;
    float area_b;

    if (inter_w <= 0.0f || inter_h <= 0.0f) {
        return 0.0f;
    }

    inter_area = inter_w * inter_h;
    area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    if (area_a <= 0.0f || area_b <= 0.0f) {
        return 0.0f;
    }

    return inter_area / (area_a + area_b - inter_area);
}

static void rknn_worker_sort_boxes(DetectBox *boxes, int count) {
    int i;

    for (i = 1; i < count; i++) {
        DetectBox key = boxes[i];
        int j = i - 1;

        while (j >= 0 && boxes[j].score < key.score) {
            boxes[j + 1] = boxes[j];
            j--;
        }
        boxes[j + 1] = key;
    }
}

static void rknn_worker_publish_boxes(RknnWorker *worker,
                                      const RknnWorkerTaskMeta *meta,
                                      DetectBox *boxes,
                                      int count) {
    DetectSharedState *state = worker->detect_state;
    int i;

    if (state == NULL) {
        return;
    }

    if (count > DETECT_MAX_BOXES) {
        count = DETECT_MAX_BOXES;
    }

    state->version++;
    __sync_synchronize();
    state->valid = 0;
    state->frame_seq = meta->frame_seq;
    state->timestamp_ms = meta->timestamp_ms;
    state->box_count = count;
    for (i = 0; i < count; i++) {
        state->boxes[i] = boxes[i];
    }
    __sync_synchronize();
    state->valid = 1;
    __sync_synchronize();
    state->version++;
}

static int rknn_worker_decode_yolov8(RknnWorker *worker,
                                     const RknnWorkerTaskMeta *meta,
                                     rknn_output *outputs) {
    rknn_tensor_attr *attr;
    rknn_output *output;
    uint32_t n_elems;
    int dims0;
    int dims1;
    int rows;
    int attrs;
    int transposed = 0;
    int class_count;
    int row;
    int box_count = 0;
    DetectBox candidates[DETECT_MAX_BOXES * 4];
    DetectBox final_boxes[DETECT_MAX_BOXES];
    int final_count = 0;
    float gain;
    float resized_w;
    float resized_h;
    float pad_x;
    float pad_y;
    float frame_best_score = -FLT_MAX;
    int frame_best_class = -1;
    int frame_best_row = -1;
    float frame_best_cx = 0.0f;
    float frame_best_cy = 0.0f;
    float frame_best_w = 0.0f;
    float frame_best_h = 0.0f;

    if (worker->io_num.n_output < 1 || outputs == NULL || outputs[0].buf == NULL) {
        return -1;
    }

    attr = &worker->output_attrs[0];
    output = &outputs[0];
    n_elems = attr->n_elems;
    if (attr->n_dims < 2 || n_elems == 0) {
        return -1;
    }

    dims0 = (int)attr->dims[attr->n_dims - 2];
    dims1 = (int)attr->dims[attr->n_dims - 1];
    if (dims0 <= 0 || dims1 <= 0 || (uint32_t)(dims0 * dims1) > n_elems) {
        return -1;
    }

    if (dims1 >= 5 && dims1 <= 256) {
        rows = dims0;
        attrs = dims1;
    } else if (dims0 >= 5 && dims0 <= 256) {
        rows = dims1;
        attrs = dims0;
        transposed = 1;
    } else {
        return -1;
    }

    class_count = attrs - 4;
    if (class_count <= 0 || worker->camera_width <= 0 || worker->camera_height <= 0) {
        return -1;
    }

    gain = (float)worker->input_info.width / (float)worker->camera_width;
    if (((float)worker->camera_height * gain) > (float)worker->input_info.height) {
        gain = (float)worker->input_info.height / (float)worker->camera_height;
    }
    resized_w = (float)worker->camera_width * gain;
    resized_h = (float)worker->camera_height * gain;
    pad_x = ((float)worker->input_info.width - resized_w) * 0.5f;
    pad_y = ((float)worker->input_info.height - resized_h) * 0.5f;

    for (row = 0; row < rows; row++) {
        float cx;
        float cy;
        float w;
        float h;
        float best_score = -FLT_MAX;
        int best_class = -1;
        int c;
        float x1;
        float y1;
        float x2;
        float y2;
        int ok = 1;

        if (!transposed) {
            cx = rknn_worker_output_value(attr, output, (uint32_t)(row * attrs + 0), &ok);
            if (!ok) return -1;
            cy = rknn_worker_output_value(attr, output, (uint32_t)(row * attrs + 1), &ok);
            if (!ok) return -1;
            w = rknn_worker_output_value(attr, output, (uint32_t)(row * attrs + 2), &ok);
            if (!ok) return -1;
            h = rknn_worker_output_value(attr, output, (uint32_t)(row * attrs + 3), &ok);
            if (!ok) return -1;
            for (c = 0; c < class_count; c++) {
                float score = rknn_worker_output_value(attr,
                                                       output,
                                                       (uint32_t)(row * attrs + 4 + c),
                                                       &ok);
                if (!ok) return -1;
                if (score > best_score) {
                    best_score = score;
                    best_class = c;
                }
            }
        } else {
            cx = rknn_worker_output_value(attr, output, (uint32_t)(0 * rows + row), &ok);
            if (!ok) return -1;
            cy = rknn_worker_output_value(attr, output, (uint32_t)(1 * rows + row), &ok);
            if (!ok) return -1;
            w = rknn_worker_output_value(attr, output, (uint32_t)(2 * rows + row), &ok);
            if (!ok) return -1;
            h = rknn_worker_output_value(attr, output, (uint32_t)(3 * rows + row), &ok);
            if (!ok) return -1;
            for (c = 0; c < class_count; c++) {
                float score = rknn_worker_output_value(attr,
                                                       output,
                                                       (uint32_t)((4 + c) * rows + row),
                                                       &ok);
                if (!ok) return -1;
                if (score > best_score) {
                    best_score = score;
                    best_class = c;
                }
            }
        }

        if (best_class >= 0 && best_score > frame_best_score) {
            frame_best_score = best_score;
            frame_best_class = best_class;
            frame_best_row = row;
            frame_best_cx = cx;
            frame_best_cy = cy;
            frame_best_w = w;
            frame_best_h = h;
        }

        if (best_score < 0.0f || best_score > 1.0f) {
            best_score = rknn_worker_sigmoid(best_score);
        }
        if (best_score < worker->conf_threshold || best_class < 0) {
            continue;
        }

        x1 = (cx - w * 0.5f - pad_x) / gain;
        y1 = (cy - h * 0.5f - pad_y) / gain;
        x2 = (cx + w * 0.5f - pad_x) / gain;
        y2 = (cy + h * 0.5f - pad_y) / gain;

        if (x1 < 0.0f) x1 = 0.0f;
        if (y1 < 0.0f) y1 = 0.0f;
        if (x2 > (float)(worker->camera_width - 1)) x2 = (float)(worker->camera_width - 1);
        if (y2 > (float)(worker->camera_height - 1)) y2 = (float)(worker->camera_height - 1);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        if (box_count < (int)(sizeof(candidates) / sizeof(candidates[0]))) {
            candidates[box_count].x1 = x1;
            candidates[box_count].y1 = y1;
            candidates[box_count].x2 = x2;
            candidates[box_count].y2 = y2;
            candidates[box_count].score = best_score;
            candidates[box_count].class_id = best_class;
            box_count++;
        }
    }

    rknn_worker_sort_boxes(candidates, box_count);
    for (row = 0; row < box_count && final_count < DETECT_MAX_BOXES; row++) {
        int keep = 1;
        int j;

        for (j = 0; j < final_count; j++) {
            if (candidates[row].class_id == final_boxes[j].class_id &&
                rknn_worker_box_iou(&candidates[row], &final_boxes[j]) > worker->nms_threshold) {
                keep = 0;
                break;
            }
        }

        if (keep) {
            final_boxes[final_count++] = candidates[row];
        }
    }

    rknn_worker_publish_boxes(worker, meta, final_boxes, final_count);
    if (meta->frame_seq <= 3 || (meta->frame_seq % 60) == 0) {
        printf("[RKNN][debug] frame=%lld out_size=%u elem=%u dims=[%u,%u,%u] type=%s want_float=%d rows=%d attrs=%d best_raw=%f best_cls=%d best_row=%d box=(%f,%f,%f,%f) candidates=%d final=%d\n",
               (long long)meta->frame_seq,
               output->size,
               attr->n_elems,
               attr->n_dims > 0 ? attr->dims[0] : 0,
               attr->n_dims > 1 ? attr->dims[1] : 0,
               attr->n_dims > 2 ? attr->dims[2] : 0,
               get_type_string(attr->type),
               worker->want_float_outputs,
               rows,
               attrs,
               frame_best_score,
               frame_best_class,
               frame_best_row,
               frame_best_cx,
               frame_best_cy,
               frame_best_w,
               frame_best_h,
               box_count,
               final_count);
    }
    pthread_mutex_lock(&worker->lock);
    worker->stats.decoded_candidates += (uint64_t)box_count;
    pthread_mutex_unlock(&worker->lock);
    return 0;
}

static void rknn_worker_reset(RknnWorker *worker) {
    int i;

    memset(worker, 0, sizeof(*worker));
    worker->ctx = 0;
    worker->stats.last_frame_seq = -1;
    worker->pending_slot = -1;
    for (i = 0; i < RKNN_WORKER_INPUT_SLOT_COUNT; i++) {
        worker->input_slot_state[i] = 0;
    }
}

static int rknn_worker_load_file(const char *path, unsigned char **data, size_t *size) {
    int fd = -1;
    struct stat st;
    off_t file_size;
    unsigned char *buffer;
    size_t total_read = 0;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno != 0 ? errno : ENOENT;
    }

    if (fstat(fd, &st) < 0) {
        int rc = errno != 0 ? errno : EIO;
        close(fd);
        return rc;
    }

    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return EINVAL;
    }

    file_size = st.st_size;

    buffer = (unsigned char *)malloc((size_t)file_size);
    if (buffer == NULL) {
        close(fd);
        return ENOMEM;
    }

    while (total_read < (size_t)file_size) {
        ssize_t nread = read(fd,
                             buffer + total_read,
                             (size_t)file_size - total_read);

        if (nread < 0) {
            int rc = errno != 0 ? errno : EIO;
            if (rc == EINTR) {
                continue;
            }
            free(buffer);
            close(fd);
            return rc;
        }

        if (nread == 0) {
            free(buffer);
            close(fd);
            return EIO;
        }

        total_read += (size_t)nread;
    }

    if (close(fd) < 0) {
        int rc = errno != 0 ? errno : EIO;
        free(buffer);
        return rc;
    }

    *data = buffer;
    *size = (size_t)file_size;
    return 0;
}

static void rknn_worker_fill_input_info(RknnWorker *worker) {
    rknn_tensor_attr *attr = &worker->input_attrs[0];
    RknnWorkerInputInfo *info = &worker->input_info;

    memset(info, 0, sizeof(*info));
    info->n_dims = attr->n_dims;
    memcpy(info->dims, attr->dims, sizeof(uint32_t) * attr->n_dims);
    info->fmt = attr->fmt;
    info->type = attr->type;

    if (attr->n_dims == 4 && attr->fmt == RKNN_TENSOR_NCHW) {
        info->channels = attr->dims[1];
        info->height = attr->dims[2];
        info->width = attr->dims[3];
    } else if (attr->n_dims == 4 && attr->fmt == RKNN_TENSOR_NHWC) {
        info->height = attr->dims[1];
        info->width = attr->dims[2];
        info->channels = attr->dims[3];
    }

    if (info->width > 0 && info->height > 0 && info->channels > 0) {
        info->size = info->width * info->height * info->channels;
    } else {
        info->size = attr->size_with_stride > 0 ? attr->size_with_stride : attr->size;
    }
}

static void rknn_worker_dump_tensor_attr(const char *label, const rknn_tensor_attr *attr) {
    uint32_t i;

    if (label == NULL || attr == NULL) {
        return;
    }

    printf("[RKNN] %s index=%u name=%s dims=[", label, attr->index, attr->name);
    for (i = 0; i < attr->n_dims; i++) {
        printf("%u%s", attr->dims[i], (i + 1 < attr->n_dims) ? "," : "");
    }
    printf("] elems=%u size=%u fmt=%s type=%s qnt=%s zp=%d scale=%f\n",
           attr->n_elems,
           attr->size,
           get_format_string(attr->fmt),
           get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type),
           attr->zp,
           attr->scale);
}

static int rknn_worker_query_model(RknnWorker *worker) {
    int ret;
    uint32_t i;
    rknn_sdk_version sdk_version;

    memset(&worker->io_num, 0, sizeof(worker->io_num));
    ret = rknn_query(worker->ctx, RKNN_QUERY_IN_OUT_NUM,
                     &worker->io_num, sizeof(worker->io_num));
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[RKNN] query input/output count failed: %d\n", ret);
        return EIO;
    }

    if (worker->io_num.n_input == 0 || worker->io_num.n_output == 0) {
        fprintf(stderr, "[RKNN] invalid io count: input=%u output=%u\n",
                worker->io_num.n_input, worker->io_num.n_output);
        return EINVAL;
    }

    worker->input_attrs = (rknn_tensor_attr *)calloc(worker->io_num.n_input,
                                                     sizeof(rknn_tensor_attr));
    worker->output_attrs = (rknn_tensor_attr *)calloc(worker->io_num.n_output,
                                                      sizeof(rknn_tensor_attr));
    if (worker->input_attrs == NULL || worker->output_attrs == NULL) {
        return ENOMEM;
    }

    for (i = 0; i < worker->io_num.n_input; i++) {
        worker->input_attrs[i].index = i;
        ret = rknn_query(worker->ctx, RKNN_QUERY_INPUT_ATTR,
                         &worker->input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "[RKNN] query input attr %u failed: %d\n", i, ret);
            return EIO;
        }
        rknn_worker_dump_tensor_attr("input", &worker->input_attrs[i]);
    }

    for (i = 0; i < worker->io_num.n_output; i++) {
        worker->output_attrs[i].index = i;
        ret = rknn_query(worker->ctx, RKNN_QUERY_OUTPUT_ATTR,
                         &worker->output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "[RKNN] query output attr %u failed: %d\n", i, ret);
            return EIO;
        }
        rknn_worker_dump_tensor_attr("output", &worker->output_attrs[i]);
    }

    memset(&sdk_version, 0, sizeof(sdk_version));
    if (rknn_query(worker->ctx, RKNN_QUERY_SDK_VERSION,
                   &sdk_version, sizeof(sdk_version)) == RKNN_SUCC) {
        printf("[RKNN] api=%s drv=%s\n",
               sdk_version.api_version, sdk_version.drv_version);
    }

    rknn_worker_fill_input_info(worker);
    worker->input_size = worker->input_info.size;
    printf("[RKNN] model io ready: inputs=%u outputs=%u input=%ux%ux%u size=%zu fmt=%s type=%s\n",
           worker->io_num.n_input,
           worker->io_num.n_output,
           worker->input_info.width,
           worker->input_info.height,
           worker->input_info.channels,
           worker->input_size,
           get_format_string(worker->input_info.fmt),
           get_type_string(worker->input_info.type));

    return 0;
}

static int rknn_worker_run_once(RknnWorker *worker,
                                const RknnWorkerTaskMeta *meta,
                                const void *input_data,
                                size_t input_size) {
    rknn_input input;
    rknn_output *outputs = NULL;
    rknn_output_extend output_extend;
    int64_t start_us;
    int64_t end_us;
    int ret;
    uint32_t i;

    memset(&input, 0, sizeof(input));
    input.index = 0;
    input.buf = (void *)input_data;
    input.size = (uint32_t)input_size;
    input.pass_through = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;

    start_us = rknn_worker_now_us();

    ret = rknn_inputs_set(worker->ctx, 1, &input);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[RKNN] inputs_set failed: %d frame=%lld\n",
                ret, (long long)meta->frame_seq);
        return ret;
    }

    ret = rknn_run(worker->ctx, NULL);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[RKNN] run failed: %d frame=%lld\n",
                ret, (long long)meta->frame_seq);
        return ret;
    }

    outputs = (rknn_output *)calloc(worker->io_num.n_output, sizeof(rknn_output));
    if (outputs == NULL) {
        return -ENOMEM;
    }

    for (i = 0; i < worker->io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = worker->want_float_outputs ? 1 : 0;
        outputs[i].is_prealloc = 0;
    }

    memset(&output_extend, 0, sizeof(output_extend));
    ret = rknn_outputs_get(worker->ctx, worker->io_num.n_output,
                           outputs, &output_extend);
    if (ret == RKNN_SUCC) {
        (void)rknn_worker_decode_yolov8(worker, meta, outputs);
        {
            int release_ret = rknn_outputs_release(worker->ctx, worker->io_num.n_output, outputs);
            if (release_ret != RKNN_SUCC) {
                ret = release_ret;
            }
        }
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "[RKNN] outputs_release failed: %d frame=%lld\n",
                    ret, (long long)meta->frame_seq);
        }
    } else {
        fprintf(stderr, "[RKNN] outputs_get failed: %d frame=%lld\n",
                ret, (long long)meta->frame_seq);
    }

    free(outputs);
    end_us = rknn_worker_now_us();

    pthread_mutex_lock(&worker->lock);
    worker->stats.last_infer_us = end_us > start_us ? end_us - start_us : 0;
    pthread_mutex_unlock(&worker->lock);

    return ret;
}

static int rknn_worker_run_mem_once(RknnWorker *worker,
                                    const RknnWorkerTaskMeta *meta,
                                    rknn_tensor_mem *input_mem) {
    rknn_tensor_attr input_attr;
    rknn_output *outputs = NULL;
    rknn_output_extend output_extend;
    int64_t start_us;
    int64_t end_us;
    int ret;
    uint32_t i;

    if (worker == NULL || meta == NULL || input_mem == NULL) {
        return -EINVAL;
    }

    if (worker->io_num.n_input < 1 || worker->input_attrs == NULL) {
        return -EINVAL;
    }

    input_attr = worker->input_attrs[0];
    input_attr.index = 0;
    input_attr.type = RKNN_TENSOR_UINT8;
    input_attr.fmt = RKNN_TENSOR_NHWC;
    input_attr.pass_through = 0;
    input_attr.size = (uint32_t)meta->size;
    if (worker->input_info.width > 0) {
        input_attr.w_stride = worker->input_info.width;
    }
    input_attr.h_stride = worker->input_info.height;

    start_us = rknn_worker_now_us();

    ret = rknn_set_io_mem(worker->ctx, input_mem, &input_attr);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[RKNN] set input mem failed: %d frame=%lld fd=%d\n",
                ret, (long long)meta->frame_seq, input_mem->fd);
        return ret;
    }

    ret = rknn_run(worker->ctx, NULL);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[RKNN] run failed: %d frame=%lld\n",
                ret, (long long)meta->frame_seq);
        return ret;
    }

    outputs = (rknn_output *)calloc(worker->io_num.n_output, sizeof(rknn_output));
    if (outputs == NULL) {
        return -ENOMEM;
    }

    for (i = 0; i < worker->io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = worker->want_float_outputs ? 1 : 0;
        outputs[i].is_prealloc = 0;
    }

    memset(&output_extend, 0, sizeof(output_extend));
    ret = rknn_outputs_get(worker->ctx, worker->io_num.n_output,
                           outputs, &output_extend);
    if (ret == RKNN_SUCC) {
        (void)rknn_worker_decode_yolov8(worker, meta, outputs);
        {
            int release_ret = rknn_outputs_release(worker->ctx, worker->io_num.n_output, outputs);
            if (release_ret != RKNN_SUCC) {
                ret = release_ret;
            }
        }
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "[RKNN] outputs_release failed: %d frame=%lld\n",
                    ret, (long long)meta->frame_seq);
        }
    } else {
        fprintf(stderr, "[RKNN] outputs_get failed: %d frame=%lld\n",
                ret, (long long)meta->frame_seq);
    }

    free(outputs);
    end_us = rknn_worker_now_us();

    pthread_mutex_lock(&worker->lock);
    worker->stats.last_infer_us = end_us > start_us ? end_us - start_us : 0;
    pthread_mutex_unlock(&worker->lock);

    return ret;
}

static int rknn_worker_alloc_input_mems(RknnWorker *worker) {
    int i;

    if (worker == NULL || worker->ctx == 0 || worker->input_size == 0) {
        return EINVAL;
    }

    for (i = 0; i < RKNN_WORKER_INPUT_SLOT_COUNT; i++) {
        uint32_t alloc_size = worker->input_attrs[0].size_with_stride > 0 ?
                              worker->input_attrs[0].size_with_stride :
                              (uint32_t)worker->input_size;

        worker->input_mems[i] = rknn_create_mem2(worker->ctx,
                                                 alloc_size,
                                                 RKNN_FLAG_MEMORY_NON_CACHEABLE);
        if (worker->input_mems[i] == NULL) {
            fprintf(stderr, "[RKNN] create input dma-buf slot %d failed\n", i);
            return ENOMEM;
        }
        if (worker->input_mems[i]->fd < 0 ||
            worker->input_mems[i]->size < worker->input_size) {
            fprintf(stderr,
                    "[RKNN] invalid input dma-buf slot %d fd=%d size=%u need=%zu\n",
                    i,
                    worker->input_mems[i]->fd,
                    worker->input_mems[i]->size,
                    worker->input_size);
            return EIO;
        }
        worker->input_slot_state[i] = 0;
    }

    return 0;
}

static void *rknn_worker_thread(void *arg) {
    RknnWorker *worker = (RknnWorker *)arg;

    while (1) {
        RknnWorkerTaskMeta meta;
        int pending_slot;
        int ret;

        pthread_mutex_lock(&worker->lock);
        while (!worker->stopping && !worker->pending_valid) {
            pthread_cond_wait(&worker->cond, &worker->lock);
        }

        if (worker->stopping) {
            pthread_mutex_unlock(&worker->lock);
            break;
        }

        meta = worker->pending_meta;
        pending_slot = worker->pending_slot;
        if (pending_slot < 0 && worker->pending_input != NULL && worker->work_input != NULL) {
            memcpy(worker->work_input, worker->pending_input, meta.size);
        }
        worker->pending_valid = 0;
        worker->pending_slot = -1;
        pthread_mutex_unlock(&worker->lock);

        if (pending_slot >= 0 && pending_slot < RKNN_WORKER_INPUT_SLOT_COUNT) {
            ret = rknn_worker_run_mem_once(worker,
                                           &meta,
                                           worker->input_mems[pending_slot]);
        } else {
            ret = rknn_worker_run_once(worker, &meta, worker->work_input, meta.size);
        }

        pthread_mutex_lock(&worker->lock);
        if (pending_slot >= 0 && pending_slot < RKNN_WORKER_INPUT_SLOT_COUNT &&
            worker->input_slot_state[pending_slot] == 2) {
            worker->input_slot_state[pending_slot] = 0;
        }
        if (ret == RKNN_SUCC) {
            worker->stats.processed++;
        } else {
            worker->stats.failed++;
            worker->stats.last_error = ret;
        }
        worker->stats.last_frame_seq = meta.frame_seq;
        worker->stats.last_timestamp_ms = meta.timestamp_ms;
        pthread_mutex_unlock(&worker->lock);
    }

    return NULL;
}

void rknn_worker_config_defaults(RknnWorkerConfig *config) {
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->model_path = RKNN_WORKER_DEFAULT_MODEL_PATH;
    config->core_mask = RKNN_NPU_CORE_AUTO;
    config->init_flags = RKNN_FLAG_PRIOR_MEDIUM;
    config->want_float_outputs = 1;
    config->conf_threshold = 0.25f;
    config->nms_threshold = 0.45f;
    config->camera_width = 1280;
    config->camera_height = 720;
}

int rknn_worker_start(RknnWorker *worker, const RknnWorkerConfig *config) {
    RknnWorkerConfig effective_config;
    unsigned char *model_data = NULL;
    size_t model_size = 0;
    int ret;
    int rc;

    if (worker == NULL) {
        return EINVAL;
    }

    rknn_worker_reset(worker);
    rknn_worker_config_defaults(&effective_config);
    if (config != NULL) {
        if (config->model_path != NULL) {
            effective_config.model_path = config->model_path;
        }
        effective_config.core_mask = config->core_mask;
        effective_config.init_flags = config->init_flags;
        effective_config.want_float_outputs = config->want_float_outputs;
        effective_config.conf_threshold = config->conf_threshold;
        effective_config.nms_threshold = config->nms_threshold;
        effective_config.camera_width = config->camera_width;
        effective_config.camera_height = config->camera_height;
        effective_config.detect_state = config->detect_state;
    }
    worker->want_float_outputs = effective_config.want_float_outputs;
    worker->conf_threshold = effective_config.conf_threshold;
    worker->nms_threshold = effective_config.nms_threshold;
    worker->camera_width = effective_config.camera_width;
    worker->camera_height = effective_config.camera_height;
    worker->detect_state = effective_config.detect_state;

    rc = pthread_mutex_init(&worker->lock, NULL);
    if (rc != 0) {
        return rc;
    }
    worker->lock_ready = 1;

    rc = pthread_cond_init(&worker->cond, NULL);
    if (rc != 0) {
        rknn_worker_stop(worker);
        return rc;
    }
    worker->cond_ready = 1;

    rc = rknn_worker_load_file(effective_config.model_path, &model_data, &model_size);
    if (rc != 0) {
        fprintf(stderr, "[RKNN] load model failed: %s rc=%d\n",
                effective_config.model_path, rc);
        rknn_worker_stop(worker);
        return rc;
    }

    ret = rknn_init(&worker->ctx, model_data, (uint32_t)model_size,
                    effective_config.init_flags, NULL);
    free(model_data);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[RKNN] rknn_init failed: %d model=%s\n",
                ret, effective_config.model_path);
        rknn_worker_stop(worker);
        return EIO;
    }

    if (effective_config.core_mask != RKNN_NPU_CORE_AUTO) {
        ret = rknn_set_core_mask(worker->ctx, effective_config.core_mask);
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "[RKNN] set core mask failed: %d\n", ret);
        }
    }

    rc = rknn_worker_query_model(worker);
    if (rc != 0) {
        rknn_worker_stop(worker);
        return rc;
    }

    rc = rknn_worker_alloc_input_mems(worker);
    if (rc != 0) {
        rknn_worker_stop(worker);
        return rc;
    }

    worker->pending_input = (unsigned char *)malloc(worker->input_size);
    worker->work_input = (unsigned char *)malloc(worker->input_size);
    if (worker->pending_input == NULL || worker->work_input == NULL) {
        rknn_worker_stop(worker);
        return ENOMEM;
    }

    worker->ready = 1;
    rc = pthread_create(&worker->tid, NULL, rknn_worker_thread, worker);
    if (rc != 0) {
        rknn_worker_stop(worker);
        return rc;
    }
    worker->thread_started = 1;

    printf("[RKNN] worker started with model %s\n", effective_config.model_path);
    return 0;
}

void rknn_worker_request_stop(RknnWorker *worker) {
    if (worker == NULL || !worker->lock_ready) {
        return;
    }

    pthread_mutex_lock(&worker->lock);
    worker->stopping = 1;
    if (worker->cond_ready) {
        pthread_cond_signal(&worker->cond);
    }
    pthread_mutex_unlock(&worker->lock);
}

void rknn_worker_stop(RknnWorker *worker) {
    int i;

    if (worker == NULL) {
        return;
    }

    rknn_worker_request_stop(worker);

    if (worker->thread_started) {
        pthread_join(worker->tid, NULL);
        worker->thread_started = 0;
    }

    for (i = 0; i < RKNN_WORKER_INPUT_SLOT_COUNT; i++) {
        if (worker->input_mems[i] != NULL && worker->ctx != 0) {
            int ret = rknn_destroy_mem(worker->ctx, worker->input_mems[i]);
            if (ret != RKNN_SUCC) {
                fprintf(stderr, "[RKNN] destroy input mem %d failed: %d\n", i, ret);
            }
            worker->input_mems[i] = NULL;
        }
        worker->input_slot_state[i] = 0;
    }

    if (worker->ctx != 0) {
        int ret = rknn_destroy(worker->ctx);
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "[RKNN] rknn_destroy failed: %d\n", ret);
        }
        worker->ctx = 0;
    }

    free(worker->input_attrs);
    worker->input_attrs = NULL;
    free(worker->output_attrs);
    worker->output_attrs = NULL;
    free(worker->pending_input);
    worker->pending_input = NULL;
    free(worker->work_input);
    worker->work_input = NULL;
    worker->ready = 0;

    if (worker->cond_ready) {
        pthread_cond_destroy(&worker->cond);
        worker->cond_ready = 0;
    }
    if (worker->lock_ready) {
        pthread_mutex_destroy(&worker->lock);
        worker->lock_ready = 0;
    }
}

int rknn_worker_submit(RknnWorker *worker,
                       const void *input_data,
                       size_t input_size,
                       int64_t frame_seq,
                       int64_t timestamp_ms) {
    if (worker == NULL || input_data == NULL || input_size == 0) {
        return EINVAL;
    }
    if (!worker->ready || worker->stopping) {
        return ECANCELED;
    }
    if (input_size > worker->input_size) {
        return EMSGSIZE;
    }

    pthread_mutex_lock(&worker->lock);
    if (worker->pending_valid) {
        if (worker->pending_slot >= 0 &&
            worker->pending_slot < RKNN_WORKER_INPUT_SLOT_COUNT &&
            worker->input_slot_state[worker->pending_slot] == 2) {
            worker->input_slot_state[worker->pending_slot] = 0;
        }
        worker->stats.dropped++;
    }

    memcpy(worker->pending_input, input_data, input_size);
    worker->pending_meta.frame_seq = frame_seq;
    worker->pending_meta.timestamp_ms = timestamp_ms;
    worker->pending_meta.size = input_size;
    worker->pending_slot = -1;
    worker->pending_valid = 1;
    worker->stats.submitted++;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    return 0;
}

int rknn_worker_acquire_input_buffer(RknnWorker *worker, RknnWorkerInputBuffer *buffer) {
    int i;

    if (worker == NULL || buffer == NULL) {
        return EINVAL;
    }
    if (!worker->ready || worker->stopping) {
        return ECANCELED;
    }

    pthread_mutex_lock(&worker->lock);
    if (!worker->ready || worker->stopping) {
        pthread_mutex_unlock(&worker->lock);
        return ECANCELED;
    }

    for (i = 0; i < RKNN_WORKER_INPUT_SLOT_COUNT; i++) {
        if (worker->input_mems[i] != NULL && worker->input_slot_state[i] == 0) {
            memset(buffer, 0, sizeof(*buffer));
            worker->input_slot_state[i] = 1;
            buffer->slot = i;
            buffer->fd = worker->input_mems[i]->fd;
            buffer->width = worker->input_info.width;
            buffer->height = worker->input_info.height;
            buffer->channels = worker->input_info.channels;
            buffer->size = worker->input_info.size;
            pthread_mutex_unlock(&worker->lock);
            return 0;
        }
    }

    worker->stats.dropped++;
    pthread_mutex_unlock(&worker->lock);
    return EBUSY;
}

void rknn_worker_release_input_buffer(RknnWorker *worker, int slot) {
    if (worker == NULL ||
        slot < 0 ||
        slot >= RKNN_WORKER_INPUT_SLOT_COUNT ||
        !worker->lock_ready) {
        return;
    }

    pthread_mutex_lock(&worker->lock);
    if (worker->input_slot_state[slot] == 1) {
        worker->input_slot_state[slot] = 0;
    }
    pthread_mutex_unlock(&worker->lock);
}

int rknn_worker_submit_input_buffer(RknnWorker *worker,
                                    int slot,
                                    int64_t frame_seq,
                                    int64_t timestamp_ms) {
    if (worker == NULL ||
        slot < 0 ||
        slot >= RKNN_WORKER_INPUT_SLOT_COUNT) {
        return EINVAL;
    }
    if (!worker->ready || worker->stopping) {
        return ECANCELED;
    }

    pthread_mutex_lock(&worker->lock);
    if (!worker->ready || worker->stopping) {
        if (worker->input_slot_state[slot] == 1) {
            worker->input_slot_state[slot] = 0;
        }
        pthread_mutex_unlock(&worker->lock);
        return ECANCELED;
    }
    if (worker->input_mems[slot] == NULL || worker->input_slot_state[slot] != 1) {
        pthread_mutex_unlock(&worker->lock);
        return EINVAL;
    }

    if (worker->pending_valid) {
        if (worker->pending_slot >= 0 &&
            worker->pending_slot < RKNN_WORKER_INPUT_SLOT_COUNT &&
            worker->input_slot_state[worker->pending_slot] == 2) {
            worker->input_slot_state[worker->pending_slot] = 0;
        }
        worker->stats.dropped++;
    }

    worker->pending_meta.frame_seq = frame_seq;
    worker->pending_meta.timestamp_ms = timestamp_ms;
    worker->pending_meta.size = worker->input_size;
    worker->pending_slot = slot;
    worker->pending_valid = 1;
    worker->input_slot_state[slot] = 2;
    worker->stats.submitted++;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);
    return 0;
}

int rknn_worker_get_input_info(RknnWorker *worker, RknnWorkerInputInfo *info) {
    if (worker == NULL || info == NULL) {
        return EINVAL;
    }
    if (!worker->ready) {
        return ECANCELED;
    }

    pthread_mutex_lock(&worker->lock);
    *info = worker->input_info;
    pthread_mutex_unlock(&worker->lock);
    return 0;
}

void rknn_worker_get_stats(RknnWorker *worker, RknnWorkerStats *stats) {
    if (worker == NULL || stats == NULL || !worker->lock_ready) {
        return;
    }

    pthread_mutex_lock(&worker->lock);
    *stats = worker->stats;
    pthread_mutex_unlock(&worker->lock);
}

int rknn_worker_is_ready(RknnWorker *worker) {
    int ready;

    if (worker == NULL || !worker->lock_ready) {
        return 0;
    }

    pthread_mutex_lock(&worker->lock);
    ready = worker->ready && !worker->stopping;
    pthread_mutex_unlock(&worker->lock);
    return ready;
}
