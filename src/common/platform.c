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

void sql_platform_network_cleanup(void) {
#ifdef _WIN32
    if (network_initialized) {
        WSACleanup();
        network_initialized = 0;
    }
#endif
}

int sql_platform_close_socket(sql_socket_t socket_fd) {
#ifdef _WIN32
    return closesocket(socket_fd);
#else
    return close(socket_fd);
#endif
}

int sql_platform_shutdown_socket(sql_socket_t socket_fd) {
#ifdef _WIN32
    return shutdown(socket_fd, SD_BOTH);
#else
    return shutdown(socket_fd, SHUT_RDWR);
#endif
}

void sql_platform_format_socket_error(char *error, size_t error_size, const char *action) {
#ifdef _WIN32
    snprintf(error, error_size, "%s: Winsock error %d", action, (int)WSAGetLastError());
#else
    snprintf(error, error_size, "%s: %s", action, strerror(errno));
#endif
}
