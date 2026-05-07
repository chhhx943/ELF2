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

#define SENSOR_READ_FAIL_RECONNECT_THRESHOLD 3
#define SENSOR_RECONNECT_WAIT_MS 1000
#define SENSOR_READ_WAIT_MS 500
#define SENSOR_WARN_EVERY_N 10

static void sensor_clear_alarm_status(void) {
    pthread_mutex_lock(&g_sensor_data.lock);
    g_sensor_data.alarm_status = 0;
    pthread_mutex_unlock(&g_sensor_data.lock);
}

static int sensor_modbus_connect_once(modbus_t *ctx) {
    int rc;

    rc = modbus_connect(ctx);
    if (rc == -1) {
        return -1;
    }

    modbus_set_response_timeout(ctx, 0, 500000);
    return 0;
}

static void sensor_log_read_failure(const char *label,
                                    int err,
                                    int consecutive_failures,
                                    int *warn_count) {
    if (warn_count == NULL) {
        return;
    }

    (*warn_count)++;
    if (*warn_count == 1 || (*warn_count % SENSOR_WARN_EVERY_N) == 0) {
        printf("[传感器警告] %s失败: %s (连续失败=%d, 累计警告=%d)\n",
               label,
               modbus_strerror(err),
               consecutive_failures,
               *warn_count);
    }
}

static void* sensor_thread_func(void* arg) {
    const char* path = (const char*)arg;
    int connected = 0;
    int consecutive_failures = 0;
    int read_warn_count = 0;
    int reconnect_warn_count = 0;
    
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
    if (sensor_modbus_connect_once(ctx) == -1) {
        printf("[传感器致命错误] 串口 %s 连接失败: %s\n", path, modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }
    connected = 1;
    printf("[传感器] 成功连接到串? %s\n", path);

    while (is_running) {
        uint16_t regs[10] = {0};
        int hw_alarm = 0;
        int concentration_ok = 0;
        int reconnect_needed = 0;
        int rc;

        if (!connected) {
            sensor_clear_alarm_status();
            if (sensor_modbus_connect_once(ctx) == -1) {
                reconnect_warn_count++;
                if (reconnect_warn_count == 1 ||
                    (reconnect_warn_count % SENSOR_WARN_EVERY_N) == 0) {
                    printf("[传感器警告] 串口 %s 重连失败: %s (累计=%d)\n",
                           path,
                           modbus_strerror(errno),
                           reconnect_warn_count);
                }
                poll(NULL, 0, SENSOR_RECONNECT_WAIT_MS);
                continue;
            }

            connected = 1;
            consecutive_failures = 0;
            read_warn_count = 0;
            reconnect_warn_count = 0;
            printf("[传感器] 串口 %s 重连成功\n", path);
        }

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
            concentration_ok = 1;
            consecutive_failures = 0;
            read_warn_count = 0;
        } else {
            int saved_errno = errno;
            consecutive_failures++;
            sensor_log_read_failure("读取浓度(0x0016)",
                                    saved_errno,
                                    consecutive_failures,
                                    &read_warn_count);
        }

        // 2. 读取运行/预热状?
        if (consecutive_failures < SENSOR_READ_FAIL_RECONNECT_THRESHOLD) {
            rc = modbus_read_registers(ctx, 0x001C, 1, regs);
            if (rc != -1) {
                pthread_mutex_lock(&g_sensor_data.lock);
                g_sensor_data.run_status = regs[0];
                pthread_mutex_unlock(&g_sensor_data.lock);
                consecutive_failures = 0;
                read_warn_count = 0;
            } else {
                int saved_errno = errno;
                consecutive_failures++;
                sensor_log_read_failure("读取运行状态(0x001C)",
                                        saved_errno,
                                        consecutive_failures,
                                        &read_warn_count);
            }
        }

        // 3. 读取温湿?
        if (consecutive_failures < SENSOR_READ_FAIL_RECONNECT_THRESHOLD) {
            rc = modbus_read_registers(ctx, 0x001E, 2, regs);
            if (rc != -1) {
                pthread_mutex_lock(&g_sensor_data.lock);
                g_sensor_data.temp = (float)regs[0] * 0.1f; 
                g_sensor_data.humi = (float)regs[1] * 0.1f; 
                pthread_mutex_unlock(&g_sensor_data.lock);
                consecutive_failures = 0;
                read_warn_count = 0;
            } else {
                int saved_errno = errno;
                consecutive_failures++;
                sensor_log_read_failure("读取温湿度(0x001E)",
                                        saved_errno,
                                        consecutive_failures,
                                        &read_warn_count);
            }
        }

        if (consecutive_failures >= SENSOR_READ_FAIL_RECONNECT_THRESHOLD) {
            reconnect_needed = 1;
        }

        if (reconnect_needed) {
            printf("[传感器警告] 连续读取失败达到 %d 次，关闭并准备重连串口 %s\n",
                   consecutive_failures,
                   path);
            sensor_clear_alarm_status();
            modbus_close(ctx);
            connected = 0;
            consecutive_failures = 0;
            poll(NULL, 0, SENSOR_READ_WAIT_MS);
            continue;
        }

        // 4. 报警判断逻辑
        int is_dangerous = 0;
        if (concentration_ok) {
            pthread_mutex_lock(&g_sensor_data.lock);
            if (g_sensor_data.temp > 55.0 || g_sensor_data.ppm > 300 || hw_alarm == 1) {
                is_dangerous = 1;
            }
            g_sensor_data.alarm_status = is_dangerous; 
            pthread_mutex_unlock(&g_sensor_data.lock);
        } else {
            sensor_clear_alarm_status();
        }

        // 用 poll 做可中断等待，避免 usleep 在收到信号时行为不够直观。
        poll(NULL, 0, SENSOR_READ_WAIT_MS);
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
