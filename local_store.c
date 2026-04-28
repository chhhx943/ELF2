#define _POSIX_C_SOURCE 200809L

#include "local_store.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// 本地离线队列是一个轻量 SQLite 单文件库，放在 SD 卡上做断网缓存。
#define LOCAL_STORE_DB_NAME "offline.db"
#define LOCAL_STORE_MAX_ROWS 100000

static int local_store_copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return EINVAL;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int local_store_mkdirs(const char *path) {
    char tmp[PATH_MAX];
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return EINVAL;
    }

    len = strlen(path);
    if (len >= sizeof(tmp)) {
        return ENAMETOOLONG;
    }

    memcpy(tmp, path, len + 1);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    // 逐级创建目录，确保像 /mnt/sdcard/camera_flow 这种路径在首次启动时可用。
    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
            return errno;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
        return errno;
    }

    return 0;
}

static int local_store_exec(LocalStore *store, const char *sql) {
    char *errmsg = NULL;
    int rc;

    rc = sqlite3_exec(store->db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[LocalStore] sqlite exec failed: %s\n",
                errmsg != NULL ? errmsg : sqlite3_errmsg(store->db));
        sqlite3_free(errmsg);
        return EIO;
    }

    return 0;
}

static void local_store_fill_record(sqlite3_stmt *stmt, LocalStoreRecord *record) {
    const unsigned char *topic;
    const unsigned char *payload;
    const unsigned char *media_type;
    const unsigned char *media_path;

    record->id = sqlite3_column_int64(stmt, 0);
    record->created_at_ms = sqlite3_column_int64(stmt, 1);
    topic = sqlite3_column_text(stmt, 2);
    payload = sqlite3_column_text(stmt, 3);
    record->ppm = sqlite3_column_int(stmt, 4);
    record->temp = sqlite3_column_double(stmt, 5);
    record->humi = sqlite3_column_double(stmt, 6);
    record->alarm_status = sqlite3_column_int(stmt, 7);
    media_type = sqlite3_column_text(stmt, 8);
    media_path = sqlite3_column_text(stmt, 9);

    // sqlite3_column_text 返回的内存只在 stmt 生命周期内有效，这里需要拷贝出来。
    local_store_copy_string(record->topic, sizeof(record->topic), (const char *)topic);
    local_store_copy_string(record->payload, sizeof(record->payload), (const char *)payload);
    local_store_copy_string(record->media_type, sizeof(record->media_type), (const char *)media_type);
    local_store_copy_string(record->media_path, sizeof(record->media_path), (const char *)media_path);
}

static int64_t local_store_now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int local_store_debug_fetch_by_id(LocalStore *store,
                                         int64_t id,
                                         LocalStoreRecord *record,
                                         int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, created_at_ms, topic, payload, ppm, temp, humi, alarm_status, media_type, media_path "
        "FROM offline_queue WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_record(stmt, record);
        *found_out = 1;
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    *found_out = 0;
    return 0;
}

static int local_store_init_schema(LocalStore *store) {
    // payload 直接保存最终上报 JSON，网络恢复后可以原样补发，避免再次拼包。
    static const char *kSchemaSql =
        "CREATE TABLE IF NOT EXISTS offline_queue ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "created_at_ms INTEGER NOT NULL,"
        "topic TEXT NOT NULL,"
        "payload TEXT NOT NULL,"
        "ppm INTEGER NOT NULL,"
        "temp REAL NOT NULL,"
        "humi REAL NOT NULL,"
        "alarm_status INTEGER NOT NULL,"
        "media_type TEXT NOT NULL DEFAULT '',"
        "media_path TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_offline_queue_created_at "
        "ON offline_queue(created_at_ms);";

    return local_store_exec(store, kSchemaSql);
}

static int local_store_prune(LocalStore *store) {
    sqlite3_stmt *count_stmt = NULL;
    sqlite3_stmt *delete_stmt = NULL;
    int rc;
    int count = 0;

    rc = sqlite3_prepare_v2(store->db, "SELECT COUNT(*) FROM offline_queue;", -1, &count_stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    if (count <= store->max_rows) {
        return 0;
    }

    // 超出上限时删除最旧的数据，防止 SD 卡被持续离线写满。
    rc = sqlite3_prepare_v2(
        store->db,
        "DELETE FROM offline_queue WHERE id IN ("
        "SELECT id FROM offline_queue ORDER BY id ASC LIMIT ?"
        ");",
        -1,
        &delete_stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int(delete_stmt, 1, count - store->max_rows);
    rc = sqlite3_step(delete_stmt);
    sqlite3_finalize(delete_stmt);

    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_open(LocalStore *store, const char *root_dir) {
    const char *effective_root = root_dir;
    int rc;

    if (store == NULL) {
        return EINVAL;
    }

    memset(store, 0, sizeof(*store));
    store->max_rows = LOCAL_STORE_MAX_ROWS;

    if (effective_root == NULL || effective_root[0] == '\0') {
        effective_root = getenv("CAMERA_FLOW_STORE_DIR");
    }
    if (effective_root == NULL || effective_root[0] == '\0') {
        effective_root = LOCAL_STORE_DEFAULT_ROOT;
    }

    rc = local_store_mkdirs(effective_root);
    if (rc != 0) {
        return rc;
    }

    rc = snprintf(store->db_path, sizeof(store->db_path), "%s/%s", effective_root, LOCAL_STORE_DB_NAME);
    if (rc < 0 || (size_t)rc >= sizeof(store->db_path)) {
        return ENAMETOOLONG;
    }

    rc = sqlite3_open(store->db_path, &store->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[LocalStore] sqlite open failed: %s\n",
                store->db != NULL ? sqlite3_errmsg(store->db) : "unknown");
        local_store_close(store);
        return EIO;
    }

    sqlite3_busy_timeout(store->db, 1000);

    // WAL + NORMAL 在 SD 卡场景下比默认模式更适合频繁写入和异常恢复。
    rc = local_store_exec(store, "PRAGMA journal_mode=WAL;");
    if (rc != 0) {
        local_store_close(store);
        return rc;
    }

    rc = local_store_exec(store, "PRAGMA synchronous=NORMAL;");
    if (rc != 0) {
        local_store_close(store);
        return rc;
    }

    rc = local_store_init_schema(store);
    if (rc != 0) {
        local_store_close(store);
        return rc;
    }

    return 0;
}

void local_store_close(LocalStore *store) {
    if (store == NULL) {
        return;
    }

    if (store->db != NULL) {
        sqlite3_close(store->db);
        store->db = NULL;
    }
}

int local_store_enqueue(LocalStore *store,
                        int64_t created_at_ms,
                        const char *topic,
                        const char *payload,
                        int ppm,
                        double temp,
                        double humi,
                        int alarm_status,
                        const char *media_type,
                        const char *media_path) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || topic == NULL || payload == NULL) {
        return EINVAL;
    }

    // 结构化字段和完整 payload 一起落库，后面既能查业务字段，也能直接补发。
    rc = sqlite3_prepare_v2(
        store->db,
        "INSERT INTO offline_queue ("
        "created_at_ms, topic, payload, ppm, temp, humi, alarm_status, media_type, media_path"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, created_at_ms);
    sqlite3_bind_text(stmt, 2, topic, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, payload, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, ppm);
    sqlite3_bind_double(stmt, 5, temp);
    sqlite3_bind_double(stmt, 6, humi);
    sqlite3_bind_int(stmt, 7, alarm_status);
    sqlite3_bind_text(stmt, 8, media_type != NULL ? media_type : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, media_path != NULL ? media_path : "", -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return local_store_prune(store);
}

int local_store_fetch_batch(LocalStore *store,
                            LocalStoreRecord *records,
                            int max_records,
                            int *count_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    int count = 0;

    if (store == NULL || store->db == NULL || records == NULL || max_records <= 0 || count_out == NULL) {
        return EINVAL;
    }

    // 按自增 id 顺序取最旧的记录，保证补发顺序和离线写入顺序一致。
    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, created_at_ms, topic, payload, ppm, temp, humi, alarm_status, media_type, media_path "
        "FROM offline_queue ORDER BY id ASC LIMIT ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int(stmt, 1, max_records);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        local_store_fill_record(stmt, &records[count]);
        count++;
        if (count >= max_records) {
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return EIO;
    }

    *count_out = count;
    return 0;
}

int local_store_delete(LocalStore *store, int64_t id) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(store->db, "DELETE FROM offline_queue WHERE id = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_pending_count(LocalStore *store, int *count_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || count_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(store->db, "SELECT COUNT(*) FROM offline_queue;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return EIO;
    }

    *count_out = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int local_store_debug_selftest(const char *root_dir) {
    LocalStore store;
    LocalStoreRecord record;
    int rc;
    int pending_before = 0;
    int pending_after_enqueue = 0;
    int pending_after_delete = 0;
    int found = 0;
    int64_t inserted_id;
    int64_t test_ts = local_store_now_ms();
    char payload[LOCAL_STORE_MAX_PAYLOAD_LEN];

    memset(&store, 0, sizeof(store));
    memset(&record, 0, sizeof(record));

    printf("[LocalStoreTest] opening store...\n");
    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        printf("[LocalStoreTest] open failed: %d\n", rc);
        return rc;
    }

    printf("[LocalStoreTest] db path: %s\n", store.db_path);

    rc = local_store_pending_count(&store, &pending_before);
    if (rc != 0) {
        printf("[LocalStoreTest] pending_count(before) failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] pending before: %d\n", pending_before);

    rc = snprintf(payload,
                  sizeof(payload),
                  "{\"debug\":true,\"ts\":%lld,\"source\":\"local_store_selftest\"}",
                  (long long)test_ts);
    if (rc < 0 || (size_t)rc >= sizeof(payload)) {
        local_store_close(&store);
        return ENOSPC;
    }

    rc = local_store_enqueue(&store,
                             test_ts,
                             "/debug/local_store",
                             payload,
                             123,
                             25.5,
                             60.0,
                             0,
                             "none",
                             "");
    if (rc != 0) {
        printf("[LocalStoreTest] enqueue failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    inserted_id = sqlite3_last_insert_rowid(store.db);
    printf("[LocalStoreTest] enqueue ok, id=%lld\n", (long long)inserted_id);

    rc = local_store_pending_count(&store, &pending_after_enqueue);
    if (rc != 0) {
        printf("[LocalStoreTest] pending_count(after enqueue) failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] pending after enqueue: %d\n", pending_after_enqueue);

    rc = local_store_debug_fetch_by_id(&store, inserted_id, &record, &found);
    if (rc != 0) {
        printf("[LocalStoreTest] fetch_by_id failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    if (!found) {
        printf("[LocalStoreTest] inserted row not found: id=%lld\n", (long long)inserted_id);
        local_store_close(&store);
        return ENOENT;
    }

    printf("[LocalStoreTest] fetched row: id=%lld ts=%lld ppm=%d temp=%.1f humi=%.1f alarm=%d\n",
           (long long)record.id,
           (long long)record.created_at_ms,
           record.ppm,
           record.temp,
           record.humi,
           record.alarm_status);
    printf("[LocalStoreTest] topic=%s\n", record.topic);
    printf("[LocalStoreTest] payload=%s\n", record.payload);

    rc = local_store_delete(&store, inserted_id);
    if (rc != 0) {
        printf("[LocalStoreTest] delete failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] delete ok: id=%lld\n", (long long)inserted_id);

    rc = local_store_pending_count(&store, &pending_after_delete);
    if (rc != 0) {
        printf("[LocalStoreTest] pending_count(after delete) failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] pending after delete: %d\n", pending_after_delete);

    local_store_close(&store);
    printf("[LocalStoreTest] selftest done\n");
    return 0;
}

int local_store_debug_dump(const char *root_dir, int limit) {
    LocalStore store;
    LocalStoreRecord *records = NULL;
    int rc;
    int count = 0;
    int pending = 0;

    if (limit <= 0) {
        limit = 10;
    }

    records = calloc((size_t)limit, sizeof(*records));
    if (records == NULL) {
        return ENOMEM;
    }

    memset(&store, 0, sizeof(store));
    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        free(records);
        return rc;
    }

    rc = local_store_pending_count(&store, &pending);
    if (rc != 0) {
        printf("[LocalStoreDump] pending_count failed: %d\n", rc);
        local_store_close(&store);
        free(records);
        return rc;
    }

    rc = local_store_fetch_batch(&store, records, limit, &count);
    if (rc != 0) {
        printf("[LocalStoreDump] fetch failed: %d\n", rc);
        local_store_close(&store);
        free(records);
        return rc;
    }

    printf("[LocalStoreDump] db path: %s\n", store.db_path);
    printf("[LocalStoreDump] pending rows: %d, fetched: %d\n", pending, count);
    for (int i = 0; i < count; ++i) {
        printf("[LocalStoreDump] row[%d] id=%lld ts=%lld ppm=%d temp=%.1f humi=%.1f alarm=%d topic=%s\n",
               i,
               (long long)records[i].id,
               (long long)records[i].created_at_ms,
               records[i].ppm,
               records[i].temp,
               records[i].humi,
               records[i].alarm_status,
               records[i].topic);
        printf("[LocalStoreDump] row[%d] payload=%s\n", i, records[i].payload);
    }

    local_store_close(&store);
    free(records);
    return 0;
}
