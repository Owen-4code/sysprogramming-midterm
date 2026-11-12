#include "common.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if SYSPROG_DEBUG
#include <stdarg.h>
#endif

int parse_number(const char *text, unsigned maximum, unsigned *value)
{
    if (*text < '0' || *text > '9') {
        return -1;
    }

    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (errno || *end || number > maximum) {
        return -1;
    }

    *value = (unsigned)number;
    return 0;
}

int connect_loopback(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    return fd;
}

int send_all(int fd, const char *data, size_t length, int flags)
{
    while (length > 0) {
        ssize_t written = send(fd, data, length, flags);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written == 0) {
                errno = EPIPE;
            }
            return -1;
        }
        data += written;
        length -= (size_t)written;
    }

    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) {
        return -1;
    }
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

enum line_status receive_line(int fd, char *line, size_t capacity, int timeout_ms)
{
    int64_t start = monotonic_ms();
    if (start < 0) {
        return LINE_IO_ERROR;
    }

    int64_t deadline = start + timeout_ms;
    size_t used = 0;
    struct pollfd input = { .fd = fd, .events = POLLIN };

    while (used < capacity) {
        int64_t now = monotonic_ms();
        if (now < 0) {
            return LINE_IO_ERROR;
        }
        if (now >= deadline) {
            return LINE_TIMEOUT;
        }

        int ready = poll(&input, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return ready == 0 ? LINE_TIMEOUT : LINE_IO_ERROR;
        }

        ssize_t received = recv(fd, line + used, capacity - used, MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return LINE_IO_ERROR;
        }
        if (received == 0) {
            return used == 0 ? LINE_EOF : LINE_INCOMPLETE;
        }

        char *chunk = line + used;
        char *newline = memchr(chunk, '\n', (size_t)received);
        size_t length = newline ? (size_t)(newline - chunk) : (size_t)received;
        if (memchr(chunk, '\0', length)) {
            return LINE_INVALID;
        }
        if (newline) {
            *newline = '\0';
            return LINE_OK;
        }

        used += (size_t)received;
    }

    return LINE_TOO_LONG;
}

const char *line_error(enum line_status status)
{
    switch (status) {
    case LINE_OK:         return "ok";
    case LINE_EOF:        return "peer closed";
    case LINE_INCOMPLETE: return "incomplete line";
    case LINE_TOO_LONG:   return "line too long";
    case LINE_TIMEOUT:    return "timeout";
    case LINE_INVALID:    return "invalid input";
    case LINE_IO_ERROR:   return "receive failed";
    }
    return "unknown error";
}

#if SYSPROG_DEBUG
void debug_log(int enabled, const char *format, ...)
{
    if (!enabled) {
        return;
    }

    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    if (length < 0) {
        return;
    }

    size_t used = (size_t)length;
    if (used > sizeof(line) - 2) {
        used = sizeof(line) - 2;
    }
    line[used++] = '\n';
    while (write(STDERR_FILENO, line, used) < 0 && errno == EINTR) {
    }
}
#endif
