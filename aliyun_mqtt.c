#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include "sensor_modbus.h" // 获取全局的 g_sensor_data
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

// 阿里云连接参数
#define ADDRESS     "tcp://iot-06z00be8pk7p1uz.mqtt.iothub.aliyuncs.com:1883"
#define CLIENTID    "k29ovUMboAH.0122|securemode=2,signmethod=hmacsha256,timestamp=1774788552812|"
#define USERNAME    "0122&k29ovUMboAH"
#define PASSWORD    "83de467647429c49f5887f9ca57da2aebd92764176aa02bcb9e5175ea758e035"
#define TOPIC       "/sys/k29ovUMboAH/0122/thing/event/property/post"

// 引用 main.c 中的全局运行标志，方便优雅退出
extern volatile sig_atomic_t is_running;

// 【修改点 1】函数参数增加了 int alarm
static void publish_sensor_data(MQTTClient client, float smoke, float humidity, float temp, int alarm) {
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    char payload[512];

    // 【修改点 2】JSON 中增加了 alarm_status 字段，对应阿里云的物模型
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
    pubmsg.payloadlen = strlen(payload);
    pubmsg.qos = 0; 
    pubmsg.retained = 0;

    MQTTClient_deliveryToken token;
    MQTTClient_publishMessage(client, TOPIC, &pubmsg, &token);
    
    int rc = MQTTClient_waitForCompletion(client, token, 1000L);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[阿里云告警] 数据上传失败! 错误码: %d\n", rc);
    }
}

// MQTT 独立后台线程
static void *mqtt_thread_func(void *arg) {
    printf("[阿里云] MQTT 线程启动，准备连接...\n");
    MQTTClient client;
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    int rc;

    MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    conn_opts.keepAliveInterval = 60;
    conn_opts.cleansession = 1;
    conn_opts.username = USERNAME;
    conn_opts.password = PASSWORD;

    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
        printf("[阿里云错误] 连接失败，返回码: %d\n", rc);
        return NULL;
    }
    // 【修改点 3】修改了提示信息
    printf("[阿里云] 连接平台成功！开始每1秒上报数据。\n");

    while (is_running) {
        // 1. 加锁获取最新传感器数据
        pthread_mutex_lock(&g_sensor_data.lock);
        float current_smoke = (float)g_sensor_data.ppm; // ppm 当作烟雾浓度
        float current_hum = g_sensor_data.humi;         
        float current_temp = g_sensor_data.temp;  
        // 【修改点 4】从全局结构体里读出当前的报警状态
        int current_alarm = g_sensor_data.alarm_status;      
        pthread_mutex_unlock(&g_sensor_data.lock);

        // 2. 推送上云 (加上报警状态)
        publish_sensor_data(client, current_smoke, current_hum, current_temp, current_alarm);
        
        // 3. 极简主义：睡 1 秒
        sleep(1);
    }

    MQTTClient_disconnect(client, 10000);
    MQTTClient_destroy(&client);
    return NULL;
}

// 供 main.c 调用的启动接口
int start_mqtt_reporter(void) {
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, mqtt_thread_func, NULL);
    if (rc != 0) {
        return rc;
    }

    pthread_detach(tid);
    return 0;
}
#else
int start_mqtt_reporter(void) {
    return ENOSYS;
}
#endif
