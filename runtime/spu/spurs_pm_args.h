#ifndef SPURS_PM_ARGS_H
#define SPURS_PM_ARGS_H

#include <stdint.h>

#include "spu_context.h"

static inline void spurs_pm_set_wkl_arg(spu_context* ctx, uint64_t wkl_data)
{
    ctx->gpr[4]._u32[0] = (uint32_t)(wkl_data >> 32);
    ctx->gpr[4]._u32[1] = (uint32_t)wkl_data;
    ctx->gpr[4]._u32[2] = 0;
    ctx->gpr[4]._u32[3] = 0;
}

#endif
