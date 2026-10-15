#ifndef SPU_LINE_TRACE_H
#define SPU_LINE_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SPU_LINE_TRACE_GETLLAR = 1, SPU_LINE_TRACE_PUTLLC_OK, SPU_LINE_TRACE_PUTLLC_FAIL,
    SPU_LINE_TRACE_PUT, SPU_LINE_TRACE_PPU_CAS, SPU_LINE_TRACE_PPU_STORE
};

typedef struct spu_line_trace_entry {
    uint64_t ns;
    uint32_t seq, kind, ea, size, pc, lr, spu_num, tid, line_ea;
    const void* actor;
    uint8_t line[128];
} spu_line_trace_entry;

extern uint32_t g_spu_line_trace_ea;
extern uint32_t g_spu_line_trace_n;
extern uint32_t g_spu_line_trace_eas[32];
int spu_line_trace_hit(uint32_t ea, uint32_t size);
extern spu_line_trace_entry g_spu_line_trace[4096];
extern volatile uint32_t g_spu_line_trace_seq;

void spu_line_trace_init(void);
void spu_line_trace_note(uint32_t kind, uint32_t ea, uint32_t size, const void* actor,
                         uint32_t pc, uint32_t lr, uint32_t spu_num);
#define SPU_LINE_TRACE_SPUNUM(c) \
    (((uint32_t)(c)->ls[0x1C8] << 24) | ((uint32_t)(c)->ls[0x1C9] << 16) | \
     ((uint32_t)(c)->ls[0x1CA] << 8) | (uint32_t)(c)->ls[0x1CB])
#define SPU_LINE_TRACE_HIT(ea, size) \
    (g_spu_line_trace_ea && spu_line_trace_hit((uint32_t)(ea), (uint32_t)(size)))

#ifdef __cplusplus
}
#endif

#endif
