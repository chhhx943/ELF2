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

#define ADDRESS  "tcp://iot-06z00be8pk7p1uz.mqtt.iothub.aliyuncs.com:1883"
#define CLIENTID "k29ovUMboAH.0122|securemode=2,signmethod=hmacsha256,timestamp=1774788552812|"
#define USERNAME "0122&k29ovUMboAH"
#define PASSWORD "83de467647429c49f5887f9ca57da2aebd92764176aa02bcb9e5175ea758e035"
#define TOPIC    "/sys/k29ovUMboAH/0122/thing/event/property/post"

extern volatile sig_atomic_t is_running;

static pthread_t g_mqtt_tid;
static int g_mqtt_stop_fd = -1;
static int g_mqtt_started = 0;

static void publish_sensor_data(MQTTClient client, float smoke, float humidity, float temp, int alarm) {
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    char payload[512];

    snprintf(payload, sizeof(payload),
             "{"
             "\"id\":\"12345\","
             "\"version\":\"1.0\","
             "\"params\":{"
             "\"smokeconcentration\":%.2f,"
             "\"Humidity\":%.2f,"
             "\"temperature\":%.2f,"
             "\"alarm_status\":%d"
             "},"
             "\"method\":\"thing.event.property.post\""
             "}",
             smoke, humidity, temp, alarm);

    pubmsg.payload = payload;
    pubmsg.payloadlen = (int)strlen(payload);
    pubmsg.qos = 0;
    pubmsg.retained = 0;

    MQTTClient_deliveryToken token;
    MQTTClient_publishMessage(client, TOPIC, &pubmsg, &token);

    int rc = MQTTClient_waitForCompletion(client, token, 1000L);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[阿里云告警] 数据上传失败! 错误码: %d\n", rc);
    }
}

static void *mqtt_thread_func(void *arg) {
    (void)arg;

    MQTTClient client = NULL;
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    struct pollfd fds[2];
    int timer_fd = -1;
    int rc;

    printf("[阿里云] MQTT 线程启动，准备连接...\n");

    rc = MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[阿里云错误] 创建客户端失败，返回码: %d\n", rc);
        return NULL;
    }

    conn_opts.keepAliveInterval = 60;
    conn_opts.cleansession = 1;
    conn_opts.username = USERNAME;
    conn_opts.password = PASSWORD;

    rc = MQTTClient_connect(client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[阿里云错误] 连接失败，返回码: %d\n", rc);
        goto cleanup;
    }

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer_fd < 0) {
        perror("timerfd_create");
        goto cleanup;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 1;
    its.it_interval.tv_sec = 10;

    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
        goto cleanup;
    }

    printf("[阿里云] 连接平台成功！开始每10秒上报数据。\n");

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

            if (read(timer_fd, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) {
                if (errno != EINTR) {
                    perror("timerfd read");
                }
                continue;
            }

            if (!is_running) {
                break;
            }

            pthread_mutex_lock(&g_sensor_data.lock);
            float current_smoke = (float)g_sensor_data.ppm;
            float current_hum = g_sensor_data.humi;
            float current_temp = g_sensor_data.temp;
            int current_alarm = g_sensor_data.alarm_status;
            pthread_mutex_unlock(&g_sensor_data.lock);

            publish_sensor_data(client, current_smoke, current_hum, current_temp, current_alarm);
        }
    }

cleanup:
    if (timer_fd >= 0) {
        close(timer_fd);
    }
    if (client != NULL) {
        MQTTClient_disconnect(client, 10000);
        MQTTClient_destroy(&client);
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
#else
int start_mqtt_reporter(void) {
    return ENOSYS;
}

void stop_mqtt_reporter(void) {
}
#endif
