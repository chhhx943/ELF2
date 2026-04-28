#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "local_store.h"

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s selftest [root_dir]\n", prog);
    printf("  %s dump [root_dir] [limit]\n", prog);
}

int main(int argc, char *argv[]) {
    const char *cmd = "selftest";
    const char *root_dir = NULL;
    int limit = 10;
    int rc;

    if (argc >= 2) {
        cmd = argv[1];
    }

    if (strcmp(cmd, "selftest") == 0) {
        if (argc >= 3) {
            root_dir = argv[2];
        }
        printf("=== Local Store Selftest ===\n");
        rc = local_store_debug_selftest(root_dir);
        if (rc != 0) {
            printf("selftest failed: %d\n", rc);
            return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "dump") == 0) {
        if (argc >= 3) {
            root_dir = argv[2];
        }
        if (argc >= 4) {
            limit = atoi(argv[3]);
        }
        printf("=== Local Store Dump ===\n");
        rc = local_store_debug_dump(root_dir, limit);
        if (rc != 0) {
            printf("dump failed: %d\n", rc);
            return 1;
        }
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
