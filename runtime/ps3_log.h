/* ps3_log.h -- gate for high-volume diagnostic logging.
 *
 * Chatty per-wait / per-round logs stay FULL when stderr is redirected
 * (debug runs capture to a file) or PS3_VERBOSE is set, and go quiet on a
 * live console: conhost flushes stall the emitting threads, and the ~20k
 * [WAIT] lines/minute of an LBP intro run showed up as harsh ~1 Hz hitches
 * in user-visible runs while file-redirected runs played smoothly.
 */
#ifndef PS3_LOG_H
#define PS3_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define PS3_ISATTY_STDERR() _isatty(_fileno(stderr))
#else
#include <unistd.h>
#define PS3_ISATTY_STDERR() isatty(fileno(stderr))
#endif

#define PS3_TRACE_WAIT  0x1u
#define PS3_TRACE_EVT   0x2u
#define PS3_TRACE_SPURS 0x4u
#define PS3_TRACE_GCM   0x8u
#define PS3_TRACE_ALL   0xFu

#ifdef __cplusplus
static inline int ps3_log_verbose(void)
#else
static __inline int ps3_log_verbose(void)
#endif
{
    /* PS3_VERBOSE, when set, decides -- INCLUDING "0" to force quiet. Without it
     * the old default stands: a redirected stderr means someone is capturing a
     * log, so be verbose.
     *
     * That default has a sharp edge worth knowing about. The per-event lines are
     * emitted from every guest thread through one FILE lock, and Windows locks
     * are not fair, so at the ~6k lines/s a SPURS audio loop produces a thread
     * can starve on the lock for seconds. Tokyo Jungle presents four frames in
     * 40 s with the log discarded and one with it redirected to a file -- the
     * logging changes what the title does. Measure timing-sensitive behaviour
     * with PS3_VERBOSE=0. */
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PS3_VERBOSE");
        if (e) v = (e[0] && e[0] != '0') ? 1 : 0;
        else   v = !PS3_ISATTY_STDERR();
    }
    return v;
}

#ifdef __cplusplus
static inline unsigned ps3_trace_parse(const char* list)
#else
static __inline unsigned ps3_trace_parse(const char* list)
#endif
{
    static const struct { const char* name; unsigned bit; } cats[] = {
        { "wait", PS3_TRACE_WAIT }, { "evt", PS3_TRACE_EVT },
        { "spurs", PS3_TRACE_SPURS }, { "gcm", PS3_TRACE_GCM }, { "all", PS3_TRACE_ALL },
    };
    unsigned mask = 0;
    while (*list) {
        size_t len = strcspn(list, ",");
        for (size_t i = 0; i < sizeof cats / sizeof cats[0]; i++)
            if (strlen(cats[i].name) == len && strncmp(list, cats[i].name, len) == 0)
                mask |= cats[i].bit;
        list += len;
        if (*list) list++;
    }
    return mask;
}

#ifdef __cplusplus
static inline int ps3_trace(unsigned category)
#else
static __inline int ps3_trace(unsigned category)
#endif
{
    static int mask = -1;
    if (mask < 0) {
        const char* list = getenv("PS3RECOMP_TRACE");
        const char* verbose = getenv("PS3_VERBOSE");
        if (list) mask = (int)ps3_trace_parse(list);
        else mask = (verbose && verbose[0] && verbose[0] != '0') ? (int)PS3_TRACE_ALL : 0;
    }
    return (mask & (int)category) != 0;
}

#endif /* PS3_LOG_H */
