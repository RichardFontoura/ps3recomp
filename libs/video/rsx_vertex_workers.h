#ifndef PS3RECOMP_RSX_VERTEX_WORKERS_H
#define PS3RECOMP_RSX_VERTEX_WORKERS_H

#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

u32 rsx_vertex_workers_start(u32 threads);
void rsx_vertex_workers_stop(void);
u32 rsx_vertex_workers_count(void);
int rsx_vertex_fetch_range(const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
                           u32 first, u32 count, u8* out, u32 stride);
int rsx_vertex_fetch_parallel(const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
                              u32 count, u8* out, u32 stride, u32 chunk);

#ifdef __cplusplus
}
#endif

#endif
