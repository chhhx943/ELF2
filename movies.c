#include "movies.h"
#include "encoder.h"
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

#define VIDEO_DEV "/dev/video11"
#define WIDTH  1280
#define HEIGHT 720
#define BUF_COUNT 4
#define FRAME_SIZE (WIDTH * HEIGHT * 3 / 2) // NV12

// ==========================================================
// 第一层：硬件抽象层 (V4L2 封装)
// ==========================================================

struct Buffer {
    void *start;
    size_t length;
};

// 摄像头上下文句柄
typedef struct {
    int fd;
    struct Buffer buffers[BUF_COUNT];
    int buf_type; // V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
} CameraCtx;

// 初始化摄像头并分配内存映射
int camera_init(CameraCtx *ctx, const char *dev_name, int width, int height) {
    ctx->fd = open(dev_name, O_RDWR);
    if (ctx->fd < 0) {
        perror("camera open");
        return -1;
    }

    ctx->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = ctx->buf_type;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = 1;

    if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("camera SetFormat");
        return -1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUF_COUNT;
    req.type = ctx->buf_type;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("camera request buffer");
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    for (int i = 0; i < BUF_COUNT; ++i) {
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = ctx->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("camera query buffer");
            return -1;
        }

        ctx->buffers[i].length = buf.m.planes[0].length;
        ctx->buffers[i].start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, 
                                     MAP_SHARED, ctx->fd, buf.m.planes[0].m.mem_offset);
        
        if (ctx->buffers[i].start == MAP_FAILED) {
            perror("camera mmap");
            return -1;
        }

        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("camera queue buf");
            return -1;
        }
    }
    return 0;
}

// 开启数据流
int camera_start(CameraCtx *ctx) {
    return ioctl(ctx->fd, VIDIOC_STREAMON, &ctx->buf_type);
}

// 停止数据流
int camera_stop(CameraCtx *ctx) {
    return ioctl(ctx->fd, VIDIOC_STREAMOFF, &ctx->buf_type);
}

// 释放资源
void camera_deinit(CameraCtx *ctx) {
    for (int i = 0; i < BUF_COUNT; i++) {
        if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
            munmap(ctx->buffers[i].start, ctx->buffers[i].length);
        }
    }
    if (ctx->fd >= 0) close(ctx->fd);
}

// ==========================================================
// 第二层：应用业务层 (主函数)
// ==========================================================

int main() {
    CameraCtx cam;
    if (camera_init(&cam, VIDEO_DEV, WIDTH, HEIGHT) < 0) {
        fprintf(stderr, "Failed to init camera\n");
        exit(1);
    }

    // 初始化 IPC (共享内存与管道)
    int shmid = shmget(IPC_PRIVATE, FRAME_SIZE, IPC_CREAT | 0666);
    if (shmid < 0) { perror("shmget"); camera_deinit(&cam); exit(1); }
    
    void *shmaddr = shmat(shmid, NULL, 0);
    if (shmaddr == (void *)-1) { perror("shmat"); camera_deinit(&cam); exit(1); }

    int sync_pipe[2];
    if (pipe(sync_pipe) < 0) { perror("pipe"); exit(1); }
    signal(SIGPIPE, SIG_IGN);

    // ==========================================
    // 进程分离 (Fork)
    // ==========================================
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    } 
    else if (pid == 0) {
        // --- 子进程：推流服务 ---
        close(sync_pipe[1]); // 关闭写端
        void *child_shmaddr = shmat(shmid, NULL, 0);
        
        FFmpegStreamer streamer;
        if (streamer_init(&streamer, "rtmp://192.168.31.121/live/test", WIDTH, HEIGHT, 30) < 0) {
            fprintf(stderr, "Streamer init failed\n");
            exit(1);
        }

        char signal_buf;
        while (read(sync_pipe[0], &signal_buf, 1) > 0) {
            streamer_push(&streamer, child_shmaddr);
        }

        streamer_clean(&streamer);
        shmdt(child_shmaddr);
        close(sync_pipe[0]);
        exit(0);
    } 
    else {
        // --- 父进程：采图服务 ---
        close(sync_pipe[0]); // 关闭读端
        
        if (camera_start(&cam) < 0) {
            perror("camera start failed");
            goto cleanup;
        }

        struct v4l2_buffer qbuf;
        struct v4l2_plane qplanes[1];
        
        while (1) {
            memset(&qbuf, 0, sizeof(qbuf));
            qbuf.type = cam.buf_type;
            qbuf.memory = V4L2_MEMORY_MMAP;
            qbuf.m.planes = qplanes;
            qbuf.length = 1;

            // 1. 出队 (获取一帧)
            if (ioctl(cam.fd, VIDIOC_DQBUF, &qbuf) < 0) {
                perror("camera dequeue buf");
                break;
            }

            // 2. 数据处理 (拷贝到共享内存)
            memcpy(shmaddr, cam.buffers[qbuf.index].start, FRAME_SIZE);

            // 3. 通知子进程
            char go = 'g';
            if (write(sync_pipe[1], &go, 1) <= 0) break;

            // 4. 入队 (交还给内核)
            if (ioctl(cam.fd, VIDIOC_QBUF, &qbuf) < 0) {
                perror("camera queue buf");
                break;
            }
        }

cleanup:
        // 清理回收
        camera_stop(&cam);
        camera_deinit(&cam);
        close(sync_pipe[1]);
        shmdt(shmaddr);
        shmctl(shmid, IPC_RMID, NULL);
        wait(NULL); // 等待子进程退出
    }
 
    return 0;
}