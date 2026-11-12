#define _GNU_SOURCE
#include "perf_metrics.h"

#include <errno.h>
#include <linux/perf_event.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fail(PerfPair *pair, const char *stage)
{
    pair->error = errno;
    pair->stage = stage;
    return -1;
}

int PerfScale(uint64_t raw, uint64_t enabled, uint64_t running, double *out)
{
    if (!out || running == 0 || enabled == 0 || running > enabled) {
        errno = ERANGE;
        return -1;
    }
    *out = (double)((long double)raw * enabled / running);
    return 0;
}

void PerfPairClose(PerfPair *pair)
{
    for (int i = 0; i < 2; ++i) {
        if (pair->fd[i] >= 0)
            close(pair->fd[i]);
        pair->fd[i] = -1;
    }
}

int PerfPairOpen(PerfPair *pair, uint32_t type, uint64_t a, uint64_t b)
{
    uint64_t configs[2] = {a, b};
    memset(pair, 0, sizeof(*pair));
    pair->fd[0] = pair->fd[1] = -1;
    for (int i = 0; i < 2; ++i) {
        struct perf_event_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.type = type;
        attr.size = sizeof(attr);
        attr.config = configs[i];
        attr.disabled = i == 0;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
                           PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;
        /* Current thread only, any CPU. Caller pins the thread separately. */
        pair->fd[i] = (int)syscall(__NR_perf_event_open, &attr, 0, -1,
                                  i ? pair->fd[0] : -1, PERF_FLAG_FD_CLOEXEC);
        if (pair->fd[i] < 0) {
            fail(pair, i ? "perf_event_open(member)" : "perf_event_open(leader)");
            PerfPairClose(pair);
            return -1;
        }
        if (ioctl(pair->fd[i], PERF_EVENT_IOC_ID, &pair->id[i]) < 0) {
            fail(pair, "PERF_EVENT_IOC_ID");
            PerfPairClose(pair);
            return -1;
        }
    }
    return 0;
}

int PerfPairStart(PerfPair *pair)
{
    if (pair->fd[0] < 0) {
        errno = EBADF;
        return fail(pair, "group not open");
    }
    if (ioctl(pair->fd[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) < 0)
        return fail(pair, "PERF_EVENT_IOC_RESET");
    if (ioctl(pair->fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0)
        return fail(pair, "PERF_EVENT_IOC_ENABLE");
    return 0;
}

int PerfPairStop(PerfPair *pair, PerfReading *out)
{
    struct {
        uint64_t nr, enabled, running;
        struct { uint64_t value, id; } item[2];
    } buf;
    ssize_t n;
    unsigned seen = 0;
    if (ioctl(pair->fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) < 0)
        return fail(pair, "PERF_EVENT_IOC_DISABLE");
    do {
        n = read(pair->fd[0], &buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(buf) || buf.nr != 2) {
        if (n >= 0)
            errno = EIO;
        return fail(pair, "read(group)");
    }
    out->time_enabled = buf.enabled;
    out->time_running = buf.running;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            if (buf.item[i].id == pair->id[j]) {
                if (PerfScale(buf.item[i].value, buf.enabled, buf.running,
                              &out->value[j]) < 0)
                    return fail(pair, "counter never scheduled or invalid time");
                seen |= 1u << j;
            }
        }
    }
    if (seen != 3) {
        errno = EIO;
        return fail(pair, "unexpected event IDs");
    }
    return 0;
}
