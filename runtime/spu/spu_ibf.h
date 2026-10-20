#ifndef SPU_IBF_H
#define SPU_IBF_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_ibf_fn)(spu_context*);

#define SPU_IBF_HANDLED ((spu_ibf_fn)(uintptr_t)1)

spu_ibf_fn spu_ibf_take(spu_context* ctx, uint32_t target);
spu_ibf_fn spu_ibf_take_return(spu_context* ctx, uint32_t target);
spu_ibf_fn spu_ibf_resolve_call(spu_context* ctx);

static inline void spu_ibf_push(spu_context* ctx, uint32_t return_pc)
{
    const uint32_t n = ctx->ibf_ret_n;
    if (n < SPU_IBF_RET_MAX) {
        ctx->ibf_ret_depth[n] = ctx->host_depth;
        ctx->ibf_ret_pc[n] = return_pc & SPU_LS_MASK;
    }
    ctx->ibf_ret_n = n + 1u;
}

static inline void spu_ibf_pop(spu_context* ctx)
{
    if (ctx->ibf_ret_n) ctx->ibf_ret_n--;
}

#if defined(__clang__)
#define SPU_IBF_TAIL(ctx, f) __attribute__((musttail)) return (f)(ctx)
#define SPU_IBF_DISPATCH(ctx, f, self)                                  \
    if ((f) == SPU_IBF_HANDLED) return;                                 \
    if ((f) == (spu_ibf_fn)(self)) goto spu_ibf_top;                    \
    SPU_IBF_TAIL(ctx, f)
#define SPU_IBF_SITE(ctx, n, self)                                      \
    { spu_ibf_fn _ibf = spu_ibf_take((ctx), (ctx)->gpr[n]._u32[0]);   \
      if (_ibf) { SPU_IBF_DISPATCH(ctx, _ibf, self); } }
#define SPU_IBF_RETSELF(ctx, selfpc, self)                              \
    { if (((ctx)->gpr[0]._u32[0] & SPU_LS_MASK) == (selfpc)) {          \
          spu_ibf_fn _ibf = spu_ibf_take_return((ctx), (ctx)->gpr[0]._u32[0]); \
          if (_ibf) { SPU_IBF_DISPATCH(ctx, _ibf, self); } } }
#define SPU_IBF_CALL(ctx)                                               \
    { spu_ibf_fn _ibf = spu_ibf_resolve_call(ctx);                      \
      if (_ibf) _ibf(ctx); else spu_indirect_branch(ctx); }
#else
#define SPU_IBF_SITE(ctx, n, self) { if (0) goto spu_ibf_top; }
#define SPU_IBF_RETSELF(ctx, selfpc, self) { if (0) goto spu_ibf_top; }
#define SPU_IBF_CALL(ctx) spu_indirect_branch(ctx)
#endif

#ifdef __cplusplus
}
#endif

#endif
