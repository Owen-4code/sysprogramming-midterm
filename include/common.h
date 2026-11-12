#ifndef SYSPROGRAM_COMMON_H
#define SYSPROGRAM_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define DEFAULT_PORT 39085
#define REQUEST_CAP 128
#define RESPONSE_CAP 512

enum line_status {
    LINE_OK,
    LINE_EOF,
    LINE_INCOMPLETE,
    LINE_TOO_LONG,
    LINE_TIMEOUT,
    LINE_INVALID,
    LINE_IO_ERROR
};

int parse_number(const char *text, unsigned maximum, unsigned *value);
int connect_loopback(uint16_t port);
int send_all(int fd, const char *data, size_t length, int flags);
enum line_status receive_line(int fd, char *line, size_t capacity, int timeout_ms);
const char *line_error(enum line_status status);

#if SYSPROG_DEBUG
void debug_log(int enabled, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#else
#define debug_log(...) ((void)0)
#endif

#endif
