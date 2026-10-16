#include "../cellGcmSys.c"
#include <assert.h>
#include <stdlib.h>

enum {
    VM_BYTES = 8u << 20,
    HLE_BASE = 0x400000,
    CTX_EA   = 0x1000,
    FIFO_EA  = 0x100000,
    HANDLER  = 0x100,
    USER_ARG = 0x009B52FC,
};

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
static unsigned handler_calls;
static uint32_t handler_arg;
static unsigned lle_events;

void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; lle_events++; }
void ppu_dump_guest_stack(ppu_context* c, const char* t) { (void)c; (void)t; }
void ppu_dump_bctrl_ring(uint32_t id, const char* t) { (void)id; (void)t; }
int rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 m, u32 a) { (void)m; (void)a; }
void rsx_draw_engine_note_flip(u32 id) { (void)id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int rsx_draw_engine_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h;
    return 0;
}
int rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 m, u32 a) { (void)m; (void)a; }
int rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h;
    return 0;
}
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                               u32 x, u32 y, u32 w, u32 z)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    (void)x; (void)y; (void)w; (void)z;
    return 0;
}
void rsx_state_init(rsx_state* s) { (void)s; }
int rsx_process_method(rsx_state* s, u32 m, u32 d) { (void)s; (void)m; (void)d; return 0; }

static void guest_call(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    if (opd != HANDLER) return;
    handler_calls++;
    handler_arg = (uint32_t)a;
}

static void run_fifo_with_user_command(void)
{
    s_fifo_getoff = 0;
    vm_write32(FIFO_EA + 0, 0x0004EB00u);
    vm_write32(FIFO_EA + 4, USER_ARG);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 8);
    gcm_rsx_process_fifo_unlocked();
    assert(s_fifo_getoff == 8);
}

static void hle_user_command_reaches_the_title_handler(void)
{
    s_gcm_syscall_mode = 0;
    handler_calls = 0; lle_events = 0;
    run_fifo_with_user_command();
    assert(lle_events == 0);
    ppu_gcm_pump();
    assert(handler_calls == 1 && handler_arg == USER_ARG);
}

static void syscall_mode_raises_the_lv2_event(void)
{
    s_gcm_syscall_mode = 1;
    handler_calls = 0; lle_events = 0;
    run_fifo_with_user_command();
    ppu_gcm_pump();
    assert(lle_events == 1 && handler_calls == 0);
    s_gcm_syscall_mode = 0;
}

int main(void)
{
    vm_base = calloc(1, VM_BYTES);
    assert(vm_base);
    ppu_vm_size = VM_BYTES;
    ppu_hle_inject_base = HLE_BASE;
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    populate_offset_table(FIFO_EA, 0, 0x100000);
    s_gcm_context_ea = CTX_EA;
    vm_write32(CTX_EA + 0, FIFO_EA);
    vm_write32(CTX_EA + 4, FIFO_EA + 0xFFFFC);
    vm_write32(CTX_EA + 8, FIFO_EA + 8);
    g_ps3_guest_caller = guest_call;
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)HANDLER);
    hle_user_command_reaches_the_title_handler();
    syscall_mode_raises_the_lv2_event();
    puts("GCM FIFO user command checks passed");
    return 0;
}
