#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

#define TASKSET_A  0x00030000u
#define TASKSET_B  0x00032000u
#define ELF_EA     0x00040000u
#define LSA        0x00001000u
#define READY      0x10u
#define PENDING    0x20u
#define ENABLED    0x30u
#define TASK       2u

void spu_taskset_track(uint32_t taskset_ea);

static int g_pass, g_fail;
static const char* g_current = "(none)";
static spu_context g_spu;

#define TEST(name) g_current = (name)
#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void be16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void write_minimal_spu_elf(void)
{
    uint8_t* e = &g_guest_mem[ELF_EA];
    memset(e, 0, 0x200);
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 2; e[6] = 1;
    be16(e + 0x10, 2);
    be16(e + 0x12, 0x17);
    be32(e + 0x1C, 0x34);
    be16(e + 0x2A, 0x20);
    be16(e + 0x2C, 1);
    be32(e + 0x34, 1);
    be32(e + 0x38, 0x100);
    be32(e + 0x3C, 0x3000);
    be32(e + 0x44, 0x80);
    be32(e + 0x48, 0x80);
}

static void wrch(spu_context* ctx, uint32_t channel, uint32_t v)
{
    spu_wrch(ctx, channel, spu_make_preferred_u32(v));
}

static void spu_atomic(uint32_t ea, uint32_t cmd)
{
    wrch(&g_spu, MFC_LSA, LSA);
    wrch(&g_spu, MFC_EAH, 0);
    wrch(&g_spu, MFC_EAL, ea);
    wrch(&g_spu, MFC_Cmd, cmd);
}

static void spu_create_task(uint32_t taskset, uint32_t task, uint32_t elf_field)
{
    be32(&g_guest_mem[taskset + 0x80 + 0x30 * task + 0x14], elf_field);
    spu_atomic(taskset, MFC_GETLLAR_CMD);
    g_spu.ls[LSA + ENABLED + task / 8u] |= (uint8_t)(0x80u >> (task % 8u));
    g_spu.ls[LSA + PENDING + task / 8u] |= (uint8_t)(0x80u >> (task % 8u));
    spu_atomic(taskset, MFC_PUTLLC_CMD);
}

static int bit(uint32_t taskset, uint32_t off, uint32_t task)
{
    return (g_guest_mem[taskset + off + task / 8u] >> (7u - task % 8u)) & 1u;
}

static void test_spu_created_task_starts(void)
{
    TEST("a task an SPU enables with PUTLLC is started, ELF flag bits ignored");
    spu_taskset_track(TASKSET_A);
    spu_create_task(TASKSET_A, TASK, ELF_EA | 4u);
    CHECK(g_spu.atomic_stat == 0);
    CHECK(bit(TASKSET_A, ENABLED, TASK));
    CHECK(!bit(TASKSET_A, PENDING, TASK));
}

static void test_started_task_is_not_restarted(void)
{
    TEST("a later commit does not start the same running task again");
    spu_create_task(TASKSET_A, TASK, ELF_EA | 4u);
    CHECK(bit(TASKSET_A, PENDING, TASK));
}

static void test_untracked_taskset_is_left_alone(void)
{
    TEST("a commit to a line that is not a known taskset starts nothing");
    spu_create_task(TASKSET_B, TASK, ELF_EA);
    CHECK(bit(TASKSET_B, PENDING, TASK));
}

static void test_task_without_elf_is_left_alone(void)
{
    TEST("an enabled task with no ELF is not started");
    spu_create_task(TASKSET_A, TASK + 1u, 0);
    CHECK(bit(TASKSET_A, PENDING, TASK + 1u));
}

int main(void)
{
    printf("SPURS task created from an SPU\n");
    spu_context_init(&g_spu, 0);
    write_minimal_spu_elf();
    test_spu_created_task_starts();
    test_started_task_is_not_restarted();
    test_untracked_taskset_is_left_alone();
    test_task_without_elf_is_left_alone();
    printf("SPURS task created from an SPU: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return now_ms(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
