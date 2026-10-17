#include "../spu_pic_match.h"
#include <stdio.h>
#include <string.h>

enum { LS = 0x40000, CODE = 0x200 };

static uint8_t ls[LS];
static uint8_t code[CODE];
static int failures;

static void check(int cond, const char* what)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static void place(uint32_t at, uint32_t lo, uint32_t hi)
{
    memset(ls, 0xEE, sizeof ls);
    memcpy(ls + at, code + lo, hi - lo);
}

int main(void)
{
    for (uint32_t i = 0; i < CODE; i++) code[i] = (uint8_t)(i * 7 + 3);
    const uint32_t addr = 0x40, lo = 0x20, hi = 0x100;
    const uint32_t ranges[] = { 0x40, 0x80, 0xC0, 0xD0 };
    const uint32_t pc = 0x10040;

    place(pc - (addr - lo), lo, hi);
    check(spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, 0, 0), "hull: identical copy matches");
    ls[pc + 0x90 - addr] ^= 1;
    check(!spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, 0, 0), "hull: any byte differs -> reject");
    check(spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, ranges, 2), "ranges: byte outside ranges ignored");

    place(pc, addr, 0x80);
    memcpy(ls + pc + 0xC0 - addr, code + 0xC0, 0x10);
    check(spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, ranges, 2), "ranges: truncated copy with gap matches");
    check(!spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, 0, 0), "hull: truncated copy rejected");

    ls[pc + 0x44 - addr] ^= 0x80;
    check(!spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, ranges, 2), "ranges: patched word in first range rejected");
    ls[pc + 0x44 - addr] ^= 0x80;
    ls[pc + 0xCF - addr] ^= 0x01;
    check(!spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, ranges, 2), "ranges: last byte of second range checked");
    ls[pc + 0xCF - addr] ^= 0x01;

    const uint32_t before[] = { 0x20, 0x30, 0x40, 0x60 };
    place(pc - (addr - lo), lo, hi);
    check(spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, before, 2), "ranges: range before the entry");
    check(!spu_pic_entry_matches(ls, LS, 0x10, code, addr, lo, hi, before, 2), "ranges: range would start below LS 0");

    const uint32_t tail[] = { 0x40, 0x100 };
    check(!spu_pic_entry_matches(ls, LS, LS - 0x40, code, addr, lo, hi, tail, 1), "ranges: range past LS end rejected");
    const uint32_t bad[] = { 0x40, 0x120 };
    check(!spu_pic_entry_matches(ls, LS, pc, code, addr, lo, hi, bad, 1), "ranges: range outside lo..hi rejected");

    if (failures) return 1;
    printf("SPU PIC match: all checks passed\n");
    return 0;
}
