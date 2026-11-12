#include "common.h"

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const struct option options[] = {
        { "port", required_argument, NULL, 'p' },
        { "debug", no_argument, NULL, 'd' },
        { NULL, 0, NULL, 0 }
    };
    unsigned port = DEFAULT_PORT;
    bool debug = false;
    bool valid = true;
    int option;
    while ((option = getopt_long(argc, argv, "p:d", options, NULL)) != -1) {
        switch (option) {
        case 'p':
            valid = parse_number(optarg, 65535, &port) == 0 && port > 0;
            break;
        case 'd':
            debug = true;
            break;
        default:
            valid = false;
            break;
        }
        if (!valid) {
            break;
        }
    }

    const char *command = argc - optind == 1 ? argv[optind] : "";
    if (!valid || (strcmp(command, "SYSINFO") != 0 && strcmp(command, "PING") != 0)) {
        fprintf(stderr, "Usage: %s [--port 1..65535] [--debug] SYSINFO|PING\n", argv[0]);
        return 2;
    }

    int fd = connect_loopback((uint16_t)port);
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    char request[REQUEST_CAP];
    int length = snprintf(request, sizeof(request), "%s\n", command);
    debug_log(debug, "event=request command=%s port=%u", command, port);
    (void)debug;
    if (send_all(fd, request, (size_t)length, MSG_NOSIGNAL) < 0) {
        perror("send request");
        close(fd);
        return 1;
    }

    char reply[RESPONSE_CAP];
    enum line_status status = receive_line(fd, reply, sizeof(reply), 15000);
    close(fd);
    if (status != LINE_OK) {
        fprintf(stderr, "read response: %s\n", line_error(status));
        return 1;
    }

    puts(reply);
    return strncmp(reply, "OK ", 3) == 0 ? 0 : 1;
}
