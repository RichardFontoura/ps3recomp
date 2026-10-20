#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_ibf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x00100000u;

#include "../../syscalls/lv2_register.c"

enum { IMG = 91, FUNCS = 5 };

typedef struct {
    uint32_t gpr[16][4];
    uint64_t steps;
    uint32_t pc, stop_code, status, host_depth, ibf_ret_n, drain_ret_pc;
    uint32_t recent[32];
    unsigned recent_n;
    uint32_t pch[8];
    unsigned entries[FUNCS];
    int halted;
} outcome;

void spu_begin_image(int image_id);
void spu_register_function(uint32_t addr, void (*fn)(spu_context*));
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
unsigned spu_recent_pcs(uint32_t* out, unsigned max);
void spu_stop(spu_context* ctx);

static unsigned g_entries[FUNCS];
static int g_pass, g_fail;

__attribute__((weak)) uint32_t cellGcm_flip_request_count(void) { return 0; }

int64_t sys_event_flag_set(ppu_context* c)
{
    (void)c;
    return 0;
}

void sys_ppu_thread_init(lv2_syscall_table* t) { (void)t; }
void sys_mutex_init(lv2_syscall_table* t)      { (void)t; }
void sys_cond_init(lv2_syscall_table* t)       { (void)t; }
void sys_semaphore_init(lv2_syscall_table* t)  { (void)t; }
void sys_rwlock_init(lv2_syscall_table* t)     { (void)t; }
void sys_event_init(lv2_syscall_table* t)      { (void)t; }
void sys_timer_init(lv2_syscall_table* t)      { (void)t; }
void sys_memory_init(lv2_syscall_table* t)     { (void)t; }
void sys_vm_init(lv2_syscall_table* t)         { (void)t; }
void sys_fs_init(lv2_syscall_table* t)         { (void)t; }
const char* np_psnr_identity(void)            { return NULL; }

int sys_event_queue_push_by_id(uint32_t q, uint64_t d0, uint64_t d1,
                               uint64_t d2, uint64_t d3)
{
    (void)q; (void)d0; (void)d1; (void)d2; (void)d3;
    return 0;
}
void sys_fs_translate_path(const char* ps3_path, char* host, int size)
{
    (void)ps3_path;
    if (host && size > 0) host[0] = 0;
}
void sys_process_exit(int32_t status)            { exit((int)status); }
int32_t sys_process_getpid(void)                 { return 1; }

void ppu_dump_guest_stack(ppu_context* c, const char* tag) { (void)c; (void)tag; }
void ppu_guard_page(uint32_t ea)                 { (void)ea; }
void ppu_guest_caller(char* out, size_t n)       { if (out && n) out[0] = 0; }
void ppu_guest_callstack(const char* tag)        { (void)tag; }
void ppu_log_host_chain(const char* tag)         { (void)tag; }
void ps3_hle_register_ctx(uint32_t nid, const char* name,
                          void (*fn)(ppu_context*))
{
    (void)nid; (void)name; (void)fn;
}
void ppu_resv_break_store(uint64_t ea)           { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)              { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
int      g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint32_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t ps3_spu_image_source_ea(uint32_t ea) { return ea; }

uint32_t spurs_port_queue(uint32_t port)        { (void)port; return 0; }
void sys_raw_spu_init(lv2_syscall_table* t) { (void)t; }
void sys_rsx_init(lv2_syscall_table* t) { (void)t; }
void spu_raw_note_image(uint32_t ea, uint32_t src) { (void)ea; (void)src; }
int32_t spu_registry_fallback(uint32_t tid, uint32_t args, uint32_t size, void* user)
{ (void)tid; (void)args; (void)size; (void)user; return -1; }

void t_4000(spu_context* ctx);
void t_4100(spu_context* ctx);
void t_4200(spu_context* ctx);
void t_4300(spu_context* ctx);
void t_4400(spu_context* ctx);

void t_4000(spu_context* ctx)
{
    g_entries[0]++;
spu_ibf_top: ;
    ctx->gpr[3] = spu_ai(ctx->gpr[3], -1);
    ctx->gpr[7] = spu_a(ctx->gpr[7], ctx->gpr[3]);
    if (ctx->gpr[3]._u32[0] != 0) { SPU_IBF_SITE(ctx, 5, t_4000) ctx->pc = ctx->gpr[5]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return; }
    ctx->gpr[12] = spu_ila(0x4100);
    { SPU_IBF_SITE(ctx, 12, t_4000) ctx->pc = ctx->gpr[12]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return; }
}

void t_4100(spu_context* ctx)
{
    g_entries[1]++;
    ctx->gpr[0] = spu_link(0x4110);
    { int32_t _si = (int32_t)ctx->image_id; ctx->host_depth++; spu_ibf_push(ctx, 0x4110); t_4200(ctx); spu_drain_call(ctx, 0x4110); spu_ibf_pop(ctx); ctx->host_depth--; spu_img_restore(ctx, _si); }
    { uint32_t _bt = ctx->gpr[9]._u32[0]; ctx->gpr[0] = spu_link(0x4120); int32_t _si = (int32_t)ctx->image_id; ctx->pc = _bt; ctx->host_depth++; spu_ibf_push(ctx, 0x4120); ctx->drain_ret_pc = 0x4120; SPU_IBF_CALL(ctx); spu_drain_call(ctx, 0x4120); spu_ibf_pop(ctx); ctx->host_depth--; spu_img_restore(ctx, _si); }
    ctx->stop_code = 0x3FFFu; ctx->status = SPU_STATUS_STOPPED_BY_STOP; spu_stop(ctx); return;
}

void t_4200(spu_context* ctx)
{
    g_entries[2]++;
spu_ibf_top: ;
    ctx->gpr[13] = spu_ai(ctx->gpr[13], -1);
    ctx->gpr[7] = spu_xor(ctx->gpr[7], ctx->gpr[13]);
    ctx->gpr[14] = spu_ila(0x4200);
    if (ctx->gpr[13]._u32[0] != 0) { SPU_IBF_SITE(ctx, 14, t_4200) ctx->pc = ctx->gpr[14]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return; }
    ctx->gpr[15] = spu_ila(0x4400);
    { SPU_IBF_SITE(ctx, 15, t_4200) ctx->pc = ctx->gpr[15]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return; }
}

void t_4300(spu_context* ctx)
{
    g_entries[3]++;
spu_ibf_top: ;
    ctx->gpr[10] = spu_ai(ctx->gpr[10], -1);
    ctx->gpr[7] = spu_a(ctx->gpr[7], ctx->gpr[10]);
    if (ctx->gpr[10]._u32[0] != 0) ctx->gpr[0] = spu_ila(0x4300); else ctx->gpr[0] = ctx->gpr[11];
    SPU_IBF_RETSELF(ctx, 0x4300u, t_4300) SPU_RET(ctx);
}

void t_4400(spu_context* ctx)
{
    g_entries[4]++;
spu_ibf_top: ;
    ctx->gpr[7] = spu_a(ctx->gpr[7], ctx->gpr[6]);
    { SPU_IBF_SITE(ctx, 6, t_4400) ctx->pc = ctx->gpr[6]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return; }
}

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

static void run_program(outcome* out)
{
    spu_begin_image(IMG);
    spu_register_function(0x4000, t_4000);
    spu_register_function(0x4100, t_4100);
    spu_register_function(0x4200, t_4200);
    spu_register_function(0x4300, t_4300);
    spu_register_function(0x4400, t_4400);
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) exit(3);
    ctx->ls = ctx->ls_store;
    ctx->image_id = IMG;
    ctx->pc = 0x4000;
    ctx->gpr[3]._u32[0] = 5;
    ctx->gpr[5]._u32[0] = 0x4000;
    ctx->gpr[6]._u32[0] = 0x4110;
    ctx->gpr[7]._u32[0] = 0x1234;
    ctx->gpr[9]._u32[0] = 0x4300;
    ctx->gpr[10]._u32[0] = 4;
    ctx->gpr[11]._u32[0] = 0x4120;
    ctx->gpr[13]._u32[0] = 3;
    memset(out, 0, sizeof *out);
    out->halted = spu_run_with_halt(t_4000, ctx);
    for (int r = 0; r < 16; r++) memcpy(out->gpr[r], ctx->gpr[r]._u32, sizeof out->gpr[r]);
    out->steps = ctx->steps;
    out->pc = ctx->pc;
    out->stop_code = ctx->stop_code;
    out->status = ctx->status;
    out->host_depth = ctx->host_depth;
    out->ibf_ret_n = ctx->ibf_ret_n;
    out->drain_ret_pc = ctx->drain_ret_pc;
    out->recent_n = spu_recent_pcs(out->recent, 32);
    memcpy(out->pch, g_spu_pch, sizeof out->pch);
    memcpy(out->entries, g_entries, sizeof out->entries);
    free(ctx);
}

static int run_child(const char* ibf, outcome* out)
{
    int fds[2];
    if (pipe(fds) != 0) return 0;
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (ibf) setenv("SPU_IBF", ibf, 1); else unsetenv("SPU_IBF");
        outcome o;
        run_program(&o);
        _exit(write(fds[1], &o, sizeof o) == (ssize_t)sizeof o ? 0 : 1);
    }
    close(fds[1]);
    const ssize_t got = read(fds[0], out, sizeof *out);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return got == (ssize_t)sizeof *out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void)
{
    outcome slow, fast;
    check(run_child("0", &slow), "the program runs with SPU_IBF=0");
    check(run_child(NULL, &fast), "the program runs with the lean dispatch on");
    check(slow.stop_code == 0x3FFFu && slow.status == SPU_STATUS_STOPPED_BY_STOP, "the program reaches its stop");
    check(memcmp(slow.gpr, fast.gpr, sizeof slow.gpr) == 0, "same registers");
    check(slow.steps == fast.steps, "same step count");
    check(slow.pc == fast.pc && slow.stop_code == fast.stop_code && slow.status == fast.status,
          "same pc, stop code and status");
    check(slow.host_depth == fast.host_depth && slow.ibf_ret_n == fast.ibf_ret_n && fast.ibf_ret_n == 0,
          "same host depth; pending returns balanced");
    check(slow.drain_ret_pc == fast.drain_ret_pc, "same drain return pc");
    check(slow.recent_n == fast.recent_n && memcmp(slow.recent, fast.recent, sizeof slow.recent) == 0,
          "same trampoline-hop history");
    check(memcmp(slow.pch, fast.pch, sizeof slow.pch) == 0, "same top-level pc ring");
#if defined(__clang__)
    check(slow.entries[0] == 5 && fast.entries[0] == 1, "top-level self loop stays in one host call");
    check(slow.entries[2] == 3 && fast.entries[2] == 1, "nested self loop stays in one host call");
    check(slow.entries[3] == 4 && fast.entries[3] == 1, "self return loop stays in one host call");
#else
    check(memcmp(slow.entries, fast.entries, sizeof slow.entries) == 0, "no lean dispatch without musttail");
#endif
    check(slow.entries[1] == 1 && fast.entries[1] == 1 && slow.entries[4] == 1 && fast.entries[4] == 1,
          "every other function runs once");
    printf("SPU lean dispatch: %d passed, %d failed (steps %llu, r7 %08X, halted %d, stop 0x%X, status 0x%X)\n", g_pass, g_fail,
           (unsigned long long)slow.steps, slow.gpr[7][0], slow.halted, slow.stop_code, slow.status);
    return g_fail != 0;
}
