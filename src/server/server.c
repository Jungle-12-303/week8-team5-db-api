/*
 * server/server.c
 *
 * API 서버 런타임의 중심 모듈이다.
 *
 * 이 파일은 문서에서 정의한
 * "accept loop + task queue + worker thread pool" 구조를 실제로 조립한다.
 *
 * 책임:
 * - 서버 설정 검증과 런타임 상태 생성
 * - listen socket open / accept thread 시작
 * - 연결 단위 작업을 bounded queue에 적재
 * - worker thread에서 HTTP 요청 처리 루틴 호출
 * - shutdown 요청 시 accept loop와 queue/worker를 순서대로 정리
 *
 * 비책임:
 * - HTTP 파싱 세부 규칙
 * - SQL 실행과 락 정책의 세부 구현
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include "sqlparser/server/server.h"

#include "sqlparser/api/api_context.h"
#include "sqlparser/common/platform.h"
#include "sqlparser/common/util.h"
#include "sqlparser/engine/engine_lock_manager.h"
#include "sqlparser/http/http_request.h"
#include "sqlparser/http/http_response.h"
#include "sqlparser/http/router.h"
#include "sqlparser/index/table_index.h"
#include "sqlparser/server/task_queue.h"
#include "sqlparser/server/worker_pool.h"
#include "sqlparser/service/db_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#endif

struct SqlApiServer {
    /* CLI/문서에 노출되는 런타임 설정값이다. */
    char *host;
    int port;
    int worker_count;
    int queue_capacity;
    char *schema_dir;
    char *data_dir;
    size_t request_body_limit;
    size_t sql_length_limit;
    size_t header_limit;
    /* 네트워크 수신과 종료 제어에 필요한 소켓/스레드 상태다. */
    sql_socket_t listen_socket;
    pthread_t accept_thread;
    pthread_mutex_t state_mutex;
    int shutting_down;
    int started;
    /* 연결 분배, worker 실행, SQL 엔진 접근에 필요한 하위 모듈이다. */
    ServerTaskQueue task_queue;
    ServerWorkerPool worker_pool;
    EngineLockManager lock_manager;
    DbService db_service;
};

/* 설정 검증 단계에서 schema/data 경로가 실제 디렉터리인지 확인한다. */
static int directory_exists(const char *path) {
#ifdef _WIN32
    /* Windows는 stat 대신 속성 비트를 읽어 "존재 + 디렉터리" 여부를 동시에 확인한다. */
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    /* POSIX는 stat 성공 후 mode 비트가 디렉터리인지 본다. */
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

/*
 * 문서에 정의된 기본 서버 설정을 config 구조체에 채운다.
 *
 * 사용자가 옵션을 생략하고 `./build/bin/sqlapi_server` 만 실행해도
 * 이 기본값들로 서버가 뜰 수 있게 하는 함수다.
 */
void sqlapi_server_config_set_defaults(SqlApiServerConfig *config) {
    config->host = "127.0.0.1";
    config->port = 8080;
    config->worker_count = 4;
    config->queue_capacity = 64;
    config->schema_dir = "schema";
    config->data_dir = "data";
    config->request_body_limit = 16 * 1024;
    config->sql_length_limit = 8 * 1024;
    config->header_limit = 8 * 1024;
}

/*
 * listen 전에 설정값을 미리 검증한다.
 *
 * 소켓, 스레드, mutex를 만들기 전에 잘못된 입력을 걸러야
 * 실패 원인이 더 명확하고 정리 경로도 단순해진다.
 */
int sqlapi_server_validate_config(const SqlApiServerConfig *config, char *error, size_t error_size) {
    /* 잘못된 포트 범위는 bind 이전에 명시적인 설정 오류로 반환한다. */
    if (config->port < 1 || config->port > 65535) {
        snprintf(error, error_size, "--port must be between 1 and 65535");
        return 0;
    }
    /* worker가 0개면 요청을 처리할 주체가 없으므로 시작 자체를 막는다. */
    if (config->worker_count < 1) {
        snprintf(error, error_size, "--worker-count must be at least 1");
        return 0;
    }
    /* queue 용량이 0이면 accept thread가 어떤 연결도 넘길 수 없으므로 비정상 설정이다. */
    if (config->queue_capacity < 1) {
        snprintf(error, error_size, "--queue-capacity must be at least 1");
        return 0;
    }
    /* schema 경로가 디렉터리가 아니면 엔진이 meta 파일을 읽을 수 없다. */
    if (!directory_exists(config->schema_dir)) {
        snprintf(error, error_size, "--schema-dir must point to an existing directory");
        return 0;
    }
    /* data 경로가 디렉터리가 아니면 CSV 파일 접근이 불가능하다. */
    if (!directory_exists(config->data_dir)) {
        snprintf(error, error_size, "--data-dir must point to an existing directory");
        return 0;
    }
    return 1;
}

/*
 * 요청 전체를 정상 파싱하지 못한 경우에도 최소한의 JSON 오류 응답은 직접 내려준다.
 *
 * 예:
 * - header too large
 * - invalid content length
 * - chunked not supported
 */
static int send_simple_error(sql_socket_t socket_fd, SqlEngineErrorCode code, const char *message) {
    HttpResponse response;
    int ok;
    http_response_init(&response);
    if (!http_response_set_error(&response, code, message)) {
        return 0;
    }
    ok = http_response_send(socket_fd, &response);
    http_response_free(&response);
    return ok;
}

/* 유효한 소켓만 닫도록 공통 close 경로를 분리한다. */
static void close_client_socket(sql_socket_t socket_fd) {
    if (socket_fd != SQL_INVALID_SOCKET) {
        sql_platform_close_socket(socket_fd);
    }
}

/*
 * worker가 큐에서 꺼낸 연결 하나를 처리한다.
 * "1 connection = 1 request" 모델이므로 요청/응답 한 번이 끝나면 소켓을 닫는다.
 */
static void handle_client_request(SqlApiServer *server, sql_socket_t client_socket) {
    HttpRequest request;
    HttpRequestReadResult read_result;
    HttpResponse response;
    ApiContext context;

    http_request_init(&request);
    http_response_init(&response);

    /*
     * 요청 자체를 읽거나 검증하지 못한 경우는
     * 라우팅/서비스 단계로 내려가지 않고 HTTP 계층 오류 응답만 내려준다.
     */
    if (!http_read_request(client_socket,
                           server->header_limit,
                           server->request_body_limit,
                           &request,
                           &read_result)) {
        send_simple_error(client_socket, read_result.error_code, read_result.message);
        http_request_free(&request);
        http_response_free(&response);
        close_client_socket(client_socket);
        return;
    }

    /*
     * API 계층으로 넘길 실행 문맥을 만든다.
     * - db_service: 실제 SQL 실행 진입점
     * - worker_count / queue_depth: /health, / 화면에서 보여줄 런타임 상태
     */
    /* 라우터/핸들러가 헬스체크 응답에 넣을 현재 서버 상태를 context로 전달한다. */
    context.db_service = &server->db_service;
    context.worker_count = server->worker_count;
    context.queue_depth = sqlapi_server_queue_depth(server);

    /*
     * 라우팅이나 응답 조립에 실패하면 내부 오류로 정리하고,
     * 성공한 경우에만 완성된 HttpResponse를 그대로 전송한다.
     */
    if (!http_route_request(&request, &context, &response)) {
        send_simple_error(client_socket,
                          SQL_ENGINE_ERROR_INTERNAL_ERROR,
                          "failed to construct HTTP response");
    } else {
        /* 라우터가 만든 완성 응답을 그대로 클라이언트 소켓에 전송한다. */
        http_response_send(client_socket, &response);
    }

    /* 1 connection = 1 request 이므로 응답 후 요청/응답 자원과 소켓을 모두 정리한다. */
    http_request_free(&request);
    http_response_free(&response);
    close_client_socket(client_socket);
}

/*
 * worker thread 본체다.
 *
 * queue에서 연결 하나를 꺼내 처리하고,
 * queue가 close되어 더 이상 pop할 작업이 없으면 루프를 끝낸다.
 */
static void *server_worker_main(void *context) {
    SqlApiServer *server = (SqlApiServer *)context;
    ServerTask task;

    while (server_task_queue_pop(&server->task_queue, &task)) {
        handle_client_request(server, task.client_socket);
    }

    return NULL;
}

/* accept loop가 종료되어야 하는지 state mutex로 보호된 플래그를 읽는다. */
static int server_is_shutting_down(SqlApiServer *server) {
    int shutting_down;

    pthread_mutex_lock(&server->state_mutex);
    shutting_down = server->shutting_down;
    pthread_mutex_unlock(&server->state_mutex);
    return shutting_down;
}

/*
 * accept thread는 새 연결을 받아 bounded queue에 넣는다.
 * 큐가 가득 찬 경우 worker로 넘기지 않고 즉시 503을 반환한다.
 */
static void *server_accept_main(void *context) {
    SqlApiServer *server = (SqlApiServer *)context;

    while (!server_is_shutting_down(server)) {
        sql_socket_t client_socket = accept(server->listen_socket, NULL, NULL);
        if (client_socket == SQL_INVALID_SOCKET) {
            /*
             * shutdown이 이미 요청된 상태에서 accept가 깨진 경우는 정상 종료 경로다.
             * 이때는 더 이상 새 연결을 받지 않고 루프를 끝낸다.
             */
            if (server_is_shutting_down(server)) {
                break;
            }
            /*
             * 일시적인 accept 실패나 외부 소켓 오류는
             * 서버 전체 종료로 보지 않고 다음 accept 시도로 넘어간다.
             */
            continue;
        }

        /*
         * 큐에 자리가 있으면 worker에게 넘기고,
         * 자리가 없으면 이 연결은 서버가 직접 503으로 거절한다.
         */
        if (!server_task_queue_try_push(&server->task_queue, &(ServerTask){client_socket})) {
            send_simple_error(client_socket,
                              SQL_ENGINE_ERROR_QUEUE_FULL,
                              "server is busy");
            close_client_socket(client_socket);
        }
    }

    return NULL;
}

/* host/port 설정을 실제 listen socket으로 바꾸는 단계다. */
static int open_listen_socket(SqlApiServer *server, char *error, size_t error_size) {
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *current;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port_text, sizeof(port_text), "%d", server->port);

    /*
     * host/port를 실제 bind 가능한 sockaddr 목록으로 변환한다.
     * 실패하면 listen 단계로 내려갈 수 없으므로 즉시 시작 실패를 반환한다.
     */
    status = getaddrinfo(server->host, port_text, &hints, &result);
    if (status != 0) {
        snprintf(error, error_size, "failed to resolve listen address");
        return 0;
    }

    for (current = result; current != NULL; current = current->ai_next) {
        int yes = 1;

        /* 후보 주소 하나마다 새 소켓을 만들고 bind/listen 가능 여부를 시험한다. */
        server->listen_socket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (server->listen_socket == SQL_INVALID_SOCKET) {
            /* 이 주소군에서 소켓 생성이 안 되면 다음 후보 주소를 시도한다. */
            continue;
        }

        /* 재시작 직후에도 같은 포트를 빠르게 다시 bind할 수 있도록 설정한다. */
        setsockopt(server->listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
        if (bind(server->listen_socket, current->ai_addr, (int)current->ai_addrlen) == 0 &&
            listen(server->listen_socket, server->queue_capacity) == 0) {
            /* bind와 listen이 모두 성공한 첫 번째 주소를 최종 listen socket으로 채택한다. */
            freeaddrinfo(result);
            return 1;
        }

        /* 현재 후보는 bind/listen에 실패했으므로 소켓을 닫고 다음 주소를 시도한다. */
        sql_platform_close_socket(server->listen_socket);
        server->listen_socket = SQL_INVALID_SOCKET;
    }

    /* 모든 주소 후보를 소진한 경우 마지막 소켓 오류를 사람이 읽을 수 있게 남긴다. */
    freeaddrinfo(result);
    sql_platform_format_socket_error(error, error_size, "failed to bind or listen");
    return 0;
}

/*
 * 서버 런타임 객체를 생성한다.
 *
 * create 단계는 "시작 전 준비"만 담당한다.
 * 즉 여기서는 설정 복사, mutex/queue/lock/registry 초기화까지만 하고,
 * 실제 네트워크 바인드와 스레드 시작은 start 단계에서 수행한다.
 */
int sqlapi_server_create(SqlApiServer **out_server,
                         const SqlApiServerConfig *config,
                         char *error,
                         size_t error_size) {
    SqlApiServer *server;

    /* 잘못된 설정은 메모리/스레드/소켓 할당 전에 바로 거절한다. */
    if (!sqlapi_server_validate_config(config, error, error_size)) {
        return 0;
    }

    /* 서버 본체 구조체를 먼저 만들고 이후 하위 자원을 단계적으로 붙여 나간다. */
    server = (SqlApiServer *)calloc(1, sizeof(*server));
    if (server == NULL) {
        snprintf(error, error_size, "out of memory while creating server");
        return 0;
    }

    /*
     * 이후 런타임 동안 config 원본보다 오래 살아야 할 수 있으므로,
     * 설정 문자열은 서버 객체가 독립 소유하도록 복사한다.
     */
    server->host = copy_string(config->host);
    server->schema_dir = copy_string(config->schema_dir);
    server->data_dir = copy_string(config->data_dir);
    if (server->host == NULL || server->schema_dir == NULL || server->data_dir == NULL) {
        /* 문자열 복사 단계가 실패하면 아직 초기화한 자원만 정리하고 종료한다. */
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        snprintf(error, error_size, "out of memory while copying server configuration");
        return 0;
    }

    server->port = config->port;
    server->worker_count = config->worker_count;
    server->queue_capacity = config->queue_capacity;
    server->request_body_limit = config->request_body_limit;
    server->sql_length_limit = config->sql_length_limit;
    server->header_limit = config->header_limit;
    server->listen_socket = SQL_INVALID_SOCKET;

    /* 이후 shutdown 플래그와 started 상태를 안전하게 읽고 쓰기 위한 mutex다. */
    if (pthread_mutex_init(&server->state_mutex, NULL) != 0) {
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        snprintf(error, error_size, "failed to initialize server state mutex");
        return 0;
    }

    /* accept thread와 worker thread가 공유할 bounded queue를 만든다. */
    if (!server_task_queue_init(&server->task_queue, server->queue_capacity, error, error_size)) {
        pthread_mutex_destroy(&server->state_mutex);
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        return 0;
    }

    /* 전역 인덱스 레지스트리는 서버 시작 전에 공용 상태로 초기화해 둔다. */
    if (!table_index_registry_init(error, error_size)) {
        server_task_queue_destroy(&server->task_queue);
        pthread_mutex_destroy(&server->state_mutex);
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        return 0;
    }

    /* SQL 실행 전후의 table-level 직렬화를 위해 lock manager를 준비한다. */
    if (!engine_lock_manager_init(&server->lock_manager, error, error_size)) {
        server_task_queue_destroy(&server->task_queue);
        pthread_mutex_destroy(&server->state_mutex);
        free(server->host);
        free(server->schema_dir);
        free(server->data_dir);
        free(server);
        return 0;
    }

    /*
     * service 계층은 내부 adapter_config를 통해
     * schema/data 경로, SQL 길이 제한, lock manager를 공유받는다.
     */
    server->db_service.adapter_config.schema_dir = server->schema_dir;
    server->db_service.adapter_config.data_dir = server->data_dir;
    server->db_service.adapter_config.sql_length_limit = server->sql_length_limit;
    server->db_service.adapter_config.lock_manager = &server->lock_manager;

    *out_server = server;
    return 1;
}

/*
 * 실제 서버를 시작한다.
 *
 * 시작 순서가 중요하다:
 * 1. 네트워크 스택 초기화
 * 2. listen socket open
 * 3. worker pool 시작
 * 4. accept thread 시작
 *
 * worker가 먼저 떠 있어야 accept thread가 넣은 작업을 바로 소비할 수 있다.
 */
int sqlapi_server_start(SqlApiServer *server, char *error, size_t error_size) {
    /* Windows 환경에서는 listen 전에 네트워크 스택 초기화가 필요하다. */
    if (!sql_platform_network_init(error, error_size)) {
        return 0;
    }

    /* 실제 bind/listen 단계다. 실패하면 아직 thread는 시작되지 않았다. */
    if (!open_listen_socket(server, error, error_size)) {
        return 0;
    }

    /* worker pool부터 시작해야 accept thread가 넘긴 작업을 바로 처리할 수 있다. */
    if (!server_worker_pool_start(&server->worker_pool,
                                  server->worker_count,
                                  server_worker_main,
                                  server,
                                  error,
                                  error_size)) {
        /* worker 생성이 실패하면 listen socket만 되감으면 된다. */
        sql_platform_close_socket(server->listen_socket);
        server->listen_socket = SQL_INVALID_SOCKET;
        return 0;
    }

    /* 마지막으로 accept thread를 띄워 외부 연결 유입을 시작한다. */
    if (pthread_create(&server->accept_thread, NULL, server_accept_main, server) != 0) {
        snprintf(error, error_size, "failed to create accept thread");
        /* accept thread 생성 실패 시 이미 띄운 worker와 queue를 정상 종료 경로로 정리한다. */
        server_task_queue_close(&server->task_queue);
        server_worker_pool_join(&server->worker_pool);
        server_worker_pool_destroy(&server->worker_pool);
        sql_platform_close_socket(server->listen_socket);
        server->listen_socket = SQL_INVALID_SOCKET;
        return 0;
    }

    server->started = 1;
    return 1;
}

/*
 * shutdown은 여러 번 호출될 수 있으므로 idempotent해야 한다.
 * listen socket을 먼저 끊어 accept를 깨우고, 이어서 queue를 닫아 worker를 종료시킨다.
 */
void sqlapi_server_request_shutdown(SqlApiServer *server) {
    pthread_mutex_lock(&server->state_mutex);
    if (!server->shutting_down) {
        /* 첫 shutdown 요청만 실제 자원 정리를 시작하고, 이후 호출은 무시한다. */
        server->shutting_down = 1;
        if (server->listen_socket != SQL_INVALID_SOCKET) {
            /*
             * shutdown -> close 순서로 listen socket을 끊어
             * accept thread가 블로킹에서 깨어나 종료 플래그를 볼 수 있게 만든다.
             */
            sql_platform_shutdown_socket(server->listen_socket);
            sql_platform_close_socket(server->listen_socket);
            server->listen_socket = SQL_INVALID_SOCKET;
        }
        /* 큐를 닫으면 대기 중인 worker들이 pop에서 빠져나와 종료할 수 있다. */
        server_task_queue_close(&server->task_queue);
    }
    pthread_mutex_unlock(&server->state_mutex);
}

/*
 * 시작된 스레드들이 모두 정리될 때까지 기다린다.
 *
 * request_shutdown이 "종료를 요청"하는 단계라면,
 * wait는 실제 종료 완료를 기다리는 단계다.
 */
void sqlapi_server_wait(SqlApiServer *server) {
    if (!server->started) {
        /* start 이전에는 join할 스레드가 없으므로 바로 반환한다. */
        return;
    }

    /* accept thread가 끝난 뒤 worker thread들도 순서대로 종료될 때까지 기다린다. */
    pthread_join(server->accept_thread, NULL);
    server_worker_pool_join(&server->worker_pool);
}

/*
 * 서버 객체가 소유한 하위 모듈과 동적 메모리를 모두 해제한다.
 *
 * destroy는 마지막 정리 단계이며,
 * 네트워크/스레드/인덱스/락/큐/설정 문자열을 역순으로 반납한다.
 */
void sqlapi_server_destroy(SqlApiServer *server) {
    if (server == NULL) {
        return;
    }

    /* destroy는 안전하게 shutdown을 선행 호출해 미종료 스레드가 없게 만든다. */
    sqlapi_server_request_shutdown(server);
    if (server->started) {
        /*
         * thread handle 배열은 started 경로에서만 생성된다.
         * 실제 스레드 종료 대기는 wait 단계에서 끝났다고 가정한다.
         */
        server_worker_pool_destroy(&server->worker_pool);
    }
    /* 전역 인덱스/락/큐/mutex를 역순으로 정리한 뒤 설정 문자열과 서버 본체를 해제한다. */
    table_index_registry_reset();
    table_index_registry_shutdown();
    engine_lock_manager_destroy(&server->lock_manager);
    server_task_queue_destroy(&server->task_queue);
    pthread_mutex_destroy(&server->state_mutex);
    free(server->host);
    free(server->schema_dir);
    free(server->data_dir);
    sql_platform_network_cleanup();
    free(server);
}

/* 헬스체크 응답에서 현재 queue depth를 노출할 때 사용한다. */
int sqlapi_server_queue_depth(SqlApiServer *server) {
    return server_task_queue_depth(&server->task_queue);
}

/* 헬스체크 응답에서 worker 수를 노출할 때 사용한다. */
int sqlapi_server_worker_count(SqlApiServer *server) {
    return server->worker_count;
}
