#ifndef SENSOR_MODBUS_H
#define SENSOR_MODBUS_H

#include <pthread.h>

typedef struct {
    int ppm;           // 气体浓度 (PPM)
    float temp;        // 温度 (°C)
    float humi;        // 湿度 (%RH)
    int alarm_status;  // 报警状? (0:正常, 1:报警) 
    int run_status;    // 运行状? (0:正常, 1:延迟?, 2:校准?) 
    pthread_mutex_t lock;
} SensorData;

extern SensorData g_sensor_data;

int start_sensor_collector(const char* device_path);

#endif
