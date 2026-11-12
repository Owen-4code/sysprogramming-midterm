#include "common.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct settings {
    unsigned port;
    unsigned timeout_ms;
    unsigned reply_delay_ms;
    bool baseline;
    bool debug;
};

static int parse_options(int argc, char **argv, struct settings *settings)
{
    const struct option options[] = {
        { "port", required_argument, NULL, 'p' },
        { "timeout-ms", required_argument, NULL, 't' },
        { "reply-delay-ms", required_argument, NULL, 'r' },
        { "baseline", no_argument, NULL, 'b' },
        { "debug", no_argument, NULL, 'd' },
        { NULL, 0, NULL, 0 }
    };

    int option;
    while ((option = getopt_long(argc, argv, "p:t:r:bd", options, NULL)) != -1) {
        switch (option) {
        case 'p':
            if (parse_number(optarg, 65535, &settings->port) < 0) {
                return -1;
            }
            break;
        case 't':
            if (parse_number(optarg, 60000, &settings->timeout_ms) < 0 ||
                settings->timeout_ms == 0) {
                return -1;
            }
            break;
        case 'r':
            if (parse_number(optarg, 5000, &settings->reply_delay_ms) < 0) {
                return -1;
            }
            break;
        case 'b':
            settings->baseline = true;
            break;
        case 'd':
            settings->debug = true;
            break;
        default:
            return -1;
        }
    }

    return optind == argc ? 0 : -1;
}

static int open_listener(uint16_t port, struct sockaddr_in *address)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }

    *address = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    socklen_t size = sizeof(*address);
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(fd, (struct sockaddr *)address, size) < 0 ||
        listen(fd, SOMAXCONN) < 0 ||
        getsockname(fd, (struct sockaddr *)address, &size) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    return fd;
}

static enum line_status receive_baseline(int fd, char *line, size_t capacity)
{
    /* Demonstration: parse the first recv result as the entire request. */
    ssize_t received;
    do {
        received = recv(fd, line, capacity - 1, 0);
    } while (received < 0 && errno == EINTR);

    if (received <= 0) {
        return received == 0 ? LINE_EOF : LINE_IO_ERROR;
    }
    line[received] = '\0';
    char *newline = strchr(line, '\n');
    if (newline) {
        *newline = '\0';
    }
    return LINE_OK;
}

static void system_info(char *reply, size_t capacity)
{
    struct utsname os;
    struct sysinfo info;
    if (uname(&os) < 0 || sysinfo(&info) < 0) {
        snprintf(reply, capacity, "ERR system info unavailable\n");
        return;
    }

    const uintmax_t mib = 1024 * 1024;
    uintmax_t total = (uintmax_t)info.totalram * info.mem_unit / mib;
    uintmax_t free = (uintmax_t)info.freeram * info.mem_unit / mib;
    snprintf(reply, capacity,
             "OK system=%s release=%s arch=%s uptime_s=%ld "
             "ram_total_mib=%ju ram_free_mib=%ju worker_pid=%ld\n",
             os.sysname, os.release, os.machine, info.uptime,
             total, free, (long)getpid());
}

static int send_reply(int fd, const char *reply, const struct settings *settings)
{
    int flags = settings->baseline ? 0 : MSG_NOSIGNAL;
    if (settings->reply_delay_ms > 0 && strncmp(reply, "OK ", 3) == 0) {
        /* Separate the status prefix and payload for the disconnect test. */
        if (send_all(fd, reply, 3, flags) < 0) {
            return -1;
        }
        reply += 3;
        struct timespec delay = {
            .tv_sec = settings->reply_delay_ms / 1000,
            .tv_nsec = (long)(settings->reply_delay_ms % 1000) * 1000000L
        };
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
        }
    }

    return send_all(fd, reply, strlen(reply), flags);
}

static int handle_request(int fd, const struct settings *settings)
{
    char request[REQUEST_CAP];
    enum line_status status = settings->baseline
        ? receive_baseline(fd, request, sizeof(request))
        : receive_line(fd, request, sizeof(request), (int)settings->timeout_ms);

    char reply[RESPONSE_CAP];
    if (status != LINE_OK) {
        debug_log(settings->debug, "event=read_stopped worker=%ld reason=%s",
                  (long)getpid(), line_error(status));
        if (status == LINE_EOF || status == LINE_IO_ERROR) {
            return status == LINE_EOF ? 0 : 2;
        }
        snprintf(reply, sizeof(reply), "ERR %s\n", line_error(status));
    } else if (strcmp(request, "SYSINFO") == 0) {
        system_info(reply, sizeof(reply));
    } else if (strcmp(request, "PING") == 0) {
        snprintf(reply, sizeof(reply), "OK pong\n");
    } else {
        snprintf(reply, sizeof(reply), "ERR unknown command\n");
    }

    if (send_reply(fd, reply, settings) < 0 && !settings->baseline) {
        int error = errno;
        debug_log(settings->debug, "event=write_failed worker=%ld errno=%d (%s)",
                  (long)getpid(), error, strerror(error));
        (void)error;
        return 3;
    }

    return 0;
}

static int reap_workers(int signal_fd, bool debug)
{
    struct signalfd_siginfo signal_info;
    ssize_t size = read(signal_fd, &signal_info, sizeof(signal_info));
    if (size < 0 && errno == EINTR) {
        return 0;
    }
    if (size != sizeof(signal_info)) {
        return -1;
    }

    int status;
    pid_t worker;
    while ((worker = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            debug_log(debug, "event=reaped worker=%ld exit=%d",
                      (long)worker, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            debug_log(debug, "event=reaped worker=%ld signal=%d",
                      (long)worker, WTERMSIG(status));
        }
    }
    (void)debug;
    return 0;
}

static int serve(int listener, int signal_fd, const struct settings *settings)
{
    struct pollfd events[] = {
        { .fd = listener, .events = POLLIN },
        { .fd = signal_fd, .events = POLLIN }
    };
    pid_t parent = getpid();

    for (;;) {
        if (poll(events, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return 1;
        }
        if (events[1].revents && reap_workers(signal_fd, settings->debug) < 0) {
            perror("read signalfd");
            return 1;
        }
        if (!events[0].revents) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t length = sizeof(peer);
        int client = accept4(listener, (struct sockaddr *)&peer, &length, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            return 1;
        }

        pid_t worker = fork();
        if (worker == 0) {
            close(listener);
            close(signal_fd);
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) {
                _exit(4);
            }
            int result = handle_request(client, settings);
            close(client);
            _exit(result);
        }
        close(client);
        if (worker < 0) {
            perror("fork");
            continue;
        }
        debug_log(settings->debug, "event=accepted worker=%ld peer_port=%u",
                  (long)worker, (unsigned)ntohs(peer.sin_port));
    }
}

int main(int argc, char **argv)
{
    struct settings settings = { .port = DEFAULT_PORT, .timeout_ms = 5000 };
    if (parse_options(argc, argv, &settings) < 0) {
        fprintf(stderr,
                "Usage: %s [--port 0..65535] [--timeout-ms 1..60000] "
                "[--reply-delay-ms 0..5000] [--baseline] [--debug]\n", argv[0]);
        return 2;
    }

    struct sigaction default_action = { .sa_handler = SIG_DFL };
    sigemptyset(&default_action.sa_mask);
    if (sigaction(SIGPIPE, &default_action, NULL) < 0 ||
        sigaction(SIGCHLD, &default_action, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }

    int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
    if (signal_fd < 0) {
        perror("signalfd");
        return 1;
    }

    struct sockaddr_in address;
    int listener = open_listener((uint16_t)settings.port, &address);
    if (listener < 0) {
        perror("listen on 127.0.0.1");
        close(signal_fd);
        return 1;
    }

    printf("LISTEN port=%u pid=%ld mode=%s\n", (unsigned)ntohs(address.sin_port),
           (long)getpid(), settings.baseline ? "baseline" : "protected");
    fflush(stdout);
    int result = serve(listener, signal_fd, &settings);
    close(listener);
    close(signal_fd);
    return result;
}
