#include "ps3_ab.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <time.h>
#endif

#define PS3_AB_MAX          32u
#define PS3_AB_DEFAULT_SEC  10.0
#define PS3_AB_MIN_SEC      2.0

static ps3_ab_switch s_switches[PS3_AB_MAX];
static unsigned s_count;
static unsigned (*s_frames)(void);
static double s_seconds;

unsigned ps3_ab_parse(const char* spec, const ps3_ab_switch* table, unsigned count)
{
    unsigned mask = 0;
    if (count > PS3_AB_MAX) count = PS3_AB_MAX;
    while (spec && *spec) {
        const char* end = strchr(spec, ',');
        const size_t n = end ? (size_t)(end - spec) : strlen(spec);
        int known = 0;
        for (unsigned i = 0; i < count; i++)
            if ((n == 3 && strncmp(spec, "all", 3) == 0) ||
                (strlen(table[i].name) == n && strncmp(spec, table[i].name, n) == 0)) {
                mask |= 1u << i;
                known = 1;
            }
        if (!known && n)
            fprintf(stderr, "[AB] unknown switch '%.*s' ignored\n", (int)n, spec);
        if (!end) break;
        spec = end + 1;
    }
    return mask;
}

#if !defined(_WIN32)
static double ab_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void ab_sleep(double seconds)
{
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) != 0) {}
}

static void ab_set(int on)
{
    for (unsigned i = 0; i < s_count; i++) s_switches[i].set(on);
}

static void* ab_main(void* arg)
{
    (void)arg;
    double sum[2] = { 0.0, 0.0 }, sum_d = 0.0, sum_d2 = 0.0, last_a = 0.0;
    unsigned n[2] = { 0, 0 }, pairs = 0;
    for (unsigned k = 0;; k++) {
        const int b = (int)(k & 1u);
        ab_set(!b);
        const unsigned f0 = s_frames();
        const double t0 = ab_now();
        ab_sleep(s_seconds);
        const double fps = (double)(s_frames() - f0) / (ab_now() - t0);
        sum[b] += fps;
        n[b]++;
        fprintf(stderr, "[AB] phase %u %s: %.2f fps\n", k, b ? "B (off)" : "A (on)", fps);
        if (!b) {
            last_a = fps;
            continue;
        }
        const double d = fps - last_a;
        sum_d += d;
        sum_d2 += d * d;
        pairs++;
        const double mean_d = sum_d / pairs;
        const double var = pairs > 1 ? (sum_d2 - pairs * mean_d * mean_d) / (pairs - 1) : 0.0;
        fprintf(stderr, "[AB] %u pairs: A %.2f fps, B %.2f fps, B-A %+.2f fps (sd %.2f)\n", pairs,
                sum[0] / n[0], sum[1] / n[1], mean_d, var > 0.0 ? sqrt(var) : 0.0);
    }
    return NULL;
}
#endif

int ps3_ab_start(const ps3_ab_switch* table, unsigned count, unsigned (*frames)(void))
{
    const char* spec = getenv("PS3_AB");
    if (!spec || !*spec || !frames) return 0;
    if (count > PS3_AB_MAX) count = PS3_AB_MAX;
    const unsigned mask = ps3_ab_parse(spec, table, count);
    if (!mask) return 0;
#if defined(_WIN32)
    fprintf(stderr, "[AB] in-run A/B is not available on this platform\n");
    return 0;
#else
    s_count = 0;
    char names[512] = "";
    for (unsigned i = 0; i < count; i++)
        if ((mask >> i) & 1u) {
            s_switches[s_count++] = table[i];
            if (strlen(names) + strlen(table[i].name) + 2 < sizeof names) {
                if (names[0]) strcat(names, ",");
                strcat(names, table[i].name);
            }
        }
    const char* e = getenv("PS3_AB_SEC");
    s_seconds = e ? atof(e) : PS3_AB_DEFAULT_SEC;
    if (s_seconds < PS3_AB_MIN_SEC) s_seconds = PS3_AB_MIN_SEC;
    s_frames = frames;
    pthread_t thread;
    if (pthread_create(&thread, NULL, ab_main, NULL) != 0) return 0;
    pthread_detach(thread);
    fprintf(stderr, "[AB] in-run A/B of %s: phases of %.1f s, A = on, B = off\n", names, s_seconds);
    return 1;
#endif
}
