#ifndef SPU_REGCACHE_H
#define SPU_REGCACHE_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_rc_fn)(spu_context*);

void spu_rc_add_pairs(const void* const* pairs, unsigned n);
int spu_rc_active(void);
spu_rc_fn spu_rc_select(spu_rc_fn fn);
spu_rc_fn spu_rc_canonical(spu_rc_fn fn);
void spu_rc_set_enabled(int on);

#ifdef __cplusplus
}
#endif

#endif
