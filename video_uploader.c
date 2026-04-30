#define _POSIX_C_SOURCE 200809L

#include "video_uploader.h"

#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} VideoUploaderHttpBuffer;

typedef struct {
    char upload_url[256];
    char auth_token[256];
    char device_id[64];
    long connect_timeout_sec;
    long request_timeout_sec;
} VideoUploaderHttpConfig;

static void video_uploader_buffer_reset(VideoUploaderHttpBuffer *buffer) {
    if (buffer == NULL) {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

static int video_uploader_buffer_append(VideoUploaderHttpBuffer *buffer,
                                        const char *data,
                                        size_t len) {
    char *new_data;
    size_t new_capacity;

    if (buffer == NULL || data == NULL) {
        return EINVAL;
    }

    if (buffer->size + len + 1 <= buffer->capacity) {
        memcpy(buffer->data + buffer->size, data, len);
        buffer->size += len;
        buffer->data[buffer->size] = '\0';
        return 0;
    }

    new_capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
    while (new_capacity < buffer->size + len + 1) {
        new_capacity *= 2;
    }

    new_data = realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
        return ENOMEM;
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    memcpy(buffer->data + buffer->size, data, len);
    buffer->size += len;
    buffer->data[buffer->size] = '\0';
    return 0;
}

static size_t video_uploader_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    VideoUploaderHttpBuffer *buffer = (VideoUploaderHttpBuffer *)userdata;
    size_t total = size * nmemb;

    if (video_uploader_buffer_append(buffer, ptr, total) != 0) {
        return 0;
    }

    return total;
}

static int video_uploader_http_copy_string(char *dst, size_t dst_size, const char *src) {
    size_t len;

    if (dst == NULL || dst_size == 0) {
        return EINVAL;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    len = strlen(src);
    if (len >= dst_size) {
        return ENAMETOOLONG;
    }

    memcpy(dst, src, len + 1);
    return 0;
}

static int video_uploader_http_json_extract_string(const char *json,
                                                   const char *key,
                                                   char *out,
                                                   size_t out_size) {
    char pattern[128];
    const char *start;
    const char *value_start;
    const char *value_end;
    size_t len;
    int rc;

    if (json == NULL || key == NULL || out == NULL || out_size == 0) {
        return EINVAL;
    }

    rc = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (rc < 0 || (size_t)rc >= sizeof(pattern)) {
        return ENAMETOOLONG;
    }

    start = strstr(json, pattern);
    if (start == NULL) {
        return ENOENT;
    }

    value_start = start + strlen(pattern);
    value_end = strchr(value_start, '"');
    if (value_end == NULL) {
        return EINVAL;
    }

    len = (size_t)(value_end - value_start);
    if (len >= out_size) {
        return ENAMETOOLONG;
    }

    memcpy(out, value_start, len);
    out[len] = '\0';
    return 0;
}

static int video_uploader_http_json_extract_int(const char *json,
                                                const char *key,
                                                long *value_out) {
    char pattern[128];
    const char *start;
    const char *value_start;
    char *endptr = NULL;
    long value;
    int rc;

    if (json == NULL || key == NULL || value_out == NULL) {
        return EINVAL;
    }

    rc = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (rc < 0 || (size_t)rc >= sizeof(pattern)) {
        return ENAMETOOLONG;
    }

    start = strstr(json, pattern);
    if (start == NULL) {
        return ENOENT;
    }

    value_start = start + strlen(pattern);
    errno = 0;
    value = strtol(value_start, &endptr, 10);
    if (errno != 0 || endptr == value_start) {
        return EINVAL;
    }

    *value_out = value;
    return 0;
}

static int video_uploader_http_fill_from_macro(VideoUploaderHttpConfig *cfg) {

    if (cfg == NULL) {
        return EINVAL;
    }

    memset(cfg, 0, sizeof(*cfg));

    if (VIDEO_UPLOADER_HTTP_UPLOAD_URL[0] == '\0') {
        return ENOENT;
    }
    if (video_uploader_http_copy_string(cfg->upload_url,
                                        sizeof(cfg->upload_url),
                                        VIDEO_UPLOADER_HTTP_UPLOAD_URL) != 0) {
        return ENAMETOOLONG;
    }

    if (VIDEO_UPLOADER_HTTP_AUTH_TOKEN[0] == '\0') {
        return ENOENT;
    }
    if (video_uploader_http_copy_string(cfg->auth_token,
                                        sizeof(cfg->auth_token),
                                        VIDEO_UPLOADER_HTTP_AUTH_TOKEN) != 0) {
        return ENAMETOOLONG;
    }

    if (VIDEO_UPLOADER_HTTP_DEVICE_ID[0] == '\0') {
        return ENOENT;
    }
    if (video_uploader_http_copy_string(cfg->device_id,
                                        sizeof(cfg->device_id),
                                        VIDEO_UPLOADER_HTTP_DEVICE_ID) != 0) {
        return ENAMETOOLONG;
    }

    cfg->connect_timeout_sec = VIDEO_UPLOADER_HTTP_CONNECT_TIMEOUT_SEC;
    cfg->request_timeout_sec = VIDEO_UPLOADER_HTTP_REQUEST_TIMEOUT_SEC;
    return 0;
}

static int video_uploader_copy_root_dir(VideoUploader *uploader, const char *root_dir) {
    const char *effective_root = root_dir;
    size_t len;

    if (effective_root == NULL || effective_root[0] == '\0') {
        effective_root = getenv("CAMERA_FLOW_STORE_DIR");
    }
    if (effective_root == NULL || effective_root[0] == '\0') {
        effective_root = LOCAL_STORE_DEFAULT_ROOT;
    }

    len = strlen(effective_root);
    if (len >= sizeof(uploader->root_dir)) {
        return ENAMETOOLONG;
    }

    memcpy(uploader->root_dir, effective_root, len + 1);
    return 0;
}

static void video_uploader_reset_backoff(VideoUploader *uploader) {
    uploader->retry_backoff_ms = VIDEO_UPLOADER_RETRY_BASE_MS;
}

static void video_uploader_increase_backoff(VideoUploader *uploader) {
    int next_delay = uploader->retry_backoff_ms * 2;

    if (next_delay > uploader->retry_backoff_max_ms) {
        next_delay = uploader->retry_backoff_max_ms;
    }
    uploader->retry_backoff_ms = next_delay;
}

static int video_uploader_wait(VideoUploader *uploader, int timeout_ms) {
    struct pollfd pfd;
    uint64_t stop_value;
    int rc;

    pfd.fd = uploader->stop_event_fd;
    pfd.events = POLLIN;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }

    if (rc == 0) {
        return 0;
    }

    if (pfd.revents & POLLIN) {
        if (read(uploader->stop_event_fd, &stop_value, sizeof(stop_value)) < 0 && errno != EAGAIN) {
            return -1;
        }
        return 1;
    }

    return 0;
}

static int video_uploader_http_progress_cb(void *clientp,
                                           curl_off_t dltotal,
                                           curl_off_t dlnow,
                                           curl_off_t ultotal,
                                           curl_off_t ulnow) {
    VideoUploader *uploader = (VideoUploader *)clientp;

    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;

    if (uploader != NULL && uploader->stopping) {
        return 1;
    }

    return 0;
}

static int video_uploader_handle_one(VideoUploader *uploader) {
    LocalVideoSegmentRecord segment;
    char remote_path[LOCAL_STORE_MAX_MEDIA_PATH_LEN];
    char error_msg[LOCAL_STORE_MAX_PAYLOAD_LEN];
    int found = 0;
    int rc;

    if (!uploader->store_ready) {
        return ENOSYS;
    }

    rc = local_store_fetch_oldest_pending_video_segment(&uploader->store, &segment, &found);
    if (rc != 0) {
        return rc;
    }
    if (!found) {
        return 1;
    }

    rc = local_store_mark_video_segment_uploading(&uploader->store, segment.id);
    if (rc != 0) {
        return rc;
    }

    if (uploader->upload_fn == NULL) {
        snprintf(error_msg, sizeof(error_msg), "upload callback is not configured");
        local_store_mark_video_segment_retry(&uploader->store, segment.id, error_msg);
        return ENOSYS;
    }

    remote_path[0] = '\0';
    error_msg[0] = '\0';
    rc = uploader->upload_fn(&segment,
                             remote_path,
                             sizeof(remote_path),
                             error_msg,
                             sizeof(error_msg),
                             uploader->user_data);
    if (rc == 0) {
        rc = local_store_mark_video_segment_uploaded(&uploader->store,
                                                     segment.id,
                                                     remote_path,
                                                     (int64_t)time(NULL) * 1000LL);
        if (rc == 0) {
            printf("[VideoUploader] Uploaded segment id=%lld path=%s remote=%s\n",
                   (long long)segment.id,
                   segment.file_path,
                   remote_path);
        }
        return rc;
    }

    if (error_msg[0] == '\0') {
        snprintf(error_msg, sizeof(error_msg), "upload callback failed: %d", rc);
    }

    rc = local_store_mark_video_segment_retry(&uploader->store, segment.id, error_msg);
    if (rc == 0) {
        printf("[VideoUploader] Upload failed, segment id=%lld, error=%s\n",
               (long long)segment.id,
               error_msg);
    }
    return rc;
}

static void *video_uploader_thread(void *arg) {
    VideoUploader *uploader = (VideoUploader *)arg;
    int rc;

    rc = local_store_open(&uploader->store, uploader->root_dir);
    if (rc != 0) {
        fprintf(stderr, "[VideoUploader] local_store_open failed: %d\n", rc);
        return NULL;
    }
    uploader->store_ready = 1;

    rc = local_store_reset_uploading_video_segments(&uploader->store);
    if (rc != 0) {
        fprintf(stderr, "[VideoUploader] reset uploading states failed: %d\n", rc);
    }

    printf("[VideoUploader] Started, db=%s\n", uploader->store.db_path);

    while (1) {
        int wait_rc;

        rc = video_uploader_handle_one(uploader);
        if (rc == 1) {
            wait_rc = video_uploader_wait(uploader, uploader->idle_interval_ms);
            if (wait_rc == 1) {
                break;
            }
            if (wait_rc < 0) {
                fprintf(stderr, "[VideoUploader] wait failed\n");
                break;
            }
            continue;
        }

        if (rc != 0) {
            video_uploader_increase_backoff(uploader);
            wait_rc = video_uploader_wait(uploader, uploader->retry_backoff_ms);
            if (wait_rc == 1) {
                break;
            }
            if (wait_rc < 0) {
                fprintf(stderr, "[VideoUploader] retry wait failed\n");
                break;
            }
            continue;
        }

        video_uploader_reset_backoff(uploader);
    }

    if (uploader->store_ready) {
        local_store_close(&uploader->store);
        uploader->store_ready = 0;
    }

    return NULL;
}

int video_uploader_start(VideoUploader *uploader,
                         const char *root_dir,
                         VideoUploaderUploadFn upload_fn,
                         void *user_data) {
    int rc;

    if (uploader == NULL) {
        return EINVAL;
    }

    memset(uploader, 0, sizeof(*uploader));
    uploader->stop_event_fd = -1;
    uploader->stopping = 0;
    uploader->idle_interval_ms = VIDEO_UPLOADER_IDLE_INTERVAL_MS;
    uploader->retry_backoff_ms = VIDEO_UPLOADER_RETRY_BASE_MS;
    uploader->retry_backoff_max_ms = VIDEO_UPLOADER_RETRY_MAX_MS;
    uploader->upload_fn = upload_fn;
    uploader->user_data = user_data;

    rc = video_uploader_copy_root_dir(uploader, root_dir);
    if (rc != 0) {
        return rc;
    }

    uploader->stop_event_fd = eventfd(0, EFD_CLOEXEC);
    if (uploader->stop_event_fd < 0) {
        return errno;
    }

    rc = pthread_create(&uploader->tid, NULL, video_uploader_thread, uploader);
    if (rc != 0) {
        close(uploader->stop_event_fd);
        uploader->stop_event_fd = -1;
        return rc;
    }

    uploader->started = 1;
    return 0;
}

void video_uploader_stop(VideoUploader *uploader) {
    uint64_t one = 1;

    if (uploader == NULL || !uploader->started) {
        return;
    }

    uploader->stopping = 1;

    if (write(uploader->stop_event_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        perror("video_uploader_stop write");
    }

    pthread_join(uploader->tid, NULL);
    close(uploader->stop_event_fd);
    uploader->stop_event_fd = -1;
    uploader->started = 0;
    uploader->stopping = 0;
}

int video_uploader_http_upload_callback(const LocalVideoSegmentRecord *segment,
                                        char *remote_path,
                                        size_t remote_path_size,
                                        char *error_msg,
                                        size_t error_msg_size,
                                        void *user_data) {
    VideoUploaderHttpConfig cfg;
    VideoUploaderHttpBuffer response = {0};
    CURL *curl = NULL;
    curl_mime *mime = NULL;
    curl_mimepart *part;
    struct curl_slist *headers = NULL;
    char auth_header[512];
    char segment_id_buf[32];
    char start_ms_buf[32];
    char end_ms_buf[32];
    char size_bytes_buf[32];
    long http_code = 0;
    long server_code = 0;
    int rc;

    if (segment == NULL || remote_path == NULL || error_msg == NULL) {
        return EINVAL;
    }

    remote_path[0] = '\0';
    error_msg[0] = '\0';

    if (access(segment->file_path, F_OK) != 0) {
        snprintf(error_msg, error_msg_size, "file not found: %s", segment->file_path);
        return ENOENT;
    }

    rc = video_uploader_http_fill_from_macro(&cfg);
    if (rc != 0) {
        snprintf(error_msg, error_msg_size, "upload macro is incomplete: %d", rc);
        return rc;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        snprintf(error_msg, error_msg_size, "curl_easy_init failed");
        return EIO;
    }

    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg.auth_token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Expect:");

    snprintf(segment_id_buf, sizeof(segment_id_buf), "%lld", (long long)segment->id);
    snprintf(start_ms_buf, sizeof(start_ms_buf), "%lld", (long long)segment->start_ms);
    snprintf(end_ms_buf, sizeof(end_ms_buf), "%lld", (long long)segment->end_ms);
    snprintf(size_bytes_buf, sizeof(size_bytes_buf), "%lld", (long long)segment->size_bytes);

    mime = curl_mime_init(curl);
    if (mime == NULL) {
        snprintf(error_msg, error_msg_size, "curl_mime_init failed");
        curl_easy_cleanup(curl);
        return EIO;
    }

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "device_id");
    curl_mime_data(part, cfg.device_id, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "segment_id");
    curl_mime_data(part, segment_id_buf, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "start_ms");
    curl_mime_data(part, start_ms_buf, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "end_ms");
    curl_mime_data(part, end_ms_buf, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "size_bytes");
    curl_mime_data(part, size_bytes_buf, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, segment->file_path);

    curl_easy_setopt(curl, CURLOPT_URL, cfg.upload_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg.connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg.request_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, video_uploader_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, video_uploader_http_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, user_data);

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        snprintf(error_msg, error_msg_size, "curl perform failed: %s", curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        video_uploader_buffer_reset(&response);
        return EIO;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        snprintf(error_msg, error_msg_size, "http code=%ld body=%s",
                 http_code, response.data != NULL ? response.data : "");
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        video_uploader_buffer_reset(&response);
        return EIO;
    }

    rc = video_uploader_http_json_extract_int(response.data, "code", &server_code);
    if (rc != 0 || server_code != 0) {
        snprintf(error_msg, error_msg_size, "server code parse failed, body=%s",
                 response.data != NULL ? response.data : "");
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        video_uploader_buffer_reset(&response);
        return EIO;
    }

    rc = video_uploader_http_json_extract_string(response.data,
                                                 "remote_path",
                                                 remote_path,
                                                 remote_path_size);
    if (rc != 0) {
        snprintf(error_msg, error_msg_size, "remote_path missing, body=%s",
                 response.data != NULL ? response.data : "");
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        video_uploader_buffer_reset(&response);
        return EIO;
    }

    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    video_uploader_buffer_reset(&response);
    return 0;
}
