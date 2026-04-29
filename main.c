#include "camera.h"
#include "ipc.h"
#include "encoder.h"
#include "sensor_modbus.h"
#include "aliyun_mqtt.h"
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
#define STREAM_RESTART_INTERVAL_MS 3000

volatile sig_atomic_t is_running = 1;

typedef struct {
    int stream_online;
    pid_t child_pid;
    int parent_sock;
    int restart_interval_ms;
    int64_t last_restart_ms;
} StreamState;

static void sig_handler(int sig) {
    (void)sig;
    is_running = 0;
}

static int64_t now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
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
    stream->last_restart_ms = now_ms();
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

static int child_stream_loop(int sock) {
    FFmpegStreamer streamer;
    AVPacket *pkt;
    int child_fd;
    int ret;

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    if (streamer_init(&streamer, RTMP_URL, WIDTH, HEIGHT, 30) < 0) {
        fprintf(stderr, "[Child] Streamer init failed\n");
        return -1;
    }

    while ((child_fd = recv_fd(sock)) >= 0) {
        ret = streamer_push_zerocopy(&streamer, child_fd);
        close(child_fd);

        if (ret < 0) {
            fprintf(stderr, "[Child] Push failed, exit child loop.\n");
            break;
        }

        char ack = 'k';
        if (write(sock, &ack, 1) <= 0) {
            break;
        }
    }

    avcodec_send_frame(streamer.enc_ctx, NULL);
    pkt = av_packet_alloc();
    while (pkt != NULL && avcodec_receive_packet(streamer.enc_ctx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, streamer.enc_ctx->time_base, streamer.video_st->time_base);
        pkt->stream_index = streamer.video_st->index;
        if (av_interleaved_write_frame(streamer.fmt_ctx, pkt) < 0) {
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    streamer_clean(&streamer);
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
    stream->last_restart_ms = now_ms();

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

    now = now_ms();
    if (now - stream->last_restart_ms < stream->restart_interval_ms) {
        return;
    }

    stream->last_restart_ms = now;
    printf("[Parent] Try restart stream child...\n");

    if (spawn_stream_child(stream) < 0) {
        printf("[Parent] Restart stream child failed.\n");
    }
}

int main(void) {
    CameraCtx cam;
    StreamState stream = {
        .stream_online = 0,
        .child_pid = -1,
        .parent_sock = -1,
        .restart_interval_ms = STREAM_RESTART_INTERVAL_MS,
        .last_restart_ms = 0,
    };
    int shmid = -1;
    void *shmaddr = (void *)-1;
    int dma_fds[BUF_COUNT] = { -1, -1, -1, -1 };

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
        int rc = start_mqtt_reporter();
        if (rc != 0) {
            fprintf(stderr, "[Parent] Failed to start MQTT reporter: %s\n", strerror(rc));
        } else {
            printf("[Parent] MQTT reporter started\n");
        }
    }

    if (spawn_stream_child(&stream) < 0) {
        printf("[Parent] Initial stream child start failed, continue in offline mode.\n");
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
