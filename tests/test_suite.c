#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CLIENT_COUNT 10

struct server {
    pid_t pid;
    uint16_t port;
    FILE *output;
};

static struct server running;
static pid_t tester_pid;

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static pid_t wait_child(pid_t child, int *status)
{
    pid_t result;
    do {
        result = waitpid(child, status, 0);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void stop_server(void)
{
    if (getpid() != tester_pid) {
        return;
    }
    if (running.pid > 0) {
        kill(running.pid, SIGTERM);
        int status;
        wait_child(running.pid, &status);
        running.pid = 0;
    }
    if (running.output) {
        fclose(running.output);
        running.output = NULL;
    }
}

static void read_log(char *line, size_t size)
{
    struct pollfd input = { .fd = fileno(running.output), .events = POLLIN };
    int ready;
    do {
        ready = poll(&input, 1, 5000);
    } while (ready < 0 && errno == EINTR);
    require(ready > 0, "wait for server output");
    require(fgets(line, (int)size, running.output) != NULL, "read server output");
    line[strcspn(line, "\n")] = '\0';
}

static void start_server(const char *path, bool baseline, const char *timeout,
                         const char *reply_delay)
{
    int logs[2];
    require(pipe2(logs, O_CLOEXEC) == 0, "create server output pipe");
    pid_t child = fork();
    require(child >= 0, "start server");
    if (child == 0) {
        close(logs[0]);
        if (dup2(logs[1], STDOUT_FILENO) < 0 || dup2(logs[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(logs[1]);
        char *arguments[] = {
            (char *)path, "--port", "0", "--timeout-ms", (char *)timeout,
            "--reply-delay-ms", (char *)reply_delay, "--debug",
            baseline ? "--baseline" : NULL, NULL
        };
        execv(path, arguments);
        _exit(127);
    }

    close(logs[1]);
    running.pid = child;
    running.output = fdopen(logs[0], "r");
    require(running.output != NULL, "open server output");
    require(setvbuf(running.output, NULL, _IONBF, 0) == 0, "unbuffer server output");

    char line[RESPONSE_CAP];
    unsigned port;
    long server_pid;
    read_log(line, sizeof(line));
    require(sscanf(line, "LISTEN port=%u pid=%ld", &port, &server_pid) == 2 &&
            port > 0 && port <= 65535 && server_pid == child, "server listens");
    running.port = (uint16_t)port;
}

static long accepted_worker(unsigned *peer_port)
{
    char line[RESPONSE_CAP];
    long worker;
    do {
        read_log(line, sizeof(line));
    } while (sscanf(line, "event=accepted worker=%ld peer_port=%u", &worker, peer_port) != 2);
    return worker;
}

static void worker_event(const char *name, long worker, char *line, size_t size)
{
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "event=%s worker=%ld ", name, worker);
    do {
        read_log(line, size);
    } while (strncmp(line, prefix, strlen(prefix)) != 0);
}

static int open_connection(long *worker)
{
    int fd = connect_loopback(running.port);
    require(fd >= 0, "connect to server");
    unsigned peer_port;
    *worker = accepted_worker(&peer_port);
    return fd;
}

static void send_request(int fd, const char *request)
{
    require(send_all(fd, request, strlen(request), MSG_NOSIGNAL) == 0, "send request");
}

static void get_reply(int fd, const char *expected_prefix, char *reply)
{
    enum line_status status = receive_line(fd, reply, RESPONSE_CAP, 4000);
    require(status == LINE_OK, line_error(status));
    if (strncmp(reply, expected_prefix, strlen(expected_prefix)) != 0) {
        fprintf(stderr, "Expected prefix '%s', received '%s'\n", expected_prefix, reply);
        require(false, "unexpected response");
    }
}

static void check_quiet(int fd, int milliseconds)
{
    struct pollfd input = { .fd = fd, .events = POLLIN };
    require(poll(&input, 1, milliseconds) == 0, "unexpected early response or close");
}

static void run_client(const char *client)
{
    char port[6];
    snprintf(port, sizeof(port), "%u", (unsigned)running.port);
    pid_t child = fork();
    require(child >= 0, "start CLI client");
    if (child == 0) {
        execl(client, client, "--port", port, "SYSINFO", (char *)NULL);
        _exit(127);
    }
    int status;
    require(wait_child(child, &status) == child &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0, "CLI SYSINFO succeeds");

    unsigned peer_port;
    long worker = accepted_worker(&peer_port);
    char line[RESPONSE_CAP];
    worker_event("reaped", worker, line, sizeof(line));
}

struct connected_client {
    long pid;
    unsigned port;
    long worker;
};

static void hold_connection(int ready_fd, int release_fd)
{
    int fd = connect_loopback(running.port);
    require(fd >= 0, "test client connected");
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    require(getsockname(fd, (struct sockaddr *)&address, &length) == 0,
            "read client peer port");
    struct connected_client record = {
        .pid = (long)getpid(),
        .port = ntohs(address.sin_port)
    };
    require(write(ready_fd, &record, sizeof(record)) == sizeof(record), "report connected");
    close(ready_fd);

    char ignored;
    require(read(release_fd, &ignored, 1) == 0, "wait until all clients connected");
    close(release_fd);
    char reply[RESPONSE_CAP];
    send_request(fd, "SYSINFO\n");
    get_reply(fd, "OK system=Linux ", reply);
    close(fd);
    _exit(0);
}

static void prove_concurrency(void)
{
    int ready[2];
    int release[2];
    require(pipe2(ready, O_CLOEXEC) == 0 && pipe2(release, O_CLOEXEC) == 0,
            "create concurrency barriers");

    for (int i = 0; i < CLIENT_COUNT; ++i) {
        pid_t child = fork();
        require(child >= 0, "spawn test client");
        if (child == 0) {
            close(ready[0]);
            close(release[1]);
            hold_connection(ready[1], release[0]);
        }
    }
    close(ready[1]);
    close(release[0]);

    struct connected_client clients[CLIENT_COUNT];
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        require(read(ready[0], &clients[i], sizeof(clients[i])) == sizeof(clients[i]),
                "collect client PID and port");
    }
    close(ready[0]);

    for (int i = 0; i < CLIENT_COUNT; ++i) {
        unsigned peer_port;
        long worker = accepted_worker(&peer_port);
        bool matched = false;
        for (int j = 0; j < CLIENT_COUNT; ++j) {
            require(clients[j].worker != worker, "worker PIDs are unique");
            if (clients[j].port == peer_port) {
                clients[j].worker = worker;
                matched = true;
            }
        }
        require(matched, "match server worker to client port");
    }

    printf("CONCURRENCY SNAPSHOT (server_pid=%ld, before any client sent a request)\n",
           (long)running.pid);
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        struct connected_client record = clients[i];
        require(record.worker > 0 && kill((pid_t)record.worker, 0) == 0,
                "all server workers exist before release");
        printf("client_pid=%ld client_port=%u worker_pid=%ld state=connected\n",
               record.pid, record.port, record.worker);
    }

    close(release[1]);
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        pid_t child = (pid_t)clients[i].pid;
        int status;
        require(wait_child(child, &status) == child &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0, "concurrent SYSINFO succeeds");
    }
    puts("PASS: 10 simultaneous connections; 10/10 SYSINFO responses");
}

static void test_split(bool baseline)
{
    char reply[RESPONSE_CAP];
    long worker;
    int fd = open_connection(&worker);
    send_request(fd, "SYS");
    if (baseline) {
        get_reply(fd, "ERR unknown command", reply);
    } else {
        check_quiet(fd, 120);
        send_request(fd, "INFO\n");
        get_reply(fd, "OK system=Linux ", reply);
    }
    close(fd);
    printf("SPLIT    %-9s -> %s\n", baseline ? "baseline" : "protected", reply);
    worker_event("reaped", worker, reply, sizeof(reply));
}

static void test_idle(bool baseline)
{
    char reply[RESPONSE_CAP];
    long worker;
    int fd = open_connection(&worker);
    if (baseline) {
        check_quiet(fd, 750);
        send_request(fd, "PING\n");
        get_reply(fd, "OK pong", reply);
        printf("IDLE     baseline  -> still waiting after 750 ms; later PING: %s\n", reply);
    } else {
        get_reply(fd, "ERR timeout", reply);
        require(receive_line(fd, reply, sizeof(reply), 2000) == LINE_EOF,
                "timed-out connection closed");
        puts("IDLE     protected -> ERR timeout; connection closed");
    }
    close(fd);
    worker_event("reaped", worker, reply, sizeof(reply));
}

static void test_reset(bool baseline)
{
    long worker;
    int fd = open_connection(&worker);
    send_request(fd, "SYSINFO\n");

    struct pollfd input = { .fd = fd, .events = POLLIN };
    require(poll(&input, 1, 2000) > 0, "wait for response prefix");
    char prefix[3];
    require(recv(fd, prefix, sizeof(prefix), MSG_WAITALL) == sizeof(prefix) &&
            memcmp(prefix, "OK ", sizeof(prefix)) == 0, "receive prefix before reset");
    struct linger reset = { .l_onoff = 1, .l_linger = 0 };
    require(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0,
            "configure abortive close");
    close(fd);

    char line[RESPONSE_CAP];
    if (!baseline) {
        worker_event("write_failed", worker, line, sizeof(line));
        printf("RESET    protected -> %s\n", line);
    }
    worker_event("reaped", worker, line, sizeof(line));
    if (baseline) {
        require(strstr(line, "exit=0") || strstr(line, "signal=13"),
                "baseline ignored write error or died with SIGPIPE");
    } else {
        require(strstr(line, "exit=3") != NULL, "protected worker reports write failure");
    }
    printf("RESET    %-9s -> %s\n", baseline ? "baseline" : "protected", line);

    fd = open_connection(&worker);
    send_request(fd, "PING\n");
    get_reply(fd, "OK pong", line);
    close(fd);
    printf("RESET    %-9s -> next client: %s\n", baseline ? "baseline" : "protected", line);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s SERVER CLIENT\n", argv[0]);
        return 2;
    }

    tester_pid = getpid();
    require(setvbuf(stdout, NULL, _IOLBF, 0) == 0, "line-buffer test results");
    require(atexit(stop_server) == 0, "register server cleanup");

    start_server(argv[1], false, "10000", "0");
    run_client(argv[2]);
    prove_concurrency();
    stop_server();

    start_server(argv[1], true, "450", "650");
    test_split(true);
    test_idle(true);
    test_reset(true);
    stop_server();

    start_server(argv[1], false, "450", "650");
    test_split(false);
    test_idle(false);
    test_reset(false);
    stop_server();

    puts("PASS: all three baseline/protected comparisons");
    return 0;
}
