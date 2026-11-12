#ifndef HW1_PERF_METRICS_H
#define HW1_PERF_METRICS_H

#include <stdint.h>

typedef struct PerfPair {
    int fd[2];
    uint64_t id[2];
    int error;
    const char *stage;
} PerfPair;

typedef struct PerfReading {
    double value[2];
    uint64_t time_enabled;
    uint64_t time_running;
} PerfReading;

/* One start/stop per open; re-open for another independent measurement. */
int PerfPairOpen(PerfPair *pair, uint32_t type, uint64_t a, uint64_t b);
int PerfPairStart(PerfPair *pair);
int PerfPairStop(PerfPair *pair, PerfReading *out);
void PerfPairClose(PerfPair *pair);
int PerfScale(uint64_t raw, uint64_t enabled, uint64_t running, double *out);

#endif
