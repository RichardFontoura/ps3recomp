#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../lv2_syscall_table.h"
#include "../sys_timer.h"

#define GUEST_SIZE 0x00100000u
#define TZ_ADDR 0x1000u
#define DST_ADDR 0x1004u
#define STAT_ADDR 0x1010u
#define SENTINEL 0xA5A5A5A5u
#define SPU_THREAD_GROUP_LOG_ON 0
#define SPU_THREAD_GROUP_LOG_OFF 1
#define SPU_THREAD_GROUP_LOG_GET_STATUS 2

int lv2_try_syscall(ppu_context* ctx);
extern lv2_syscall_table g_lv2_syscalls;
uint8_t* vm_base;

static uint8_t g_guest_mem[GUEST_SIZE];
static int g_pass, g_fail;

static void check(int ok, const char* what)
{
    if (ok) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

#define CHECK(cond) check((cond), #cond)

static uint32_t guest_be32(uint32_t addr)
{
    const uint8_t* p = g_guest_mem + addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void guest_fill(uint32_t addr, uint32_t value)
{
    uint8_t* p = g_guest_mem + addr;
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int64_t call(uint32_t num, uint64_t r3, uint64_t r4)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[11] = num;
    ctx.gpr[3] = r3;
    ctx.gpr[4] = r4;
    CHECK(lv2_try_syscall(&ctx) == 1);
    return (int64_t)ctx.gpr[3];
}

static void use_zone(const char* tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static int64_t utc(int year, int month, int day)
{
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    return (int64_t)timegm(&t);
}

static void test_timezone_at(void)
{
    int32_t zone, summer;

    use_zone("America/Sao_Paulo");
    lv2_host_timezone_at(utc(2026, 10, 15), &zone, &summer);
    CHECK(zone == -180 && summer == 0);

    use_zone("Europe/Berlin");
    lv2_host_timezone_at(utc(2026, 1, 15), &zone, &summer);
    CHECK(zone == 60 && summer == 0);
    lv2_host_timezone_at(utc(2026, 7, 15), &zone, &summer);
    CHECK(zone == 60 && summer == 60);

    use_zone("Australia/Sydney");
    lv2_host_timezone_at(utc(2026, 1, 15), &zone, &summer);
    CHECK(zone == 600 && summer == 60);
    lv2_host_timezone_at(utc(2026, 7, 15), &zone, &summer);
    CHECK(zone == 600 && summer == 0);

    use_zone("Asia/Kolkata");
    lv2_host_timezone_at(utc(2026, 7, 15), &zone, &summer);
    CHECK(zone == 330 && summer == 0);
}

static void test_timezone_syscall(void)
{
    int32_t zone, summer;

    use_zone("America/Sao_Paulo");
    lv2_host_timezone_at((int64_t)time(NULL), &zone, &summer);
    guest_fill(TZ_ADDR, SENTINEL);
    guest_fill(DST_ADDR, SENTINEL);
    CHECK(call(SYS_TIME_GET_TIMEZONE, TZ_ADDR, DST_ADDR) == CELL_OK);
    CHECK((int32_t)guest_be32(TZ_ADDR) == zone);
    CHECK((int32_t)guest_be32(DST_ADDR) == summer);
    CHECK((int32_t)guest_be32(TZ_ADDR) == -180);

    guest_fill(TZ_ADDR, SENTINEL);
    guest_fill(DST_ADDR, SENTINEL);
    CHECK(call(SYS_TIME_GET_TIMEZONE, 0, DST_ADDR) == (int64_t)(int32_t)CELL_EFAULT);
    CHECK(call(SYS_TIME_GET_TIMEZONE, TZ_ADDR, 0) == (int64_t)(int32_t)CELL_EFAULT);
    CHECK(guest_be32(TZ_ADDR) == SENTINEL && guest_be32(DST_ADDR) == SENTINEL);
}

static void test_group_log(void)
{
    guest_fill(STAT_ADDR, SENTINEL);
    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, SPU_THREAD_GROUP_LOG_GET_STATUS, STAT_ADDR) == CELL_OK);
    CHECK(guest_be32(STAT_ADDR) == SPU_THREAD_GROUP_LOG_ON);

    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, SPU_THREAD_GROUP_LOG_OFF, 0) == CELL_OK);
    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, SPU_THREAD_GROUP_LOG_GET_STATUS, STAT_ADDR) == CELL_OK);
    CHECK(guest_be32(STAT_ADDR) == SPU_THREAD_GROUP_LOG_OFF);

    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, SPU_THREAD_GROUP_LOG_ON, 0) == CELL_OK);
    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, SPU_THREAD_GROUP_LOG_GET_STATUS, STAT_ADDR) == CELL_OK);
    CHECK(guest_be32(STAT_ADDR) == SPU_THREAD_GROUP_LOG_ON);

    guest_fill(STAT_ADDR, SENTINEL);
    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, SPU_THREAD_GROUP_LOG_GET_STATUS, 0) == (int64_t)(int32_t)CELL_EFAULT);
    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, 3, STAT_ADDR) == (int64_t)(int32_t)CELL_EINVAL);
    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, -1, STAT_ADDR) == (int64_t)(int32_t)CELL_EINVAL);
    CHECK(guest_be32(STAT_ADDR) == SENTINEL);
    CHECK(call(SYS_SPU_THREAD_GROUP_LOG, SPU_THREAD_GROUP_LOG_GET_STATUS, STAT_ADDR) == CELL_OK);
    CHECK(guest_be32(STAT_ADDR) == SPU_THREAD_GROUP_LOG_ON);
}

int main(void)
{
    vm_base = g_guest_mem;
    lv2_register_all_syscalls(&g_lv2_syscalls);
    test_timezone_at();
    test_timezone_syscall();
    test_group_log();
    printf("lv2 timezone/group_log: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
