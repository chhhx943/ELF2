#include "camera.h"
#include "ipc.h"
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

volatile sig_atomic_t keep_running = 1;

void sig_handler(int sig) {
    keep_running = 0;
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
        if (streamer_init(&streamer, "rtmp://192.168.31.122/live/test", WIDTH, HEIGHT, 30) < 0) {
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