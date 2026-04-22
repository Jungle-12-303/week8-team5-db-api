/*
 * common/platform.c
 *
 * 운영체제마다 다른 소켓 API를 최소 공용 인터페이스로 감싼다.
 *
 * 서버 계층은 이 파일을 통해 다음을 처리한다.
 * - Windows 전용 네트워크 초기화/정리
 * - 소켓 close / shutdown
 * - SIGPIPE를 피하는 안전한 send
 * - 소켓 오류 메시지 포맷팅
 */
#include "sqlparser/common/platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
static int network_initialized = 0;
#endif

/* Windows에서는 Winsock 초기화가 필요하고, POSIX에서는 no-op이다. */
int sql_platform_network_init(char *error, size_t error_size) {
#ifdef _WIN32
    WSADATA wsa_data;
    int status;

    if (network_initialized) {
        return 1;
    }

    status = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (status != 0) {
        snprintf(error, error_size, "failed to initialize Winsock: %d", status);
        return 0;
    }

    network_initialized = 1;
    return 1;
#else
    (void)error;
    (void)error_size;
    return 1;
#endif
}

/* Windows 초기화가 실제로 수행된 경우에만 정리 루틴을 호출한다. */
void sql_platform_network_cleanup(void) {
#ifdef _WIN32
    if (network_initialized) {
        WSACleanup();
        network_initialized = 0;
    }
#endif
}

/* 소켓 핸들을 닫아 커널 자원을 반납한다. */
int sql_platform_close_socket(sql_socket_t socket_fd) {
#ifdef _WIN32
    return closesocket(socket_fd);
#else
    return close(socket_fd);
#endif
}

/* accept/recv/send 대기를 빠르게 깨기 위해 양방향 shutdown을 먼저 건다. */
int sql_platform_shutdown_socket(sql_socket_t socket_fd) {
#ifdef _WIN32
    return shutdown(socket_fd, SD_BOTH);
#else
    return shutdown(socket_fd, SHUT_RDWR);
#endif
}

/*
 * 응답 전송 시 클라이언트가 먼저 연결을 끊더라도
 * POSIX 프로세스 전체가 SIGPIPE로 죽지 않게 MSG_NOSIGNAL을 사용한다.
 */
int sql_platform_send_socket(sql_socket_t socket_fd, const char *buffer, size_t length) {
#ifdef _WIN32
    return send(socket_fd, buffer, (int)length, 0);
#else
#ifdef MSG_NOSIGNAL
    return send(socket_fd, buffer, length, MSG_NOSIGNAL);
#else
    return send(socket_fd, buffer, length, 0);
#endif
#endif
}

/* 플랫폼별 마지막 소켓 오류를 사람이 읽기 쉬운 문자열로 변환한다. */
void sql_platform_format_socket_error(char *error, size_t error_size, const char *action) {
#ifdef _WIN32
    snprintf(error, error_size, "%s: Winsock error %d", action, (int)WSAGetLastError());
#else
    snprintf(error, error_size, "%s: %s", action, strerror(errno));
#endif
}
