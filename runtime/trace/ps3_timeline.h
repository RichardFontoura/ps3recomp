#ifndef PS3_TIMELINE_H
#define PS3_TIMELINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS3_TL_PM_BEGIN = 1,
    PS3_TL_PM_END,
    PS3_TL_DRAIN_BEGIN,
    PS3_TL_DRAIN_END,
    PS3_TL_PRESENT,
    PS3_TL_USLEEP_BEGIN,
    PS3_TL_USLEEP_END,
    PS3_TL_TASK_BEGIN,
    PS3_TL_TASK_END,
    PS3_TL_SIGNAL_WAIT_BEGIN,
    PS3_TL_SIGNAL_WAIT_END,
    PS3_TL_GPU_FLUSH_BEGIN,
    PS3_TL_GPU_FLUSH_END,
    PS3_TL_FLIP_WORD,
    PS3_TL_REPORT
};

extern volatile int g_ps3_timeline_on;

void ps3_timeline_event(uint32_t type, uint32_t arg);

static inline void ps3_timeline(uint32_t type, uint32_t arg)
{
    if (g_ps3_timeline_on) ps3_timeline_event(type, arg);
}

#ifdef __cplusplus
}
#endif

#endif
