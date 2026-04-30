#define _POSIX_C_SOURCE 200809L

#include "local_store.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// 本地离线队列是一个轻量 SQLite 单文件库，放在 SD 卡上做断网缓存。
#define LOCAL_STORE_DB_NAME "offline.db"
#define LOCAL_STORE_MAX_ROWS 100000
#define LOCAL_STORE_INIT_LOCK_NAME ".offline.db.init.lock"

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

static int local_store_open_init_lock(const char *root_dir, char *lock_path, size_t lock_path_size) {
    int fd;
    int rc;

    if (root_dir == NULL || lock_path == NULL || lock_path_size == 0) {
        return -1;
    }

    rc = snprintf(lock_path, lock_path_size, "%s/%s", root_dir, LOCAL_STORE_INIT_LOCK_NAME);
    if (rc < 0 || (size_t)rc >= lock_path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(lock_path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        return -1;
    }

    if (flock(fd, LOCK_EX) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void local_store_close_init_lock(int fd) {
    if (fd < 0) {
        return;
    }

    flock(fd, LOCK_UN);
    close(fd);
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

static void local_store_fill_video_segment_record(sqlite3_stmt *stmt, LocalVideoSegmentRecord *record) {
    const unsigned char *file_path;
    const unsigned char *state;
    const unsigned char *last_error;
    const unsigned char *remote_path;

    record->id = sqlite3_column_int64(stmt, 0);
    record->start_ms = sqlite3_column_int64(stmt, 1);
    record->end_ms = sqlite3_column_int64(stmt, 2);
    record->size_bytes = sqlite3_column_int64(stmt, 3);
    record->retry_count = sqlite3_column_int(stmt, 4);
    record->uploaded_at_ms = sqlite3_column_int64(stmt, 5);
    state = sqlite3_column_text(stmt, 6);
    file_path = sqlite3_column_text(stmt, 7);
    record->created_at_ms = sqlite3_column_int64(stmt, 8);
    last_error = sqlite3_column_text(stmt, 9);
    remote_path = sqlite3_column_text(stmt, 10);

    local_store_copy_string(record->state, sizeof(record->state), (const char *)state);
    local_store_copy_string(record->file_path, sizeof(record->file_path), (const char *)file_path);
    local_store_copy_string(record->last_error, sizeof(record->last_error), (const char *)last_error);
    local_store_copy_string(record->remote_path, sizeof(record->remote_path), (const char *)remote_path);
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
        "CREATE TABLE IF NOT EXISTS video_segments ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "start_ms INTEGER NOT NULL,"
        "end_ms INTEGER NOT NULL DEFAULT 0,"
        "size_bytes INTEGER NOT NULL DEFAULT 0,"
        "retry_count INTEGER NOT NULL DEFAULT 0,"
        "uploaded_at_ms INTEGER NOT NULL DEFAULT 0,"
        "state TEXT NOT NULL,"
        "file_path TEXT NOT NULL,"
        "last_error TEXT NOT NULL DEFAULT '',"
        "remote_path TEXT NOT NULL DEFAULT '',"
        "created_at_ms INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_offline_queue_created_at "
        "ON offline_queue(created_at_ms);"
        "CREATE INDEX IF NOT EXISTS idx_video_segments_start_ms "
        "ON video_segments(start_ms);"
        "CREATE INDEX IF NOT EXISTS idx_video_segments_state "
        "ON video_segments(state);";

    return local_store_exec(store, kSchemaSql);
}

static int local_store_column_exists(LocalStore *store,
                                     const char *table_name,
                                     const char *column_name,
                                     int *exists_out) {
    sqlite3_stmt *stmt = NULL;
    char sql[128];
    int rc;

    if (store == NULL || store->db == NULL || table_name == NULL || column_name == NULL || exists_out == NULL) {
        return EINVAL;
    }

    rc = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table_name);
    if (rc < 0 || (size_t)rc >= sizeof(sql)) {
        return ENAMETOOLONG;
    }

    rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    *exists_out = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name != NULL && strcmp((const char *)name, column_name) == 0) {
            *exists_out = 1;
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return EIO;
    }

    return 0;
}

static int local_store_ensure_video_segments_columns(LocalStore *store) {
    struct ColumnPatch {
        const char *name;
        const char *sql;
    };
    static const struct ColumnPatch patches[] = {
        { "retry_count", "ALTER TABLE video_segments ADD COLUMN retry_count INTEGER NOT NULL DEFAULT 0;" },
        { "uploaded_at_ms", "ALTER TABLE video_segments ADD COLUMN uploaded_at_ms INTEGER NOT NULL DEFAULT 0;" },
        { "last_error", "ALTER TABLE video_segments ADD COLUMN last_error TEXT NOT NULL DEFAULT '';" },
        { "remote_path", "ALTER TABLE video_segments ADD COLUMN remote_path TEXT NOT NULL DEFAULT '';" },
    };
    int exists = 0;
    int exists_after = 0;
    int rc;

    for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); ++i) {
        rc = local_store_column_exists(store, "video_segments", patches[i].name, &exists);
        if (rc != 0) {
            return rc;
        }
        if (!exists) {
            rc = local_store_exec(store, patches[i].sql);
            if (rc != 0) {
                // 多进程/多线程同时启动时，另一方可能刚好先一步把列补上。
                // 这里复查一次当前列是否已经存在，存在就把这次迁移视为成功。
                rc = local_store_column_exists(store, "video_segments", patches[i].name, &exists_after);
                if (rc != 0) {
                    return rc;
                }
                if (!exists_after) {
                    return EIO;
                }
            }
        }
    }

    return 0;
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
    char lock_path[PATH_MAX];
    int lock_fd = -1;
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

    lock_fd = local_store_open_init_lock(effective_root, lock_path, sizeof(lock_path));
    if (lock_fd < 0) {
        rc = errno != 0 ? errno : EIO;
        return rc;
    }

    rc = sqlite3_open(store->db_path, &store->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[LocalStore] sqlite open failed: %s\n",
                store->db != NULL ? sqlite3_errmsg(store->db) : "unknown");
        local_store_close(store);
        rc = EIO;
        goto unlock_and_exit;
    }

    sqlite3_busy_timeout(store->db, 1000);

    // WAL + NORMAL 在 SD 卡场景下比默认模式更适合频繁写入和异常恢复。
    rc = local_store_exec(store, "PRAGMA journal_mode=WAL;");
    if (rc != 0) {
        local_store_close(store);
        goto unlock_and_exit;
    }

    rc = local_store_exec(store, "PRAGMA synchronous=NORMAL;");
    if (rc != 0) {
        local_store_close(store);
        goto unlock_and_exit;
    }

    rc = local_store_init_schema(store);
    if (rc != 0) {
        local_store_close(store);
        goto unlock_and_exit;
    }

    rc = local_store_ensure_video_segments_columns(store);
    if (rc != 0) {
        local_store_close(store);
        goto unlock_and_exit;
    }

unlock_and_exit:
    local_store_close_init_lock(lock_fd);
    return rc;
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

int local_store_register_video_segment(LocalStore *store,
                                       int64_t start_ms,
                                       const char *file_path,
                                       const char *state,
                                       int64_t *segment_id_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || file_path == NULL || state == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "INSERT INTO video_segments (start_ms, state, file_path, created_at_ms) "
        "VALUES (?, ?, ?, ?);",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, start_ms);
    sqlite3_bind_text(stmt, 2, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, file_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, local_store_now_ms());

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    if (segment_id_out != NULL) {
        *segment_id_out = sqlite3_last_insert_rowid(store->db);
    }

    return 0;
}

int local_store_update_video_segment(LocalStore *store,
                                     int64_t segment_id,
                                     int64_t end_ms,
                                     int64_t size_bytes,
                                     const char *state) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || state == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments SET end_ms = ?, size_bytes = ?, state = ? WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, end_ms);
    sqlite3_bind_int64(stmt, 2, size_bytes);
    sqlite3_bind_text(stmt, 3, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, segment_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_delete_video_segment(LocalStore *store, int64_t segment_id) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(store->db, "DELETE FROM video_segments WHERE id = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_fetch_oldest_prunable_video_segment(LocalStore *store,
                                                    LocalVideoSegmentRecord *record,
                                                    int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments "
        "WHERE state IN ('uploaded', 'pending', 'broken') "
        "ORDER BY start_ms ASC LIMIT 1;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_video_segment_record(stmt, record);
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

int local_store_fetch_oldest_pending_video_segment(LocalStore *store,
                                                   LocalVideoSegmentRecord *record,
                                                   int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments "
        "WHERE state = 'pending' "
        "ORDER BY start_ms ASC LIMIT 1;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_video_segment_record(stmt, record);
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

int local_store_video_total_size(LocalStore *store, int64_t *bytes_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || bytes_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT COALESCE(SUM(size_bytes), 0) FROM video_segments "
        "WHERE state IN ('recording', 'pending', 'uploaded', 'broken');",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return EIO;
    }

    *bytes_out = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int local_store_mark_video_segment_uploading(LocalStore *store, int64_t segment_id) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments SET state = 'uploading' WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_mark_video_segment_uploaded(LocalStore *store,
                                            int64_t segment_id,
                                            const char *remote_path,
                                            int64_t uploaded_at_ms) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments "
        "SET state = 'uploaded', uploaded_at_ms = ?, remote_path = ?, last_error = '' "
        "WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, uploaded_at_ms);
    sqlite3_bind_text(stmt, 2, remote_path != NULL ? remote_path : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_mark_video_segment_retry(LocalStore *store,
                                         int64_t segment_id,
                                         const char *last_error) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments "
        "SET state = 'pending', retry_count = retry_count + 1, last_error = ? "
        "WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_text(stmt, 1, last_error != NULL ? last_error : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_reset_uploading_video_segments(LocalStore *store) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments SET state = 'pending' WHERE state = 'uploading';",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_debug_fetch_video_segment_by_id(LocalStore *store,
                                                int64_t id,
                                                LocalVideoSegmentRecord *record,
                                                int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_video_segment_record(stmt, record);
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

int local_store_debug_dump_video_segments(const char *root_dir, int limit) {
    LocalStore store;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int count = 0;

    if (limit <= 0) {
        limit = 10;
    }

    memset(&store, 0, sizeof(store));
    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        return rc;
    }

    rc = sqlite3_prepare_v2(
        store.db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments ORDER BY id DESC LIMIT ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        local_store_close(&store);
        return EIO;
    }

    sqlite3_bind_int(stmt, 1, limit);
    printf("[LocalStoreVideoDump] db path: %s\n", store.db_path);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        LocalVideoSegmentRecord record;
        memset(&record, 0, sizeof(record));
        local_store_fill_video_segment_record(stmt, &record);
        printf("[LocalStoreVideoDump] row[%d] id=%lld state=%s retry=%d size=%lld path=%s remote=%s err=%s\n",
               count,
               (long long)record.id,
               record.state,
               record.retry_count,
               (long long)record.size_bytes,
               record.file_path,
               record.remote_path,
               record.last_error);
        count++;
    }

    sqlite3_finalize(stmt);
    local_store_close(&store);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return EIO;
    }

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
