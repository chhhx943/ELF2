#ifndef ALIYUN_MQTT_H
#define ALIYUN_MQTT_H

// 启动阿里云 MQTT 上报线程
int start_mqtt_reporter(void);
void stop_mqtt_reporter(void);
int mqtt_debug_set_force_offline(int enabled);
int mqtt_debug_enqueue_fake_record(const char *root_dir, const char *payload);
int mqtt_debug_flush_offline_once(const char *root_dir);
int mqtt_debug_run_end_to_end_test(const char *root_dir);

#endif
