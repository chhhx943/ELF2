#include "sensor_modbus.h"
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

// 初始化全局传感器数据结构体
SensorData g_sensor_data = {0, 0.0f, 0.0f, 0, 0, PTHREAD_MUTEX_INITIALIZER};

#if defined(__has_include)
#if __has_include(<modbus/modbus.h>)
#define HAVE_LIBMODBUS 1
#endif
#endif

#ifndef HAVE_LIBMODBUS
#define HAVE_LIBMODBUS 0
#endif

#if HAVE_LIBMODBUS
#include <modbus/modbus.h>

extern volatile sig_atomic_t is_running;

static void* sensor_thread_func(void* arg) {
    const char* path = (const char*)arg;
    
    // 1. 初始?Modbus 句柄
    modbus_t *ctx = modbus_new_rtu(path, 9600, 'N', 8, 1); 
    if (ctx == NULL) {
        printf("[传感器错误] 无法创建 Modbus 句柄\n");
        return NULL;
    }
    
    modbus_set_slave(ctx, 1); // 默认从机地址?1

    // ==========================================
    // 【关键修?1】把连接放到循环外面！只连接一次！
    // ==========================================
    if (modbus_connect(ctx) == -1) {
        printf("[传感器致命错误] 串口 %s 连接失败: %s\n", path, modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }
    printf("[传感器] 成功连接到串? %s\n", path);

    // 设置响应超时时间?500 毫秒 (防止死等)
    modbus_set_response_timeout(ctx, 0, 500000);

    while (is_running) {
        uint16_t regs[10] = {0};
        int hw_alarm = 0; 
        int rc;

        // ==========================================
        // 【关键修?2】如果读取失败，打印出具体的错误原因?
        // ==========================================

        // 1. 读取浓度与硬件报警位
        rc = modbus_read_registers(ctx, 0x0016, 6, regs);
        if (rc != -1) {
            pthread_mutex_lock(&g_sensor_data.lock);
            g_sensor_data.ppm = regs[0];  
            hw_alarm = regs[5];           
            pthread_mutex_unlock(&g_sensor_data.lock);
        } else {
            printf("[传感器警告] 读取浓度(0x0016)失败: %s\n", modbus_strerror(errno));
        }

        // 2. 读取运行/预热状?
        rc = modbus_read_registers(ctx, 0x001C, 1, regs);
        if (rc != -1) {
            pthread_mutex_lock(&g_sensor_data.lock);
            g_sensor_data.run_status = regs[0];
            pthread_mutex_unlock(&g_sensor_data.lock);
        }

        // 3. 读取温湿?
        rc = modbus_read_registers(ctx, 0x001E, 2, regs);
        if (rc != -1) {
            pthread_mutex_lock(&g_sensor_data.lock);
            g_sensor_data.temp = (float)regs[0] * 0.1f; 
            g_sensor_data.humi = (float)regs[1] * 0.1f; 
            pthread_mutex_unlock(&g_sensor_data.lock);
        } else {
            // 避免刷屏，只打印部分错误
            // printf("[传感器警告] 读取温湿?0x001E)失败: %s\n", modbus_strerror(errno));
        }

        // 4. 报警判断逻辑
        int is_dangerous = 0;
        pthread_mutex_lock(&g_sensor_data.lock);
        if (g_sensor_data.temp > 55.0 || g_sensor_data.ppm > 300 || hw_alarm == 1) {
            is_dangerous = 1;
        }
        g_sensor_data.alarm_status = is_dangerous; 
        pthread_mutex_unlock(&g_sensor_data.lock);

        // 用 poll 做可中断等待，避免 usleep 在收到信号时行为不够直观。
        poll(NULL, 0, 500);
    }
    
    modbus_close(ctx);
    modbus_free(ctx);
    return NULL;
}

int start_sensor_collector(const char* device_path) {
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, sensor_thread_func, (void*)device_path);
    if (rc != 0) {
        return rc;
    }

    pthread_detach(tid);
    return 0;
}
#else
int start_sensor_collector(const char* device_path) {
    (void)device_path;
    return ENOSYS;
}
#endif
