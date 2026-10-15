#include "ps3_timeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/syscall.h>
#endif

typedef struct {
    uint64_t t_ns;
    uint32_t tid;
    uint16_t type;
    uint16_t pad;
    uint32_t arg;
    uint32_t pad2;
} ps3_tl_record;

enum { PS3_TL_CAPACITY = 1u << 22 };

volatile int g_ps3_timeline_on = 0;

static ps3_tl_record* s_records;
static volatile uint32_t s_count;
static uint64_t s_begin_ns, s_end_ns;
static const char* s_path;
static volatile int s_dumped;

static uint64_t ps3_tl_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ps3_tl_dump(void)
{
    if (__atomic_exchange_n(&s_dumped, 1, __ATOMIC_ACQ_REL)) return;
    g_ps3_timeline_on = 0;
    FILE* f = fopen(s_path, "wb");
    if (!f) return;
    uint32_t n = s_count < PS3_TL_CAPACITY ? s_count : PS3_TL_CAPACITY;
    fwrite(s_records, sizeof *s_records, n, f);
    fclose(f);
    fprintf(stderr, "[timeline] %u events written to %s\n", n, s_path);
}

__attribute__((constructor)) static void ps3_tl_init(void)
{
    const char* path = getenv("PS3_TIMELINE");
    const char* at = getenv("PS3_TIMELINE_AT");
    const char* len = getenv("PS3_TIMELINE_SECONDS");
    if (!path || !at) { g_ps3_timeline_on = 0; return; }
    s_records = (ps3_tl_record*)calloc(PS3_TL_CAPACITY, sizeof *s_records);
    if (!s_records) { g_ps3_timeline_on = 0; return; }
    s_path = path;
    const uint64_t now = ps3_tl_now();
    s_begin_ns = now + (uint64_t)(atof(at) * 1e9);
    s_end_ns = s_begin_ns + (uint64_t)((len ? atof(len) : 3.0) * 1e9);
    g_ps3_timeline_on = 1;
}

void ps3_timeline_event(uint32_t type, uint32_t arg)
{
    const uint64_t now = ps3_tl_now();
    if (now < s_begin_ns) return;
    if (now >= s_end_ns) { ps3_tl_dump(); return; }
    const uint32_t i = __atomic_fetch_add(&s_count, 1u, __ATOMIC_RELAXED);
    if (i >= PS3_TL_CAPACITY) { ps3_tl_dump(); return; }
    ps3_tl_record* r = &s_records[i];
    r->t_ns = now - s_begin_ns;
#ifdef _WIN32
    r->tid = (uint32_t)GetCurrentThreadId();
#else
    r->tid = (uint32_t)syscall(SYS_gettid);
#endif
    r->type = (uint16_t)type;
    r->arg = arg;
}
