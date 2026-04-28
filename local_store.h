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
int local_store_debug_selftest(const char *root_dir);
int local_store_debug_dump(const char *root_dir, int limit);

#endif
