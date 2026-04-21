#include "sqlparser/server/server.h"
#include "sqlparser/common/util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static SqlApiServer *global_server = NULL;

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    if (global_server != NULL) {
        sqlapi_server_request_shutdown(global_server);
    }
}

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s [--host HOST] [--port PORT] [--worker-count N] [--queue-capacity N] "
            "[--schema-dir DIR] [--data-dir DIR]\n",
            program_name);
}

static int read_option_value(int argc, char *argv[], int *index, const char **value) {
    if (*index + 1 >= argc) {
        return 0;
    }
    *index += 1;
    *value = argv[*index];
    return 1;
}

int main(int argc, char *argv[]) {
    SqlApiServerConfig config;
    SqlApiServer *server = NULL;
    char error[256];
    int index;

    sqlapi_server_config_set_defaults(&config);

    for (index = 1; index < argc; index++) {
        const char *value = NULL;

        if (strcmp(argv[index], "--host") == 0) {
            if (!read_option_value(argc, argv, &index, &value)) {
                fprintf(stderr, "missing value for --host\n");
                print_usage(argv[0]);
                return 1;
            }
            config.host = value;
        } else if (strcmp(argv[index], "--port") == 0) {
            if (!read_option_value(argc, argv, &index, &value) || !parse_int_strict(value, &config.port)) {
                fprintf(stderr, "invalid value for --port\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--worker-count") == 0) {
            if (!read_option_value(argc, argv, &index, &value) || !parse_int_strict(value, &config.worker_count)) {
                fprintf(stderr, "invalid value for --worker-count\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--queue-capacity") == 0) {
            if (!read_option_value(argc, argv, &index, &value) || !parse_int_strict(value, &config.queue_capacity)) {
                fprintf(stderr, "invalid value for --queue-capacity\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--schema-dir") == 0) {
            if (!read_option_value(argc, argv, &index, &value)) {
                fprintf(stderr, "missing value for --schema-dir\n");
                return 1;
            }
            config.schema_dir = value;
        } else if (strcmp(argv[index], "--data-dir") == 0) {
            if (!read_option_value(argc, argv, &index, &value)) {
                fprintf(stderr, "missing value for --data-dir\n");
                return 1;
            }
            config.data_dir = value;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[index]);
            print_usage(argv[0]);
            return 1;
        }
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);

    if (!sqlapi_server_create(&server, &config, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    global_server = server;
    if (!sqlapi_server_start(server, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        sqlapi_server_destroy(server);
        global_server = NULL;
        return 1;
    }

    sqlapi_server_wait(server);
    sqlapi_server_destroy(server);
    global_server = NULL;
    return 0;
}
