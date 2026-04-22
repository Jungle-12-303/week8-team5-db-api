#ifndef SQLPARSER_COMMON_PLATFORM_H
#define SQLPARSER_COMMON_PLATFORM_H

#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sql_socket_t;
#define SQL_INVALID_SOCKET INVALID_SOCKET
#else
#include <sys/socket.h>
#include <sys/types.h>
typedef int sql_socket_t;
#define SQL_INVALID_SOCKET (-1)
#endif

int sql_platform_network_init(char *error, size_t error_size);
void sql_platform_network_cleanup(void);
int sql_platform_close_socket(sql_socket_t socket_fd);
int sql_platform_shutdown_socket(sql_socket_t socket_fd);
int sql_platform_send_socket(sql_socket_t socket_fd, const char *buffer, size_t length);
void sql_platform_format_socket_error(char *error, size_t error_size, const char *action);

#endif
