#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aliyun_mqtt.h"
#include "local_store.h"
#include "sensor_modbus.h"

volatile sig_atomic_t is_running = 1;
SensorData g_sensor_data = {0, 0.0f, 0.0f, 0, 0, PTHREAD_MUTEX_INITIALIZER};

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s offline-test [root_dir]\n", prog);
    printf("  %s flush-test [root_dir]\n", prog);
    printf("  %s full-test [root_dir]\n", prog);
    printf("  %s dump [root_dir] [limit]\n", prog);
}

int main(int argc, char *argv[]) {
    const char *cmd;
    const char *root_dir = NULL;
    int limit = 10;
    int rc;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    cmd = argv[1];
    if (argc >= 3) {
        root_dir = argv[2];
    }

    if (strcmp(cmd, "offline-test") == 0) {
        rc = mqtt_debug_set_force_offline(1);
        if (rc != 0) {
            return 1;
        }

        rc = mqtt_debug_enqueue_fake_record(root_dir, NULL);
        if (rc != 0) {
            printf("offline-test enqueue #1 failed: %d\n", rc);
            return 1;
        }

        rc = mqtt_debug_enqueue_fake_record(root_dir, NULL);
        if (rc != 0) {
            printf("offline-test enqueue #2 failed: %d\n", rc);
            return 1;
        }

        rc = local_store_debug_dump(root_dir, 10);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(cmd, "flush-test") == 0) {
        rc = mqtt_debug_set_force_offline(0);
        if (rc != 0) {
            return 1;
        }

        rc = mqtt_debug_flush_offline_once(root_dir);
        if (rc != 0) {
            printf("flush-test failed: %d\n", rc);
            return 1;
        }

        rc = local_store_debug_dump(root_dir, 10);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(cmd, "full-test") == 0) {
        rc = mqtt_debug_run_end_to_end_test(root_dir);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(cmd, "dump") == 0) {
        if (argc >= 4) {
            limit = atoi(argv[3]);
        }
        rc = local_store_debug_dump(root_dir, limit);
        return rc == 0 ? 0 : 1;
    }

    print_usage(argv[0]);
    return 1;
}
