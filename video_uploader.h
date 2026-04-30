#ifndef VIDEO_UPLOADER_H
#define VIDEO_UPLOADER_H

#include <pthread.h>
#include <stdint.h>

#include "local_store.h"

#define VIDEO_UPLOADER_HTTP_UPLOAD_URL "http://192.168.31.122/api/video/upload"
#define VIDEO_UPLOADER_HTTP_AUTH_TOKEN "CHANGE_ME_TOKEN"
#define VIDEO_UPLOADER_HTTP_DEVICE_ID "0122"
#define VIDEO_UPLOADER_HTTP_CONNECT_TIMEOUT_SEC 5L
#define VIDEO_UPLOADER_HTTP_REQUEST_TIMEOUT_SEC 120L

#define VIDEO_UPLOADER_IDLE_INTERVAL_MS 3000
#define VIDEO_UPLOADER_RETRY_BASE_MS    2000
#define VIDEO_UPLOADER_RETRY_MAX_MS     30000

typedef int (*VideoUploaderUploadFn)(const LocalVideoSegmentRecord *segment,
                                     char *remote_path,
                                     size_t remote_path_size,
                                     char *error_msg,
                                     size_t error_msg_size,
                                     void *user_data);

typedef struct {
    pthread_t tid;
    int started;
    int stopping;
    int stop_event_fd;

    char root_dir[PATH_MAX];
    LocalStore store;
    int store_ready;

    int idle_interval_ms;
    int retry_backoff_ms;
    int retry_backoff_max_ms;

    VideoUploaderUploadFn upload_fn;
    void *user_data;
} VideoUploader;

int video_uploader_start(VideoUploader *uploader,
                         const char *root_dir,
                         VideoUploaderUploadFn upload_fn,
                         void *user_data);
void video_uploader_stop(VideoUploader *uploader);
int video_uploader_http_upload_callback(const LocalVideoSegmentRecord *segment,
                                        char *remote_path,
                                        size_t remote_path_size,
                                        char *error_msg,
                                        size_t error_msg_size,
                                        void *user_data);

#endif
