#define _GNU_SOURCE
#include "link.h"
#include "perf_metrics.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

typedef struct Options {
    size_t nodes, lists, queries;
    int size, repeats, hit_rate, cpu, json, require_perf;
    uint64_t seed, loads_raw, stores_raw;
    int have_loads, have_stores, auto_amd;
    const char *pattern;
} Options;

typedef struct Query {
    LinkData *head;
    LinkData *expected;
    char *key;
} Query;

typedef struct Dataset {
    LinkData **heads;
    Query *queries;
    char *keys;
    size_t hits, memory_before, memory_after;
    double build_ms, prepare_ms;
} Dataset;

typedef LinkData *(*SearchFn)(LinkData *, int, char *);
typedef struct CounterResult {
    int available, error;
    const char *stage;
    double a, b, enabled, running;
} CounterResult;

typedef struct Result {
    const char *name;
    double times[31], median, min, max;
    uint64_t checksum;
    size_t hits;
    CounterResult counters[3];
} Result;

static volatile uint64_t result_sink;
static uint64_t rng_state;

static uint64_t random_u64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}

static double now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t) < 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --nodes N       nodes per list (default 10000)\n"
           "  --lists N       independent lists (default 1)\n"
           "  --queries N     fixed queries per measured pass (default 20000)\n"
           "  --size N        payload bytes 32..512 (default 64)\n"
           "  --repeats N     timed and counter repeats 1..31 (default 5)\n"
           "  --hit-rate N    percentage 0..100 (default 50)\n"
           "  --pattern P     random, prefix or duplicate (default random)\n"
           "  --seed N        nonzero deterministic seed (default 20261112)\n"
           "  --cpu N         pin to allowed CPU N (default first allowed)\n"
           "  --loads-raw X   CPU-specific memory-load event (0x... accepted)\n"
           "  --stores-raw X  CPU-specific store/RMW event (both required)\n"
           "                 Ryzen 9950X3D auto-selects LS dispatch 0x129 + 0x629\n"
           "  --require-perf  exit 2 if any required hardware group is unavailable\n"
           "  --json          emit JSON (unavailable metrics are null)\n"
           "  --help          show this help\n", program);
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long v;
    if (!text[0] || text[0] == '-')
        return -1;
    errno = 0;
    v = strtoull(text, &end, 0);
    if (errno || *end)
        return -1;
    *value = (uint64_t)v;
    return 0;
}

static int options(int argc, char **argv, Options *o)
{
    *o = (Options){.nodes = 10000, .lists = 1, .queries = 20000,
                   .size = 64, .repeats = 5, .hit_rate = 50, .cpu = -1,
                   .seed = UINT64_C(20261112), .pattern = "random"};
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        uint64_t v;
        if (!strcmp(key, "--help")) {
            usage(argv[0]);
            exit(0);
        }
        if (!strcmp(key, "--json")) { o->json = 1; continue; }
        if (!strcmp(key, "--require-perf")) { o->require_perf = 1; continue; }
        if (++i >= argc) {
            fprintf(stderr, "Missing value for %s\n", key);
            return -1;
        }
        if (!strcmp(key, "--pattern")) {
            o->pattern = argv[i];
            continue;
        }
        if (parse_u64(argv[i], &v)) {
            fprintf(stderr, "Invalid number for %s\n", key);
            return -1;
        }
        if (!strcmp(key, "--nodes") && v <= SIZE_MAX) o->nodes = (size_t)v;
        else if (!strcmp(key, "--lists") && v <= SIZE_MAX) o->lists = (size_t)v;
        else if (!strcmp(key, "--queries") && v <= SIZE_MAX) o->queries = (size_t)v;
        else if (!strcmp(key, "--size") && v <= INT_MAX) o->size = (int)v;
        else if (!strcmp(key, "--repeats") && v <= INT_MAX) o->repeats = (int)v;
        else if (!strcmp(key, "--hit-rate") && v <= INT_MAX) o->hit_rate = (int)v;
        else if (!strcmp(key, "--cpu") && v < CPU_SETSIZE) o->cpu = (int)v;
        else if (!strcmp(key, "--seed")) o->seed = v;
        else if (!strcmp(key, "--loads-raw")) { o->loads_raw = v; o->have_loads = 1; }
        else if (!strcmp(key, "--stores-raw")) { o->stores_raw = v; o->have_stores = 1; }
        else {
            fprintf(stderr, "Unknown option or out-of-range value: %s\n", key);
            return -1;
        }
    }
    if (!o->nodes || !o->lists || !o->queries || !o->seed ||
        o->size < 32 || o->size > 512 || o->repeats < 1 || o->repeats > 31 ||
        o->hit_rate < 0 || o->hit_rate > 100 ||
        (strcmp(o->pattern, "random") && strcmp(o->pattern, "prefix") &&
         strcmp(o->pattern, "duplicate")) || o->have_loads != o->have_stores ||
        o->lists > SIZE_MAX / o->nodes || o->lists > SIZE_MAX / sizeof(LinkData *) ||
        o->lists * o->nodes > SIZE_MAX / sizeof(LinkData *) ||
        o->queries > SIZE_MAX / sizeof(Query) ||
        o->queries > SIZE_MAX / (size_t)o->size ||
        o->lists * o->nodes > UINT64_MAX - o->queries - 1) {
        fputs("Invalid options: check sizes, counts, pattern and paired raw configs.\n", stderr);
        return -1;
    }
    return 0;
}

static int pin_cpu(Options *o)
{
    cpu_set_t allowed, selected;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) < 0)
        return -1;
    if (o->cpu < 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &allowed)) { o->cpu = i; break; }
    }
    if (o->cpu < 0 || !CPU_ISSET(o->cpu, &allowed)) {
        errno = EINVAL;
        return -1;
    }
    CPU_ZERO(&selected);
    CPU_SET(o->cpu, &selected);
    return sched_setaffinity(0, sizeof(selected), &selected);
}

static void select_memory_events(Options *o)
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned a, b, c, d, family, model, base_family;
    char vendor[13];
    if (o->have_loads || !__get_cpuid(0, &a, &b, &c, &d))
        return;
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    if (strcmp(vendor, "AuthenticAMD") || !__get_cpuid(1, &a, &b, &c, &d))
        return;
    base_family = (a >> 8) & 0xf;
    family = base_family;
    if (base_family == 0xf)
        family += (a >> 20) & 0xff;
    model = (a >> 4) & 0xf;
    if (base_family == 0x6 || base_family == 0xf)
        model += ((a >> 16) & 0xf) << 4;
    /* Exact tested CPU model only. Linux amdzen5/load-store.json:
     * Event 0x29 masks 0x01 = loads; 0x06 = stores + load-op-stores.
     * The sum is ls_dispatch.all (mask 0x07), not retired instructions. */
    if (family == 0x1a && model == 0x44) {
        o->loads_raw = UINT64_C(0x129);
        o->stores_raw = UINT64_C(0x629);
        o->have_loads = o->have_stores = o->auto_amd = 1;
    }
#else
    (void)o;
#endif
}

static const char *memory_definition(const Options *o)
{
    if (o->auto_amd)
        return "AMD Zen5 LS-dispatched memory operations: loads + stores/RMW; speculative, not retired instructions or DRAM transactions";
    if (o->have_loads)
        return "sum of caller-selected raw load and store/RMW events; caller must validate CPU semantics";
    return "L1D read + write accesses; proxy, not retired memory instructions or DRAM transactions";
}

static void make_key(char *data, int size, uint64_t serial, const char *pattern)
{
    uint64_t x = serial + UINT64_C(0x9e3779b97f4a7c15);
    if (!strcmp(pattern, "prefix")) {
        memset(data, 0x5a, (size_t)size);
    } else {
        for (int i = 0; i < size; ++i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            data[i] = (char)(x >> 24);
        }
    }
    /* Unique serial, exact original bytes; benchmark generation is not indexing. */
    for (unsigned i = 0; i < 8; ++i)
        data[size - 8 + (int)i] = (char)(serial >> (8 * i));
}

static void destroy_dataset(Dataset *d, const Options *o)
{
    if (d->heads)
        for (size_t i = 0; i < o->lists; ++i)
            DestroyLink(d->heads[i]);
    free(d->heads);
    free(d->queries);
    free(d->keys);
}

static int create_dataset(Dataset *d, const Options *o)
{
    char key[512];
    size_t total = o->nodes * o->lists;
    LinkData **all = NULL;
    double t = now_ns();
    memset(d, 0, sizeof(*d));
    d->heads = calloc(o->lists, sizeof(*d->heads));
    d->queries = malloc(o->queries * sizeof(*d->queries));
    d->keys = malloc(o->queries * (size_t)o->size);
    all = malloc(total * sizeof(*all));
    if (!d->heads || !d->queries || !d->keys || !all)
        goto fail;
    for (size_t l = 0; l < o->lists; ++l) {
        for (size_t i = 0; i < o->nodes; ++i) {
            size_t original = !strcmp(o->pattern, "duplicate") ? (i / 8) * 8 : i;
            uint64_t serial = (uint64_t)(l * o->nodes + original);
            make_key(key, o->size, serial, o->pattern);
            if (i == 0)
                all[l * o->nodes + i] = d->heads[l] = InitLink(o->size, key);
            else
                all[l * o->nodes + i] = AppendNodeChecked(d->heads[l], o->size, key);
            if (!all[l * o->nodes + i])
                goto fail;
        }
        d->memory_before += LinkMemoryBytes(d->heads[l]);
    }
    rng_state = o->seed;
    /* Exact floor(Q * percentage / 100), without an overflowing product. */
    d->hits = (o->queries / 100) * (size_t)o->hit_rate +
              (o->queries % 100) * (size_t)o->hit_rate / 100;
    for (size_t i = 0; i < o->queries; ++i) {
        size_t l = (size_t)(random_u64() % o->lists);
        size_t n = (size_t)(random_u64() % o->nodes);
        Query *q = &d->queries[i];
        q->head = d->heads[l];
        q->key = d->keys + i * (size_t)o->size;
        q->expected = NULL;
        if (i < d->hits) {
            size_t original = !strcmp(o->pattern, "duplicate") ? (n / 8) * 8 : n;
            q->expected = all[l * o->nodes + original];
            memcpy(q->key, q->expected->data, (size_t)o->size);
        } else {
            make_key(q->key, o->size, (uint64_t)(total + i + 1), o->pattern);
        }
    }
    for (size_t i = o->queries; i > 1; --i) {
        size_t j = (size_t)(random_u64() % i);
        Query temp = d->queries[i - 1];
        d->queries[i - 1] = d->queries[j];
        d->queries[j] = temp;
    }
    free(all);
    d->build_ms = (now_ns() - t) / 1e6;
    t = now_ns();
    for (size_t i = 0; i < o->lists; ++i) {
        if (PrepareLink(d->heads[i]) < 0)
            return -1;
        d->memory_after += LinkMemoryBytes(d->heads[i]);
    }
    d->prepare_ms = (now_ns() - t) / 1e6;
    return 0;
fail:
    free(all);
    return -1;
}

static LinkData *linear_adapter(LinkData *head, int size, char *mem)
{
    return FindNodeLinear(head, size, mem);
}

static uint64_t run_queries(SearchFn fn, const Dataset *d, const Options *o,
                            size_t *hits)
{
    uint64_t checksum = 0;
    size_t h = 0;
    for (size_t i = 0; i < o->queries; ++i) {
        const Query *q = &d->queries[i];
        LinkData *found = fn(q->head, o->size, q->key);
        if (found) {
            ++h;
            checksum += (uint64_t)(unsigned char)found->data[o->size - 1] +
                        (uint64_t)(unsigned)found->dataSize + 1;
        }
    }
    *hits = h;
    return checksum;
}

static int verify_and_warm(const Dataset *d, const Options *o)
{
    for (size_t i = 0; i < o->queries; ++i) {
        const Query *q = &d->queries[i];
        if (FindNodeLinear(q->head, o->size, q->key) != q->expected ||
            FindNode(q->head, o->size, q->key) != q->expected) {
            fprintf(stderr, "Oracle verification failed at query %zu\n", i);
            return -1;
        }
    }
    return 0;
}

static int compare_doubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static uint64_t cache_config(unsigned op, unsigned result)
{
    return PERF_COUNT_HW_CACHE_L1D | ((uint64_t)op << 8) | ((uint64_t)result << 16);
}

static void measure_counters(Result *r, SearchFn fn, const Dataset *d,
                             const Options *o)
{
    uint32_t types[3] = {PERF_TYPE_HARDWARE, PERF_TYPE_HW_CACHE,
                         o->have_loads ? PERF_TYPE_RAW : PERF_TYPE_HW_CACHE};
    uint64_t configs[3][2] = {
        {PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CPU_CYCLES},
        {cache_config(PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS),
         cache_config(PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
        {o->have_loads ? o->loads_raw : cache_config(PERF_COUNT_HW_CACHE_OP_READ,
                                                   PERF_COUNT_HW_CACHE_RESULT_ACCESS),
         o->have_loads ? o->stores_raw : cache_config(PERF_COUNT_HW_CACHE_OP_WRITE,
                                                    PERF_COUNT_HW_CACHE_RESULT_ACCESS)}
    };
    for (int g = 0; g < 3; ++g) {
        CounterResult *c = &r->counters[g];
        c->available = 1;
        for (int rep = 0; rep < o->repeats; ++rep) {
            PerfPair pair;
            PerfReading reading;
            int ok = PerfPairOpen(&pair, types[g], configs[g][0], configs[g][1]);
            if (ok == 0)
                ok = PerfPairStart(&pair);
            if (ok == 0) {
                size_t hits;
                result_sink = run_queries(fn, d, o, &hits);
                ok = PerfPairStop(&pair, &reading);
            }
            if (ok < 0) {
                c->available = 0;
                c->error = pair.error;
                c->stage = pair.stage;
                PerfPairClose(&pair);
                break;
            }
            c->a += reading.value[0] / o->repeats;
            c->b += reading.value[1] / o->repeats;
            c->enabled += (double)reading.time_enabled;
            c->running += (double)reading.time_running;
            PerfPairClose(&pair);
        }
    }
}

static void json_string(const char *s)
{
    putchar('"');
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); }
        else if (ch < 32) printf("\\u%04x", ch);
        else putchar(ch);
    }
    putchar('"');
}

static void json_number(int available, double value)
{
    if (available) printf("%.6f", value);
    else fputs("null", stdout);
}

static int l1_valid(const CounterResult *c)
{
    return c->available && c->a > 0 && c->b >= 0 && c->b <= c->a;
}

static void print_json(const Options *o, const Dataset *d, const Result r[2])
{
    printf("{\n  \"schema_version\": 1,\n  \"config\": {\"nodes_per_list\": %zu, "
           "\"lists\": %zu, \"queries\": %zu, \"size\": %d, \"repeats\": %d, "
           "\"hit_rate_percent\": %d, \"pattern\": ", o->nodes, o->lists,
           o->queries, o->size, o->repeats, o->hit_rate);
    json_string(o->pattern);
    printf(", \"seed\": %" PRIu64 ", \"cpu\": %d, \"loads_raw\": ", o->seed, o->cpu);
    if (o->have_loads) printf("%" PRIu64, o->loads_raw); else fputs("null", stdout);
    fputs(", \"stores_raw\": ", stdout);
    if (o->have_stores) printf("%" PRIu64, o->stores_raw); else fputs("null", stdout);
    printf("},\n  \"verification\": \"passed\",\n  \"expected_hits\": %zu,\n"
           "  \"build_ms\": %.6f,\n  \"prepare_ms\": %.6f,\n"
           "  \"list_bytes_before_index\": %zu,\n  \"list_bytes_after_index\": %zu,\n"
           "  \"memory_accesses_definition\": ", d->hits, d->build_ms, d->prepare_ms,
           d->memory_before, d->memory_after);
    json_string(memory_definition(o));
    fputs(",\n  \"l1d_hit_rate_definition\": \"100 * (1 - generic L1D.READ.MISS / L1D.READ.ACCESS); "
          "kernel/CPU mapping dependent estimate, not L1 instruction cache\"", stdout);
    fputs(",\n  \"measurement_scope\": \"warmed search batch plus query loop and calls; "
          "excludes construction, index preparation, verification and output; "
          "three hardware groups replay the same query batch separately\",\n  \"results\": [\n", stdout);
    for (int a = 0; a < 2; ++a) {
        const CounterResult *core = &r[a].counters[0], *l1 = &r[a].counters[1],
                            *mem = &r[a].counters[2];
        fputs("    {\"algorithm\": ", stdout); json_string(r[a].name);
        printf(", \"ns_per_query_median\": %.6f, \"ns_per_query_min\": %.6f, "
               "\"ns_per_query_max\": %.6f, \"hits\": %zu, \"checksum\": %" PRIu64,
               r[a].median, r[a].min, r[a].max, r[a].hits, r[a].checksum);
        fputs(", \"timing_samples_ns_per_query\": [", stdout);
        for (int i = 0; i < o->repeats; ++i)
            printf("%s%.6f", i ? ", " : "", r[a].times[i]);
        fputs("], \"instructions\": ", stdout); json_number(core->available, core->a);
        fputs(", \"cycles\": ", stdout); json_number(core->available, core->b);
        fputs(", \"memory_accesses\": ", stdout); json_number(mem->available, mem->a + mem->b);
        fputs(", \"l1d_read_accesses\": ", stdout); json_number(l1->available, l1->a);
        fputs(", \"l1d_read_misses\": ", stdout); json_number(l1->available, l1->b);
        fputs(", \"l1d_hit_rate_percent\": ", stdout);
        json_number(l1_valid(l1), l1_valid(l1) ? 100 * (1 - l1->b / l1->a) : 0);
        fputs(", \"counter_groups\": [", stdout);
        for (int g = 0; g < 3; ++g) {
            const CounterResult *c = &r[a].counters[g];
            printf("%s{\"available\": %s, \"errno\": %d, \"error\": ", g ? ", " : "",
                   c->available ? "true" : "false", c->error);
            json_string(c->available ? "" : strerror(c->error));
            fputs(", \"stage\": ", stdout); json_string(c->stage ? c->stage : "");
            fputs(", \"running_percent\": ", stdout);
            json_number(c->available && c->enabled > 0,
                        c->enabled > 0 ? 100 * c->running / c->enabled : 0);
            putchar('}');
        }
        printf("]}%s\n", a == 0 ? "," : "");
    }
    printf("  ],\n  \"speedup\": %.6f\n}\n", r[0].median / r[1].median);
}

static void text_value(const char *name, int valid, double value)
{
    printf("  %-29s", name);
    if (valid) printf("%.3f\n", value);
    else puts("N/A");
}

static void print_text(const Options *o, const Dataset *d, const Result r[2])
{
    printf("nodes/list=%zu lists=%zu queries=%zu size=%d repeats=%d hit=%d%% pattern=%s cpu=%d\n",
           o->nodes, o->lists, o->queries, o->size, o->repeats, o->hit_rate, o->pattern, o->cpu);
    printf("verification=passed; build=%.3f ms; prepare=%.3f ms; list bytes=%zu -> %zu\n",
           d->build_ms, d->prepare_ms, d->memory_before, d->memory_after);
    printf("memory_accesses = %s\n", memory_definition(o));
    puts("L1D hit rate = 100*(1-generic READ.MISS/READ.ACCESS); CPU-dependent estimate.");
    puts("Hardware counts: mean per query batch, scaled by time_enabled/time_running.");
    puts("Scope: warmed searches + identical query-loop/call overhead; three group replays.");
    for (int a = 0; a < 2; ++a) {
        const CounterResult *c = r[a].counters;
        printf("%s: %.3f ns/query (min %.3f, max %.3f), hits=%zu checksum=%" PRIu64 "\n",
               r[a].name, r[a].median, r[a].min, r[a].max, r[a].hits, r[a].checksum);
        text_value("instructions", c[0].available, c[0].a);
        text_value("cycles", c[0].available, c[0].b);
        text_value("memory_accesses", c[2].available, c[2].a + c[2].b);
        text_value("L1D_read_accesses", c[1].available, c[1].a);
        text_value("L1D_read_misses", c[1].available, c[1].b);
        text_value("L1D_hit_rate_percent", l1_valid(&c[1]),
                   l1_valid(&c[1]) ? 100 * (1 - c[1].b / c[1].a) : 0);
        for (int g = 0; g < 3; ++g)
            if (!c[g].available)
                printf("  group %d unavailable: %s: %s (errno=%d)\n", g,
                       c[g].stage ? c[g].stage : "unknown", strerror(c[g].error), c[g].error);
    }
    printf("speedup=%.3fx\n", r[0].median / r[1].median);
}

int main(int argc, char **argv)
{
    Options o;
    Dataset d;
    Result results[2] = {{.name = "linear"}, {.name = "indexed"}};
    SearchFn functions[2] = {linear_adapter, FindNode};
    int missing = 0;
    if (options(argc, argv, &o))
        return 1;
    if (pin_cpu(&o) < 0) {
        perror("CPU affinity");
        return 1;
    }
    select_memory_events(&o);
    if (create_dataset(&d, &o) < 0) {
        perror("dataset/index allocation");
        destroy_dataset(&d, &o);
        return 1;
    }
    if (verify_and_warm(&d, &o) < 0) {
        destroy_dataset(&d, &o);
        return 1;
    }
    /* Alternate order to reduce a systematic first/second algorithm bias. */
    for (int rep = 0; rep < o.repeats; ++rep) {
        for (int position = 0; position < 2; ++position) {
            int a = position ^ (rep & 1);
            double t = now_ns();
            results[a].checksum = run_queries(functions[a], &d, &o, &results[a].hits);
            results[a].times[rep] = (now_ns() - t) / (double)o.queries;
            result_sink = results[a].checksum;
        }
    }
    for (int a = 0; a < 2; ++a) {
        double sorted[31];
        memcpy(sorted, results[a].times, (size_t)o.repeats * sizeof(double));
        qsort(sorted, (size_t)o.repeats, sizeof(double), compare_doubles);
        results[a].min = sorted[0];
        results[a].max = sorted[o.repeats - 1];
        results[a].median = o.repeats & 1 ? sorted[o.repeats / 2] :
                            (sorted[o.repeats / 2 - 1] + sorted[o.repeats / 2]) / 2;
        if (results[a].hits != d.hits || results[a].checksum != results[0].checksum) {
            fputs("Measured result mismatch\n", stderr);
            destroy_dataset(&d, &o);
            return 1;
        }
        measure_counters(&results[a], functions[a], &d, &o);
        for (int g = 0; g < 3; ++g)
            if (!results[a].counters[g].available)
                missing = 1;
        if (!l1_valid(&results[a].counters[1]))
            missing = 1;
    }
    if (o.json) print_json(&o, &d, results);
    else print_text(&o, &d, results);
    destroy_dataset(&d, &o);
    if (missing)
        fputs("Some PMU events are unavailable/invalid. N/A/null are not zero. "
              "See README.md for permissions and CPU event support.\n", stderr);
    return missing && o.require_perf ? 2 : 0;
}
