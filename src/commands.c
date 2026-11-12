#include "commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

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

void format_command_reply(const char *request, char *reply, size_t capacity)
{
    if (strcmp(request, "SYSINFO") == 0) {
        system_info(reply, capacity);
    } else if (strcmp(request, "PING") == 0) {
        snprintf(reply, capacity, "OK pong\n");
    } else {
        snprintf(reply, capacity, "ERR unknown command\n");
    }
}
