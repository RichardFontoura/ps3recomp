#include "rsx_vertex_workers.h"

#if !defined(_WIN32)
#include <pthread.h>
#include <sched.h>
#endif

#define VW_MAX_THREADS 16u

typedef struct vw_job {
    const rsx_vertex_fetch_plan* plan;
    const rsx_vertex_ref* refs;
    u8* out;
    u32 count;
    u32 stride;
    u32 chunk;
    u32 n_chunks;
    u32 generation;
} vw_job;

static vw_job s_job;
static u32 s_threads;
static u64 s_ticket;
static u32 s_done_chunks;
static int s_failed;
static int s_stop;
#if !defined(_WIN32)
static pthread_t s_thread[VW_MAX_THREADS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake = PTHREAD_COND_INITIALIZER;
#endif

int rsx_vertex_fetch_range(const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
                           u32 first, u32 count, u8* out, u32 stride)
{
    for (u32 i = first; i < first + count; i++) {
        u8* dst = stride ? out + (u64)i * stride : NULL;
        if (!rsx_vertex_fetch_one(plan, &refs[i], dst)) return 0;
    }
    return 1;
}

static void vw_run_chunks(const vw_job* job)
{
    for (;;) {
        u64 ticket = __atomic_load_n(&s_ticket, __ATOMIC_ACQUIRE);
        if ((u32)(ticket >> 32) != job->generation || (u32)ticket >= job->n_chunks) return;
        if (!__atomic_compare_exchange_n(&s_ticket, &ticket, ticket + 1u, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        const u32 first = (u32)ticket * job->chunk;
        const u32 left = job->count - first;
        const u32 n = left < job->chunk ? left : job->chunk;
        if (!rsx_vertex_fetch_range(job->plan, job->refs, first, n, job->out, job->stride))
            __atomic_store_n(&s_failed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_done_chunks, 1u, __ATOMIC_RELEASE);
    }
}

#if !defined(_WIN32)
static void* vw_main(void* arg)
{
    (void)arg;
    u32 seen = 0;
    pthread_mutex_lock(&s_lock);
    for (;;) {
        while (!s_stop && s_job.generation == seen) pthread_cond_wait(&s_wake, &s_lock);
        if (s_stop) break;
        const vw_job job = s_job;
        seen = job.generation;
        pthread_mutex_unlock(&s_lock);
        vw_run_chunks(&job);
        pthread_mutex_lock(&s_lock);
    }
    pthread_mutex_unlock(&s_lock);
    return NULL;
}
#endif

u32 rsx_vertex_workers_start(u32 threads)
{
    rsx_vertex_workers_stop();
#if !defined(_WIN32)
    if (threads > VW_MAX_THREADS) threads = VW_MAX_THREADS;
    for (u32 i = 0; i < threads; i++) {
        if (pthread_create(&s_thread[s_threads], NULL, vw_main, NULL) != 0) break;
        s_threads++;
    }
#else
    (void)threads;
#endif
    return s_threads;
}

void rsx_vertex_workers_stop(void)
{
#if !defined(_WIN32)
    if (!s_threads) return;
    pthread_mutex_lock(&s_lock);
    s_stop = 1;
    pthread_cond_broadcast(&s_wake);
    pthread_mutex_unlock(&s_lock);
    for (u32 i = 0; i < s_threads; i++) pthread_join(s_thread[i], NULL);
    s_threads = 0;
    s_stop = 0;
#endif
}

u32 rsx_vertex_workers_count(void)
{
    return s_threads;
}

int rsx_vertex_fetch_parallel(const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
                              u32 count, u8* out, u32 stride, u32 chunk)
{
    if (!chunk) chunk = 1024u;
    if (!s_threads || count <= chunk) return rsx_vertex_fetch_range(plan, refs, 0, count, out, stride);
#if !defined(_WIN32)
    pthread_mutex_lock(&s_lock);
    s_job.plan = plan;
    s_job.refs = refs;
    s_job.out = out;
    s_job.count = count;
    s_job.stride = stride;
    s_job.chunk = chunk;
    s_job.n_chunks = (count + chunk - 1u) / chunk;
    s_job.generation++;
    const vw_job job = s_job;
    __atomic_store_n(&s_failed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_done_chunks, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_ticket, (u64)job.generation << 32, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&s_wake);
    pthread_mutex_unlock(&s_lock);
    vw_run_chunks(&job);
    while (__atomic_load_n(&s_done_chunks, __ATOMIC_ACQUIRE) < job.n_chunks) sched_yield();
    return !__atomic_load_n(&s_failed, __ATOMIC_RELAXED);
#else
    return rsx_vertex_fetch_range(plan, refs, 0, count, out, stride);
#endif
}
