/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/syscall.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static volatile long g_lockline = 0;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
void spu_lockline_lock(void)
{
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
static int s_coh_armed;

#define SPU_COH_MAX_CTX 256
static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];
static int          s_coh_n;

unsigned long g_spu_lr_raise = 0;

uint32_t g_spu_line_trace_ea;
spu_line_trace_entry g_spu_line_trace[SPU_LINE_TRACE_RING];
volatile uint32_t g_spu_line_trace_seq;
uint32_t g_spu_line_trace_n;
uint32_t g_spu_line_trace_eas[32];
uint32_t g_spu_wws_check_q;
static uint32_t s_wws_obj_ptr;
static volatile char s_trace_busy;

typedef struct wws_claim {
    uint64_t ns, cmd_mem, cmd_ls;
    uint32_t trace_seq, tid, pc, spu_num, row, idx, next_old, next_new, blk;
    uint32_t key_w2, key_w3, cur_w2, cur_w3, ppu_cnt, cc, d0, d4, d8, flags;
    const void* ctx;
} wws_claim;
static wws_claim s_wws_ring[1024];
static uint32_t s_wws_n, s_wws_viol;

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    { static int inited; if (!inited) { inited = 1; spu_line_trace_init(); } }
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    s_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
    s_coh_armed = 1;

    if (!ctx) return;
    int free_i = -1;
    for (int i = 0; i < s_coh_n; i++) {
        if (s_coh_ctxs[i] == ctx) return;
        if (s_coh_ctxs[i] == NULL && free_i < 0) free_i = i;
    }
    if (free_i < 0 && s_coh_n < SPU_COH_MAX_CTX) free_i = s_coh_n;
    if (free_i >= 0) {
        s_coh_ctxs[free_i] = ctx;
        if (free_i >= s_coh_n) s_coh_n = free_i + 1;
        return;
    }
    { static int warned = 0;
      if (!warned) { warned = 1;
          fprintf(stderr, "[spu-coh] more than %d reserving SPU contexts -- "
                          "spu=0x%X will not receive lock-line events\n",
                  SPU_COH_MAX_CTX, ctx->spu_id);
          fflush(stderr); } }
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before the NULL store would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (int i = 0; i < s_coh_n; i++)
        if (s_coh_ctxs[i] == ctx) { s_coh_ctxs[i] = NULL; break; }
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (int i = 0; i < s_coh_n; i++) {
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (_n++ < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            s_coh_ctxs[i] = NULL;
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < s_coh_n; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < s_coh_n; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

void spu_line_trace_init(void)
{
    const char* e = getenv("SPU_LINE_TRACE");
    g_spu_line_trace_n = 0;
    while (e && *e && g_spu_line_trace_n < 32u) {
        char* end;
        const uint32_t v = (uint32_t)strtoul(e, &end, 16) & ~127u;
        if (end == e) break;
        g_spu_line_trace_eas[g_spu_line_trace_n++] = v;
        const uint32_t line = v >> SPU_COH_LINE_SHIFT;
        s_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
        s_coh_armed = 1;
        e = *end == ',' ? end + 1 : end;
    }
    g_spu_line_trace_ea = g_spu_line_trace_n ? g_spu_line_trace_eas[0] : 0u;
    const char* w = getenv("SPU_WWS_CLAIM_CHECK");
    if (w && *w) {
        char* end;
        g_spu_wws_check_q = (uint32_t)strtoul(w, &end, 16) & ~127u;
        s_wws_obj_ptr = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 16) : 0u;
        fprintf(stderr, "[wws-check] queue=0x%08X obj-ptr=0x%08X traced-lines=%u\n",
                g_spu_wws_check_q, s_wws_obj_ptr, g_spu_line_trace_n);
        fflush(stderr);
    }
}

static int spu_line_trace_index(uint32_t ea, uint32_t size)
{
    for (uint32_t i = 0; i < g_spu_line_trace_n; i++) {
        const uint32_t l = g_spu_line_trace_eas[i];
        if (ea < l + 128u && ea + size > l) return (int)i;
    }
    return -1;
}

int spu_line_trace_hit(uint32_t ea, uint32_t size)
{
    return spu_line_trace_index(ea, size) >= 0;
}

static uint32_t spu_line_trace_tid(void)
{
#if defined(_WIN32)
    return (uint32_t)GetCurrentThreadId();
#else
    return (uint32_t)syscall(SYS_gettid);
#endif
}

static uint64_t spu_line_trace_now_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void spu_line_trace_record(uint32_t kind, uint32_t ea, uint32_t size, const void* actor,
                                  uint32_t pc, uint32_t lr, uint32_t spu_num, int li,
                                  const uint8_t* line)
{
    static uint8_t last[32][128];
    static uint8_t have_last[32];
    while (__atomic_test_and_set(&s_trace_busy, __ATOMIC_ACQUIRE)) { }
    if (kind != SPU_LINE_TRACE_PUTLLC_FAIL && kind != SPU_LINE_TRACE_GET && have_last[li] &&
        !memcmp(line, last[li], 128)) {
        __atomic_clear(&s_trace_busy, __ATOMIC_RELEASE);
        return;
    }
    memcpy(last[li], line, 128);
    have_last[li] = 1;
    const uint32_t seq = __atomic_fetch_add(&g_spu_line_trace_seq, 1u, __ATOMIC_RELAXED);
    spu_line_trace_entry* t = &g_spu_line_trace[seq % SPU_LINE_TRACE_RING];
    t->ns = spu_line_trace_now_ns();
    t->seq = seq; t->kind = kind; t->ea = ea; t->size = size;
    t->pc = pc; t->lr = lr; t->spu_num = spu_num;
    t->tid = spu_line_trace_tid();
    t->actor = actor;
    t->line_ea = g_spu_line_trace_eas[li];
    memcpy(t->line, line, 128);
    __atomic_clear(&s_trace_busy, __ATOMIC_RELEASE);
}

void spu_line_trace_note(uint32_t kind, uint32_t ea, uint32_t size, const void* actor,
                         uint32_t pc, uint32_t lr, uint32_t spu_num)
{
    extern uint8_t* vm_base;
    const int li = spu_line_trace_index(ea, size);
    if (li < 0) return;
    spu_line_trace_record(kind, ea, size, actor, pc, lr, spu_num, li,
                          vm_base + g_spu_line_trace_eas[li]);
}

void spu_line_trace_note_data(uint32_t kind, uint32_t ea, uint32_t size, const void* actor,
                              uint32_t pc, uint32_t lr, uint32_t spu_num, const uint8_t* line)
{
    const int li = spu_line_trace_index(ea, size);
    if (li < 0) return;
    spu_line_trace_record(kind, ea, size, actor, pc, lr, spu_num, li, line);
}

static uint32_t wws_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t wws_be64(const uint8_t* p)
{
    return ((uint64_t)wws_be32(p) << 32) | wws_be32(p + 4);
}

static uint32_t wws_guest32(uint32_t ea)
{
    extern uint8_t* vm_base;
    if (ea == 0 || ea >= 0xF0000000u || (ea & 3u)) return 0;
    return __builtin_bswap32(__atomic_load_n((const uint32_t*)(vm_base + ea), __ATOMIC_RELAXED));
}

static uint64_t wws_guest64(uint32_t ea)
{
    extern uint8_t* vm_base;
    if (ea == 0 || ea >= 0xF0000000u || (ea & 7u)) return 0;
    return __builtin_bswap64(__atomic_load_n((const uint64_t*)(vm_base + ea), __ATOMIC_RELAXED));
}

static void wws_print_claim(const char* tag, const wws_claim* c)
{
    fprintf(stderr, "[wws-check] %s tseq=%u ns=%llu tid=%u ctx=%p pc=0x%05X spu=%u row=%u idx=%u"
                    " next=%u->%u mem=%016llX ls=%016llX blk=0x%08X key=%08X/%08X cur=%08X/%08X"
                    " ppu cnt=%d cc=%d d0=%d d4=%d d8=%d flags=0x%X\n",
            tag, c->trace_seq, (unsigned long long)c->ns, c->tid, c->ctx, c->pc, c->spu_num,
            c->row, c->idx, c->next_old, c->next_new, (unsigned long long)c->cmd_mem,
            (unsigned long long)c->cmd_ls, c->blk, c->key_w2, c->key_w3, c->cur_w2, c->cur_w3,
            (int)c->ppu_cnt, (int)c->cc, (int)c->d0, (int)c->d4, (int)c->d8, c->flags);
}

static void wws_print_block_history(uint32_t blk, uint32_t off)
{
    static const char* names[] = {"?", "GETLLAR", "PUTLLC+", "PUTLLC-", "PUT", "PPUCAS", "PPUSTO", "GET"};
    const uint32_t row = off & ~15u;
    while (__atomic_test_and_set(&s_trace_busy, __ATOMIC_ACQUIRE)) { }
    const uint32_t end = g_spu_line_trace_seq;
    uint32_t idx[40], n = 0;
    for (uint32_t k = 1; k <= SPU_LINE_TRACE_RING && k <= end && n < 40u; k++) {
        const spu_line_trace_entry* t = &g_spu_line_trace[(end - k) % SPU_LINE_TRACE_RING];
        if (t->kind && t->seq == end - k && t->line_ea == blk) idx[n++] = (end - k) % SPU_LINE_TRACE_RING;
    }
    for (uint32_t k = n; k > 0; k--) {
        const spu_line_trace_entry* t = &g_spu_line_trace[idx[k - 1]];
        fprintf(stderr, "[wws-check]   hist seq=%u ns=%llu %-7s tid=%u actor=%p pc=0x%05X lr=0x%05X ea=+0x%02X sz=%u row@+%02X=%08X%08X%08X%08X\n",
                t->seq, (unsigned long long)t->ns, names[t->kind < 8 ? t->kind : 0], t->tid, t->actor,
                t->pc, t->lr, t->ea & 127u, t->size, row, wws_be32(t->line + row), wws_be32(t->line + row + 4),
                wws_be32(t->line + row + 8), wws_be32(t->line + row + 12));
    }
    __atomic_clear(&s_trace_busy, __ATOMIC_RELEASE);
}

void spu_wws_check_claim(const void* actor, uint32_t ea, const uint8_t* old_line,
                         const uint8_t* new_line)
{
    const spu_context* ctx = (const spu_context*)actor;
    const uint32_t q = ea & ~127u;
    uint32_t obj = 0;
    if (s_wws_obj_ptr) {
        obj = wws_guest32(s_wws_obj_ptr);
        if (wws_guest32(obj) != q) obj = 0;
    }
    for (uint32_t r = 0; r < 4; r++) {
        const uint32_t on = ((uint32_t)old_line[0x42 + 16 * r] << 8) | old_line[0x43 + 16 * r];
        const uint32_t nn = ((uint32_t)new_line[0x42 + 16 * r] << 8) | new_line[0x43 + 16 * r];
        if (on == nn) continue;
        const uint32_t list_ea = wws_be32(old_line + 0x30 + 4 * r);
        const uint32_t last = nn > on ? nn : on + 1u;
        for (uint32_t i = on; i < last; i++) {
            wws_claim* c = &s_wws_ring[s_wws_n++ % 1024u];
            const uint32_t cmd_ea = list_ea + 8u * i;
            c->ns = spu_line_trace_now_ns();
            c->trace_seq = g_spu_line_trace_seq;
            c->tid = spu_line_trace_tid();
            c->ctx = actor;
            c->pc = (uint32_t)ctx->pc & SPU_LS_MASK;
            c->spu_num = SPU_LINE_TRACE_SPUNUM(ctx);
            c->row = r; c->idx = i; c->next_old = on; c->next_new = nn;
            c->cmd_mem = wws_guest64(cmd_ea);
            c->cmd_ls = wws_be64(ctx->ls + 0xD00u + (cmd_ea & 127u));
            c->blk = wws_be32(ctx->ls + 0x1500u);
            c->key_w2 = wws_be32(ctx->ls + 0x1518u);
            c->key_w3 = wws_be32(ctx->ls + 0x151Cu);
            c->cur_w2 = wws_be32(old_line + 8);
            c->cur_w3 = wws_be32(old_line + 12);
            c->ppu_cnt = obj ? wws_guest32(obj + 0x2Cu + 0x14u * r) : 0xFFFFFFFFu;
            c->cc = obj ? wws_guest32(obj + 0xCCu) : 0xFFFFFFFFu;
            c->d0 = obj ? wws_guest32(obj + 0xD0u) : 0xFFFFFFFFu;
            c->d4 = obj ? wws_guest32(obj + 0xD4u) : 0xFFFFFFFFu;
            c->d8 = obj ? wws_guest32(obj + 0xD8u) : 0xFFFFFFFFu;
            const uint32_t ls_type = (uint32_t)(c->cmd_ls >> 32) & 0xFFFFu;
            uint32_t f = 0;
            if (nn < on) f |= 0x10u;
            if (ls_type == 0) f |= 0x1u;
            if (c->cmd_ls != c->cmd_mem) f |= 0x2u;
            if (obj && i >= c->ppu_cnt) f |= 0x4u;
            if (c->blk != (cmd_ea & ~127u)) f |= 0x8u;
            if (c->key_w3 != c->cur_w3) f |= 0x20u;
            if ((ls_type == 2 || ls_type == 5) && (uint32_t)c->cmd_ls >= 0xF0000000u) f |= 0x40u;
            if (ls_type > 5) f |= 0x80u;
            c->flags = f;
            if (s_wws_n == 1 || (s_wws_n % 100000u) == 0) {
                wws_print_claim("progress", c);
                fflush(stderr);
            }
            if (!f) continue;
            const uint32_t nv = ++s_wws_viol;
            if (nv > 16 && (nv % 1000u) != 0) continue;
            wws_print_claim("VIOLATION", c);
            fprintf(stderr, "[wws-check]   line old:");
            for (uint32_t b = 0x30; b < 0x80; b += 4) fprintf(stderr, " %08X", wws_be32(old_line + b));
            fprintf(stderr, "\n[wws-check]   line new:");
            for (uint32_t b = 0x30; b < 0x80; b += 4) fprintf(stderr, " %08X", wws_be32(new_line + b));
            fprintf(stderr, "\n[wws-check]   ls  blk:");
            for (uint32_t b = 0; b < 128; b += 8) fprintf(stderr, " %016llX",
                    (unsigned long long)wws_be64(ctx->ls + 0xD00u + b));
            fprintf(stderr, "\n[wws-check]   mem blk:");
            for (uint32_t b = 0; b < 128; b += 8) fprintf(stderr, " %016llX",
                    (unsigned long long)wws_guest64((cmd_ea & ~127u) + b));
            fprintf(stderr, "\n");
            wws_print_block_history(cmd_ea & ~127u, cmd_ea & 127u);
            const uint32_t n = s_wws_n < 48u ? s_wws_n : 48u;
            for (uint32_t k = n; k > 1; k--)
                wws_print_claim("  before", &s_wws_ring[(s_wws_n - k) % 1024u]);
            fflush(stderr);
        }
    }
}

void spu_wws_check_skip(const void* actor, uint32_t lsa, uint32_t ea, uint32_t size)
{
    const spu_context* ctx = (const spu_context*)actor;
    static uint32_t n;
    const uint32_t k = __atomic_add_fetch(&n, 1u, __ATOMIC_RELAXED);
    if (k > 200 && (k % 1000u) != 0) return;
    const uint32_t q = ctx->gpr[52]._u32[0];
    fprintf(stderr, "[wws-skip] n=%u tid=%u ctx=%p pc=0x%05X lsa=0x%05X ea=0x%08X size=%u"
                    " r3=%08X %08X r52=%08X r59=%08X r63=%08X blk=%08X key=%08X/%08X"
                    " q.hdr=%08X/%08X tseq=%u\n",
            k, spu_line_trace_tid(), actor, (uint32_t)ctx->pc & SPU_LS_MASK, lsa, ea, size,
            ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1], q, ctx->gpr[59]._u32[0],
            ctx->gpr[63]._u32[0], wws_be32(ctx->ls + 0x1500u), wws_be32(ctx->ls + 0x1518u),
            wws_be32(ctx->ls + 0x151Cu), wws_guest32((q & ~127u) + 8u),
            wws_guest32((q & ~127u) + 12u), g_spu_line_trace_seq);
    fflush(stderr);
}

#ifdef __cplusplus
}
#endif
