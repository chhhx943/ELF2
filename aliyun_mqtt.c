#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include "sensor_modbus.h"
#include "aliyun_mqtt.h"
#include "local_store.h"

#if defined(__has_include)
#if __has_include("MQTTClient.h")
#define HAVE_PAHO_MQTT 1
#endif
#endif

#ifndef HAVE_PAHO_MQTT
#define HAVE_PAHO_MQTT 0
#endif

#if HAVE_PAHO_MQTT
#include "MQTTClient.h"

#define ADDRESS "tcp://iot-06z00be8pk7p1uz.mqtt.iothub.aliyuncs.com:1883"
#define CLIENTID "k29ovUMboAH.0122|securemode=2,signmethod=hmacsha256,timestamp=1774788552812|"
#define USERNAME "0122&k29ovUMboAH"
#define PASSWORD "83de467647429c49f5887f9ca57da2aebd92764176aa02bcb9e5175ea758e035"
#define TOPIC "/sys/k29ovUMboAH/0122/thing/event/property/post"
#define MQTT_REPORT_INTERVAL_SEC 10
#define MQTT_OFFLINE_FLUSH_BATCH 10

typedef struct {
    int ppm;
    float temp;
    float humi;
    int alarm_status;
} SensorSnapshot;

extern volatile sig_atomic_t is_running;

static pthread_t g_mqtt_tid;
static int g_mqtt_stop_fd = -1;
static int g_mqtt_started = 0;
static int g_mqtt_force_offline = 0;

static int64_t current_time_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void read_sensor_snapshot(SensorSnapshot *snapshot) {
    pthread_mutex_lock(&g_sensor_data.lock);
    snapshot->ppm = g_sensor_data.ppm;
    snapshot->temp = g_sensor_data.temp;
    snapshot->humi = g_sensor_data.humi;
    snapshot->alarm_status = g_sensor_data.alarm_status;
    pthread_mutex_unlock(&g_sensor_data.lock);
}

static void build_debug_snapshot(SensorSnapshot *snapshot, int seq) {
    snapshot->ppm = 123 + seq;
    snapshot->temp = 25.5f + (float)seq * 0.1f;
    snapshot->humi = 60.0f + (float)seq * 0.2f;
    snapshot->alarm_status = seq % 2;
}

static int build_sensor_payload(char *payload,
                                size_t payload_size,
                                int64_t created_at_ms,
                                const SensorSnapshot *snapshot) {
    int len = snprintf(payload,
                       payload_size,
                       "{"
                       "\"id\":\"%lld\","
                       "\"version\":\"1.0\","
                       "\"params\":{"
                       "\"smokeconcentration\":%.2f,"
                       "\"Humidity\":%.2f,"
                       "\"temperature\":%.2f,"
                       "\"alarm_status\":%d"
                       "},"
                       "\"method\":\"thing.event.property.post\""
                       "}",
                       (long long)created_at_ms,
                       (double)snapshot->ppm,
                       (double)snapshot->humi,
                       (double)snapshot->temp,
                       snapshot->alarm_status);

    if (len < 0 || (size_t)len >= payload_size) {
        return ENOSPC;
    }

    return 0;
}

static int publish_payload(MQTTClient client, const char *topic, const char *payload) {
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    pubmsg.payload = (void *)payload;
    pubmsg.payloadlen = (int)strlen(payload);
    pubmsg.qos = 0;
    pubmsg.retained = 0;

    rc = MQTTClient_publishMessage(client, topic, &pubmsg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        return rc;
    }

    rc = MQTTClient_waitForCompletion(client, token, 1000L);
    return rc;
}

static int mqtt_connect_client(MQTTClient client) {
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    int rc;

    conn_opts.keepAliveInterval = 60;
    conn_opts.cleansession = 1;
    conn_opts.username = USERNAME;
    conn_opts.password = PASSWORD;

    rc = MQTTClient_connect(client, &conn_opts);
    if (rc == MQTTCLIENT_SUCCESS) {
        printf("[阿里云] 已连接，开始上传并补发离线数据。\n");
        return 0;
    }

    printf("[阿里云错误] 连接失败，返回码: %d\n", rc);
    return rc;
}

static void mqtt_disconnect_client(MQTTClient client, int *is_connected) {
    if (*is_connected) {
        MQTTClient_disconnect(client, 1000);
        *is_connected = 0;
    }
}

static int enqueue_offline_record(LocalStore *store,
                                  int store_ready,
                                  int64_t created_at_ms,
                                  const SensorSnapshot *snapshot,
                                  const char *payload) {
    int rc;

    if (!store_ready) {
        fprintf(stderr, "[离线缓存] 不可用，当前数据无法落盘。\n");
        return ENOSYS;
    }

    rc = local_store_enqueue(store,
                             created_at_ms,
                             TOPIC,
                             payload,
                             snapshot->ppm,
                             snapshot->temp,
                             snapshot->humi,
                             snapshot->alarm_status,
                             "",
                             "");
    if (rc != 0) {
        fprintf(stderr, "[离线缓存] 写入失败: %d\n", rc);
        return rc;
    }

    printf("[离线缓存] 已保存一条离线记录。\n");
    return 0;
}

static int flush_offline_records(LocalStore *store, MQTTClient client) {
    LocalStoreRecord records[MQTT_OFFLINE_FLUSH_BATCH];
    int rc;
    int count = 0;

    rc = local_store_fetch_batch(store, records, MQTT_OFFLINE_FLUSH_BATCH, &count);
    if (rc != 0) {
        fprintf(stderr, "[离线缓存] 读取待补发记录失败: %d\n", rc);
        return 0;
    }

    for (int i = 0; i < count; ++i) {
        rc = publish_payload(client, records[i].topic, records[i].payload);
        if (rc != MQTTCLIENT_SUCCESS) {
            printf("[离线补发] 补发失败，返回码: %d\n", rc);
            return -1;
        }

        rc = local_store_delete(store, records[i].id);
        if (rc != 0) {
            fprintf(stderr, "[离线缓存] 删除已补发记录失败 id=%lld, err=%d\n",
                    (long long)records[i].id, rc);
            return 0;
        }

        printf("[离线补发] 成功补发 id=%lld\n", (long long)records[i].id);
    }

    return 0;
}

static int send_or_enqueue_payload(LocalStore *store,
                                   int store_ready,
                                   MQTTClient client,
                                   int *mqtt_connected,
                                   int64_t created_at_ms,
                                   const SensorSnapshot *snapshot,
                                   const char *topic,
                                   const char *payload) {
    int rc;

    if (g_mqtt_force_offline) {
        printf("[MQTTDebug] force_offline=1，当前消息直接写入离线缓存。\n");
        return enqueue_offline_record(store, store_ready, created_at_ms, snapshot, payload);
    }

    if (client != NULL && !*mqtt_connected) {
        if (mqtt_connect_client(client) == 0) {
            *mqtt_connected = 1;
        }
    }

    if (*mqtt_connected) {
        rc = publish_payload(client, topic, payload);
        if (rc == MQTTCLIENT_SUCCESS) {
           // printf("[阿里云] 当前数据上报成功。\n");
            return 0;
        }

        printf("[阿里云错误] 当前数据上报失败，返回码: %d\n", rc);
        mqtt_disconnect_client(client, mqtt_connected);
    }

    return enqueue_offline_record(store, store_ready, created_at_ms, snapshot, payload);
}

static int mqtt_debug_enqueue_fake_record_internal(const char *root_dir,
                                                   const char *payload_in,
                                                   int seq) {
    LocalStore store;
    MQTTClient client = NULL;
    SensorSnapshot snapshot;
    char payload[LOCAL_STORE_MAX_PAYLOAD_LEN];
    int64_t created_at_ms;
    int mqtt_connected = 0;
    int store_ready = 0;
    int rc;

    memset(&store, 0, sizeof(store));
    build_debug_snapshot(&snapshot, seq);
    created_at_ms = current_time_ms();

    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        printf("[MQTTDebug] local_store_open failed: %d\n", rc);
        return rc;
    }
    store_ready = 1;

    if (payload_in != NULL && payload_in[0] != '\0') {
        rc = snprintf(payload, sizeof(payload), "%s", payload_in);
        if (rc < 0 || (size_t)rc >= sizeof(payload)) {
            local_store_close(&store);
            return ENOSPC;
        }
    } else {
        rc = build_sensor_payload(payload, sizeof(payload), created_at_ms, &snapshot);
        if (rc != 0) {
            local_store_close(&store);
            return rc;
        }
    }

    if (!g_mqtt_force_offline) {
        rc = MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
        if (rc != MQTTCLIENT_SUCCESS) {
            printf("[MQTTDebug] MQTTClient_create failed: %d\n", rc);
            client = NULL;
        }
    }

    rc = send_or_enqueue_payload(&store,
                                 store_ready,
                                 client,
                                 &mqtt_connected,
                                 created_at_ms,
                                 &snapshot,
                                 TOPIC,
                                 payload);

    mqtt_disconnect_client(client, &mqtt_connected);
    if (client != NULL) {
        MQTTClient_destroy(&client);
    }
    local_store_close(&store);
    return rc;
}

static int mqtt_debug_flush_offline_once_internal(const char *root_dir) {
    LocalStore store;
    MQTTClient client = NULL;
    int mqtt_connected = 0;
    int rc;

    memset(&store, 0, sizeof(store));

    if (g_mqtt_force_offline) {
        printf("[MQTTDebug] force_offline=1，跳过补发。\n");
        return 0;
    }

    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        printf("[MQTTDebug] local_store_open failed: %d\n", rc);
        return rc;
    }

    rc = MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTTDebug] MQTTClient_create failed: %d\n", rc);
        local_store_close(&store);
        return EIO;
    }

    rc = mqtt_connect_client(client);
    if (rc == 0) {
        mqtt_connected = 1;
        rc = flush_offline_records(&store, client);
        if (rc < 0) {
            rc = EIO;
        } else {
            rc = 0;
        }
    } else {
        rc = EIO;
    }

    mqtt_disconnect_client(client, &mqtt_connected);
    if (client != NULL) {
        MQTTClient_destroy(&client);
    }
    local_store_close(&store);
    return rc;
}

static void *mqtt_thread_func(void *arg) {
    (void)arg;

    MQTTClient client = NULL;
    LocalStore store;
    struct pollfd fds[2];
    int store_ready = 0;
    int mqtt_connected = 0;
    int timer_fd = -1;
    int rc;

    memset(&store, 0, sizeof(store));

    printf("[阿里云] MQTT 线程启动，准备连接...\n");

    rc = local_store_open(&store, NULL);
    if (rc != 0) {
        fprintf(stderr, "[离线缓存] 初始化失败: %d\n", rc);
    } else {
        store_ready = 1;
        printf("[离线缓存] SQLite 已就绪: %s\n", store.db_path);
    }

    rc = MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "[阿里云错误] 创建客户端失败，返回码: %d\n", rc);
        client = NULL;
    }

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer_fd < 0) {
        perror("timerfd_create");
        goto cleanup;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 1;
    its.it_interval.tv_sec = MQTT_REPORT_INTERVAL_SEC;

    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
        goto cleanup;
    }

    printf("[阿里云] 定时上报已启动，每%d秒触发一次。\n", MQTT_REPORT_INTERVAL_SEC);

    fds[0].fd = timer_fd;
    fds[0].events = POLLIN;
    fds[1].fd = g_mqtt_stop_fd;
    fds[1].events = POLLIN;

    while (1) {
        rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (fds[1].revents & POLLIN) {
            uint64_t stop_val;

            if (read(g_mqtt_stop_fd, &stop_val, sizeof(stop_val)) < 0 && errno != EAGAIN) {
                perror("eventfd read");
            }
            break;
        }

        if (fds[0].revents & POLLIN) {
            uint64_t expirations = 0;
            SensorSnapshot snapshot;
            int64_t created_at_ms;
            char payload[LOCAL_STORE_MAX_PAYLOAD_LEN];

            if (read(timer_fd, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) {
                if (errno != EINTR) {
                    perror("timerfd read");
                }
                continue;
            }

            if (!is_running) {
                break;
            }

            read_sensor_snapshot(&snapshot);
            created_at_ms = current_time_ms();
            rc = build_sensor_payload(payload, sizeof(payload), created_at_ms, &snapshot);
            if (rc != 0) {
                fprintf(stderr, "[阿里云错误] 构造 payload 失败: %d\n", rc);
                continue;
            }

            if (!g_mqtt_force_offline && client != NULL && !mqtt_connected) {
                if (mqtt_connect_client(client) == 0) {
                    mqtt_connected = 1;
                }
            }

            if (mqtt_connected && store_ready) {
                rc = flush_offline_records(&store, client);
                if (rc < 0) {
                    printf("[阿里云] 补发过程中掉线，当前数据转入离线缓存。\n");
                    mqtt_disconnect_client(client, &mqtt_connected);
                }
            }

            rc = send_or_enqueue_payload(&store,
                                         store_ready,
                                         client,
                                         &mqtt_connected,
                                         created_at_ms,
                                         &snapshot,
                                         TOPIC,
                                         payload);
            if (rc != 0) {
                fprintf(stderr, "[阿里云错误] 当前数据写入发送链路失败: %d\n", rc);
            }
        }
    }

cleanup:
    if (timer_fd >= 0) {
        close(timer_fd);
    }
    mqtt_disconnect_client(client, &mqtt_connected);
    if (client != NULL) {
        MQTTClient_destroy(&client);
    }
    if (store_ready) {
        local_store_close(&store);
    }
    return NULL;
}

int start_mqtt_reporter(void) {
    int rc;

    if (g_mqtt_started) {
        return 0;
    }

    g_mqtt_stop_fd = eventfd(0, EFD_CLOEXEC);
    if (g_mqtt_stop_fd < 0) {
        return errno;
    }

    rc = pthread_create(&g_mqtt_tid, NULL, mqtt_thread_func, NULL);
    if (rc != 0) {
        close(g_mqtt_stop_fd);
        g_mqtt_stop_fd = -1;
        return rc;
    }

    g_mqtt_started = 1;
    return 0;
}

void stop_mqtt_reporter(void) {
    uint64_t one = 1;

    if (!g_mqtt_started) {
        return;
    }

    if (write(g_mqtt_stop_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        perror("eventfd write");
    }

    pthread_join(g_mqtt_tid, NULL);
    close(g_mqtt_stop_fd);

    g_mqtt_stop_fd = -1;
    g_mqtt_started = 0;
}

int mqtt_debug_set_force_offline(int enabled) {
    g_mqtt_force_offline = enabled ? 1 : 0;
    printf("[MQTTDebug] force_offline=%d\n", g_mqtt_force_offline);
    return 0;
}

int mqtt_debug_enqueue_fake_record(const char *root_dir, const char *payload) {
    return mqtt_debug_enqueue_fake_record_internal(root_dir, payload, 1);
}

int mqtt_debug_flush_offline_once(const char *root_dir) {
    return mqtt_debug_flush_offline_once_internal(root_dir);
}

int mqtt_debug_run_end_to_end_test(const char *root_dir) {
    int rc;

    printf("[MQTTDebug] ===== end-to-end test start =====\n");

    rc = mqtt_debug_set_force_offline(1);
    if (rc != 0) {
        return rc;
    }

    rc = mqtt_debug_enqueue_fake_record_internal(root_dir, NULL, 1);
    if (rc != 0) {
        return rc;
    }

    rc = mqtt_debug_enqueue_fake_record_internal(root_dir, NULL, 2);
    if (rc != 0) {
        return rc;
    }

    rc = local_store_debug_dump(root_dir, 10);
    if (rc != 0) {
        return rc;
    }

    rc = mqtt_debug_set_force_offline(0);
    if (rc != 0) {
        return rc;
    }

    rc = mqtt_debug_flush_offline_once_internal(root_dir);
    if (rc != 0) {
        return rc;
    }

    rc = local_store_debug_dump(root_dir, 10);
    if (rc != 0) {
        return rc;
    }

    printf("[MQTTDebug] ===== end-to-end test done =====\n");
    return 0;
}
#else
int start_mqtt_reporter(void) {
    return ENOSYS;
}

void stop_mqtt_reporter(void) {
}

int mqtt_debug_set_force_offline(int enabled) {
    (void)enabled;
    return ENOSYS;
}

int mqtt_debug_enqueue_fake_record(const char *root_dir, const char *payload) {
    (void)root_dir;
    (void)payload;
    return ENOSYS;
}

int mqtt_debug_flush_offline_once(const char *root_dir) {
    (void)root_dir;
    return ENOSYS;
}

int mqtt_debug_run_end_to_end_test(const char *root_dir) {
    (void)root_dir;
    return ENOSYS;
}
#endif
