/*
 * app/sqlapi_server_main.c
 *
 * API 서버 전용 실행 파일의 시작점이다.
 *
 * 이 파일의 책임은 다음으로 제한한다.
 * - CLI 옵션을 읽어 SqlApiServerConfig를 채운다.
 * - 종료 시그널을 받아 서버 shutdown 요청으로 연결한다.
 * - 서버 객체의 생성/시작/대기/파괴 순서를 조율한다.
 *
 * 실제 요청 처리, HTTP 파싱, SQL 실행은 각각 server/http/service/engine
 * 계층으로 내려 보내고 여기서는 실행 흐름만 제어한다.
 */
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

/* SIGINT/SIGTERM을 받으면 main loop 대신 서버 쪽 graceful shutdown을 요청한다. */
static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    if (global_server != NULL) {
        sqlapi_server_request_shutdown(global_server);
    }
}

/* 잘못된 옵션 입력 시 서버 전용 CLI 사용법을 stderr로 출력한다. */
static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s [--host HOST] [--port PORT] [--worker-count N] [--queue-capacity N] "
            "[--schema-dir DIR] [--data-dir DIR]\n",
            program_name);
}

/* 값이 뒤따르는 옵션에서 다음 argv를 안전하게 꺼내는 작은 헬퍼다. */
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

    /* 문서에 정의된 기본 listen/worker/path 설정으로 시작한다. */
    sqlapi_server_config_set_defaults(&config);

    for (index = 1; index < argc; index++) {
        const char *value = NULL;

        if (strcmp(argv[index], "--host") == 0) {
            /* listen 주소는 문자열 옵션이므로 다음 argv를 그대로 받아 config에 저장한다. */
            if (!read_option_value(argc, argv, &index, &value)) {
                fprintf(stderr, "missing value for --host\n");
                print_usage(argv[0]);
                return 1;
            }
            config.host = value;
        } else if (strcmp(argv[index], "--port") == 0) {
            /*
             * 포트는 숫자 범주 옵션이므로
             * "값이 존재하는지"와 "정확한 정수 문자열인지"를 여기서 함께 확인한다.
             * 실제 허용 범위(1..65535) 검증은 create 단계의 config validation이 담당한다.
             */
            if (!read_option_value(argc, argv, &index, &value) || !parse_int_strict(value, &config.port)) {
                fprintf(stderr, "invalid value for --port\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--worker-count") == 0) {
            /*
             * worker 수 역시 정수 옵션이다.
             * 여기서는 파싱 가능한 정수인지까지만 확인하고,
             * 1 이상이어야 한다는 정책 검증은 서버 설정 검증 단계로 넘긴다.
             */
            if (!read_option_value(argc, argv, &index, &value) || !parse_int_strict(value, &config.worker_count)) {
                fprintf(stderr, "invalid value for --worker-count\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--queue-capacity") == 0) {
            /*
             * bounded queue 크기는 이후 listen backlog와 task queue 용량에 모두 쓰인다.
             * 따라서 여기서 정수 파싱 실패를 먼저 막고, 최소값 검증은 config validation에 맡긴다.
             */
            if (!read_option_value(argc, argv, &index, &value) || !parse_int_strict(value, &config.queue_capacity)) {
                fprintf(stderr, "invalid value for --queue-capacity\n");
                return 1;
            }
        } else if (strcmp(argv[index], "--schema-dir") == 0) {
            /*
             * schema 경로는 문자열로만 수집한다.
             * 실제로 디렉터리가 존재하는지는 서버 생성 단계에서 공통 검증한다.
             */
            if (!read_option_value(argc, argv, &index, &value)) {
                fprintf(stderr, "missing value for --schema-dir\n");
                return 1;
            }
            config.schema_dir = value;
        } else if (strcmp(argv[index], "--data-dir") == 0) {
            /*
             * data 경로도 여기서는 argv 문자열을 받기만 한다.
             * 존재 여부와 디렉터리 여부는 schema-dir과 동일하게 create 단계에서 확인한다.
             */
            if (!read_option_value(argc, argv, &index, &value)) {
                fprintf(stderr, "missing value for --data-dir\n");
                return 1;
            }
            config.data_dir = value;
        } else {
            /* 문서에 없는 옵션은 조용히 무시하지 않고 즉시 사용법 오류로 종료한다. */
            fprintf(stderr, "unknown option: %s\n", argv[index]);
            print_usage(argv[0]);
            return 1;
        }
    }

#ifndef _WIN32
    /*
     * 클라이언트가 먼저 연결을 끊은 뒤 send()가 실행돼도
     * 프로세스가 SIGPIPE로 종료되지 않도록 무시한다.
     */
    signal(SIGPIPE, SIG_IGN);
#endif
    /* Ctrl+C나 프로세스 종료 요청을 서버 shutdown 경로와 연결한다. */
    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);

    if (!sqlapi_server_create(&server, &config, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    /* signal handler가 같은 서버 인스턴스를 볼 수 있도록 전역 포인터를 노출한다. */
    global_server = server;
    if (!sqlapi_server_start(server, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        sqlapi_server_destroy(server);
        global_server = NULL;
        return 1;
    }

    /* accept thread와 worker들이 모두 종료될 때까지 기다린다. */
    sqlapi_server_wait(server);
    sqlapi_server_destroy(server);
    global_server = NULL;
    return 0;
}
