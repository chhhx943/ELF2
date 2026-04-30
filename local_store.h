#ifndef LOCAL_STORE_H
#define LOCAL_STORE_H

#include <limits.h>
#include <stdint.h>

typedef struct sqlite3 sqlite3;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define LOCAL_STORE_DEFAULT_ROOT "/media/elf/2461-4BDA/camera_flow"
#define LOCAL_STORE_MAX_TOPIC_LEN 256
#define LOCAL_STORE_MAX_PAYLOAD_LEN 2048
#define LOCAL_STORE_MAX_MEDIA_TYPE_LEN 32
#define LOCAL_STORE_MAX_MEDIA_PATH_LEN 512
#define LOCAL_STORE_MAX_SEGMENT_STATE_LEN 32

typedef struct {
    sqlite3 *db;
    char db_path[PATH_MAX];
    int max_rows;
} LocalStore;

typedef struct {
    int64_t id;
    int64_t created_at_ms;
    int ppm;
    double temp;
    double humi;
    int alarm_status;
    char topic[LOCAL_STORE_MAX_TOPIC_LEN];
    char payload[LOCAL_STORE_MAX_PAYLOAD_LEN];
    char media_type[LOCAL_STORE_MAX_MEDIA_TYPE_LEN];
    char media_path[LOCAL_STORE_MAX_MEDIA_PATH_LEN];
} LocalStoreRecord;

typedef struct {
    int64_t id;
    int64_t start_ms;
    int64_t end_ms;
    int64_t size_bytes;
    int64_t created_at_ms;
    int retry_count;
    int64_t uploaded_at_ms;
    char file_path[LOCAL_STORE_MAX_MEDIA_PATH_LEN];
    char state[LOCAL_STORE_MAX_SEGMENT_STATE_LEN];
    char last_error[LOCAL_STORE_MAX_PAYLOAD_LEN];
    char remote_path[LOCAL_STORE_MAX_MEDIA_PATH_LEN];
} LocalVideoSegmentRecord;

int local_store_open(LocalStore *store, const char *root_dir);
void local_store_close(LocalStore *store);
int local_store_enqueue(LocalStore *store,
                        int64_t created_at_ms,
                        const char *topic,
                        const char *payload,
                        int ppm,
                        double temp,
                        double humi,
                        int alarm_status,
                        const char *media_type,
                        const char *media_path);
int local_store_fetch_batch(LocalStore *store,
                            LocalStoreRecord *records,
                            int max_records,
                            int *count_out);
int local_store_delete(LocalStore *store, int64_t id);
int local_store_pending_count(LocalStore *store, int *count_out);

int local_store_register_video_segment(LocalStore *store,
                                       int64_t start_ms,
                                       const char *file_path,
                                       const char *state,
                                       int64_t *segment_id_out);
int local_store_update_video_segment(LocalStore *store,
                                     int64_t segment_id,
                                     int64_t end_ms,
                                     int64_t size_bytes,
                                     const char *state);
int local_store_delete_video_segment(LocalStore *store, int64_t segment_id);
int local_store_fetch_oldest_prunable_video_segment(LocalStore *store,
                                                    LocalVideoSegmentRecord *record,
                                                    int *found_out);
int local_store_fetch_oldest_pending_video_segment(LocalStore *store,
                                                   LocalVideoSegmentRecord *record,
                                                   int *found_out);
int local_store_video_total_size(LocalStore *store, int64_t *bytes_out);
int local_store_mark_video_segment_uploading(LocalStore *store, int64_t segment_id);
int local_store_mark_video_segment_uploaded(LocalStore *store,
                                            int64_t segment_id,
                                            const char *remote_path,
                                            int64_t uploaded_at_ms);
int local_store_mark_video_segment_retry(LocalStore *store,
                                         int64_t segment_id,
                                         const char *last_error);
int local_store_reset_uploading_video_segments(LocalStore *store);
int local_store_debug_fetch_video_segment_by_id(LocalStore *store,
                                                int64_t id,
                                                LocalVideoSegmentRecord *record,
                                                int *found_out);
int local_store_debug_dump_video_segments(const char *root_dir, int limit);

int local_store_debug_selftest(const char *root_dir);
int local_store_debug_dump(const char *root_dir, int limit);

#endif
