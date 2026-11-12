#include "perf_metrics.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    double v;
    assert(PerfScale(100, 1000, 500, &v) == 0 && v == 200);
    assert(PerfScale(100, 500, 500, &v) == 0 && v == 100);
    assert(PerfScale(UINT64_MAX, UINT64_MAX, UINT64_MAX, &v) == 0 && isfinite(v));
    assert(PerfScale(0, 1, 1, &v) == 0 && v == 0);
    assert(PerfScale(100, 1000, 0, &v) == -1 && errno == ERANGE);
    assert(PerfScale(100, 0, 1, &v) == -1);
    assert(PerfScale(100, 1, 2, &v) == -1);
    assert(PerfScale(100, 1, 1, NULL) == -1);
    puts("ALL PERF SCALING TESTS PASSED");
    return 0;
}
