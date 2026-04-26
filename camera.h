#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>

#define BUF_COUNT 4

struct Buffer {
    void *start;
    size_t length;
};

typedef struct {
    int fd;
    struct Buffer buffers[BUF_COUNT];
    int buf_type; // V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
} CameraCtx;

int camera_init(CameraCtx *ctx, const char *dev_name, int width, int height);
int camera_start(CameraCtx *ctx);
int camera_stop(CameraCtx *ctx);
void camera_deinit(CameraCtx *ctx);

#endif