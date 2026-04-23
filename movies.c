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
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>


#define VIDEO_DEV "/dev/video11"
#define WIDTH  1280
#define HEIGHT 720
#define BUF_COUNT 4
#define FRAME_SIZE (WIDTH * HEIGHT * 3 / 2) // NV12

// ==========================================================
// 第一层：硬件抽象层 (V4L2 封装)
// ==========================================================

volatile sig_atomic_t keep_running = 1;

struct Buffer {
    void *start;
    size_t length;
};

void sig_handler(int sig) {
    keep_running = 0;
}

// 摄像头上下文句柄
typedef struct {
    int fd;
    struct Buffer buffers[BUF_COUNT];
    int buf_type; // V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
} CameraCtx;

// 魔法函数 1：发送 FD (父进程用)
int send_fd(int socket, int fd_to_send) {
    struct msghdr msg = {0};
    char buf[1] = {'g'}; // 伴随的一字节普通数据
    struct iovec iov[1] = {{.iov_base = buf, .iov_len = 1}};
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    // 预留辅助数据的内存空间 (注意内存对齐)
    union {
        struct cmsghdr align;
        char cmsg_space[CMSG_SPACE(sizeof(int))];
    } u;
    msg.msg_control = u.cmsg_space;
    msg.msg_controllen = sizeof(u.cmsg_space);

    // 配置 SCM_RIGHTS 魔法
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    return sendmsg(socket, &msg, 0);
}

// 魔法函数 2：接收 FD (子进程用)
int recv_fd(int socket) {
    struct msghdr msg = {0};
    char buf[1];
    struct iovec iov[1] = {{.iov_base = buf, .iov_len = 1}};
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    union {
        struct cmsghdr align;
        char cmsg_space[CMSG_SPACE(sizeof(int))];
    } u;
    msg.msg_control = u.cmsg_space;
    msg.msg_controllen = sizeof(u.cmsg_space);

    if (recvmsg(socket, &msg, 0) <= 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int received_fd;
        memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
        return received_fd; // 返回内核在子进程中克隆出来的新 FD！
    }
    return -1;
}

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
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

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

    int sv[2]; // sv[0] 给父进程用，sv[1] 给子进程用
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    perror("socketpair");
    exit(1);
    }
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
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        close(sv[0]); 

        
        FFmpegStreamer streamer;
        if (streamer_init(&streamer, "rtmp://192.168.31.121/live/test", WIDTH, HEIGHT, 30) < 0) {
            fprintf(stderr, "Streamer init failed\n");
            exit(1);
        }

        int child_fd;
        while ((child_fd = recv_fd(sv[1])) >= 0) {
            if (streamer_push_zerocopy(&streamer, child_fd) < 0) {
                fprintf(stderr, "[Child] Push error, skipping frame.\n");
            }
            char ack = 'k';
            if (write(sv[1], &ack, 1) <= 0) {
                break; 
            }
            close(child_fd);
        }
        avcodec_send_frame(streamer.enc_ctx, NULL);
        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(streamer.enc_ctx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, streamer.enc_ctx->time_base, streamer.video_st->time_base);
            pkt->stream_index = streamer.video_st->index;
            av_interleaved_write_frame(streamer.fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        streamer_clean(&streamer);
        close(sv[1]);
        exit(0);
    } 
    else {
        // --- 父进程：采图服务 ---
        close(sv[1]); // 关闭读端
        

        int dma_fds[BUF_COUNT];
        for (int i = 0; i < BUF_COUNT; i++) {
            struct v4l2_exportbuffer expbuf;
            memset(&expbuf, 0, sizeof(expbuf));
            expbuf.type = cam.buf_type; // 通常是 V4L2_BUF_TYPE_VIDEO_CAPTURE
            expbuf.index = i;
            
            if (ioctl(cam.fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                perror("[Parent] VIDIOC_EXPBUF failed (Does your driver support DMA-BUF?)");
                goto cleanup;
            }
            dma_fds[i] = expbuf.fd; // 保存这把“金钥匙”
        }
        if (camera_start(&cam) < 0) {
            perror("camera start failed");
            goto cleanup;
        }
        struct v4l2_buffer qbuf;
        struct v4l2_plane qplanes[1];
              
        while (keep_running) {

          memset(&qbuf, 0, sizeof(qbuf));
          qbuf.type = cam.buf_type;
          qbuf.memory = V4L2_MEMORY_MMAP;
          qbuf.m.planes = qplanes;
          qbuf.length = 1;

            // 1. 出队 (获取一帧)
            if (ioctl(cam.fd, VIDIOC_DQBUF, &qbuf) < 0) {
                if (errno == EINTR) break;
                perror("camera dequeue buf");
                break;
            }
            int current_dma_fd = dma_fds[qbuf.index];
            if (send_fd(sv[0], current_dma_fd) < 0) {
                fprintf(stderr, "[Parent] Failed to send FD to child. Child might be dead.\n");
                break;
            }
            //2.死等子进程的ACK确认信息
            char ack;
            if (read(sv[0], &ack, 1) <= 0) {
                fprintf(stderr, "[Parent] Failed to receive ACK from child. Stopping.\n");
                break;
            }

            // 3. 收到 ACK 说明子进程硬件编码完毕，安全入队交还给内核
            if (ioctl(cam.fd, VIDIOC_QBUF, &qbuf) < 0) {
                perror("[Parent] camera queue buf");
                break;
            }
        
        }

cleanup:
        // 清理回收
        camera_stop(&cam);
        camera_deinit(&cam);
        for (int i = 0; i < BUF_COUNT; i++) {
            if (dma_fds[i] > 0) {
                close(dma_fds[i]);
            }
        }
        close(sv[0]);
        if (shmaddr && shmaddr != (void *)-1) {
            shmdt(shmaddr); // 1. 解除当前进程与共享内存的映射
        }
        if (shmid >= 0) {
            // 2. 发送 IPC_RMID 指令给内核，要求彻底删除该共享内存段
            if (shmctl(shmid, IPC_RMID, NULL) < 0) {
                perror("shmctl IPC_RMID failed");
            }
        }

        wait(NULL); // 等待子进程退出
    }
 
    return 0;
}