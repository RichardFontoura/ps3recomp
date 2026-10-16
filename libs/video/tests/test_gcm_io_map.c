#include "../cellGcmSys.c"
#include <assert.h>
#include <stdlib.h>

typedef struct io_op {
    char kind;
    u32 ea, io, size;
} io_op;

enum {
    VM_BYTES = 8u << 20,
    HLE_BASE = 0x400000,
    OUT_EA   = 0x1000,
};

static const io_op k_gowa_sequence[] = {
    {'U', 0, 0x00000000, 0},
    {'E', 0x11000000, 0x00000000, 0x100000},
    {'E', 0x11100000, 0x00100000, 0x5100000},
    {'M', 0x16200000, 0, 0x2000000},
    {'M', 0x18200000, 0, 0x5000000},
    {'u', 0x16200000, 0, 0},
    {'u', 0x18200000, 0, 0},
    {'M', 0x16200000, 0, 0x100000},
    {'M', 0x16300000, 0, 0xC00000},
    {'M', 0x16F00000, 0, 0xA00000},
    {'u', 0x16F00000, 0, 0},
    {'u', 0x16200000, 0, 0},
    {'u', 0x16300000, 0, 0},
    {'M', 0x16200000, 0, 0x100000},
    {'M', 0x16300000, 0, 0xE00000},
    {'M', 0x17100000, 0, 0x900000},
    {'M', 0x17A00000, 0, 0x700000},
    {'M', 0x18100000, 0, 0x300000},
    {'M', 0x18400000, 0, 0xC00000},
    {'M', 0x19000000, 0, 0x300000},
    {'M', 0x19300000, 0, 0x200000},
    {'M', 0x19500000, 0, 0x500000},
    {'M', 0x19A00000, 0, 0x200000},
    {'M', 0x19C00000, 0, 0x500000},
    {'M', 0x1A100000, 0, 0x300000},
    {'M', 0x1A400000, 0, 0xA00000},
    {'u', 0x1A400000, 0, 0},
    {'M', 0x1A400000, 0, 0x1A00000},
    {'M', 0x1BE00000, 0, 0x400000},
    {'M', 0x1C200000, 0, 0x200000},
};

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;

void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static void reset_io(u32 ea, u32 size)
{
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_io_mapping_count = 0;
    s_io_map_reserved = 0;
    s_config.ioAddress = ea;
    s_config.ioSize = size;
    populate_offset_table(ea, 0, size);
    s_io_mappings[0].ea = ea;
    s_io_mappings[0].io = 0;
    s_io_mappings[0].size = size;
    s_io_mappings[0].active = 1;
    s_io_mapping_count = 1;
}

static u32 io_of(u32 ea)
{
    IoMapping* m = find_mapping_by_ea(ea);
    assert(m);
    return m->io;
}

static void check_tables_agree(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        const IoMapping* m = &s_io_mappings[i];
        if (!m->active) continue;
        assert(m->io + m->size <= GCM_IO_SPACE_BYTES);
        for (u32 p = 0; p < (m->size >> 20); p++) {
            assert(s_ea_address_table[(m->io >> 20) + p] == (m->ea >> 20) + p);
            assert(s_io_address_table[(m->ea >> 20) + p] == (m->io >> 20) + p);
        }
    }
}

static void gowa_streaming_sequence_keeps_mapping(void)
{
    reset_io(0x11000000, 0x5200000);
    for (size_t i = 0; i < sizeof(k_gowa_sequence) / sizeof(k_gowa_sequence[0]); i++) {
        const io_op* op = &k_gowa_sequence[i];
        if (op->kind == 'U') {
            assert(cellGcmUnmapIoAddress(op->io) == CELL_OK);
        } else if (op->kind == 'u') {
            assert(cellGcmUnmapIoAddress(io_of(op->ea)) == CELL_OK);
        } else if (op->kind == 'E') {
            assert(cellGcmMapEaIoAddress(op->ea, op->io, op->size) == CELL_OK);
        } else {
            vm_write32(OUT_EA, 0xDEADBEEF);
            assert(cellGcmMapMainMemory(op->ea, op->size, (u32*)(uintptr_t)OUT_EA) == CELL_OK);
            assert(vm_read32(OUT_EA) == io_of(op->ea));
        }
        check_tables_agree();
    }
    assert(gcm_io2ea(io_of(0x1C200000)) == 0x1C200000);
    assert(gcm_io2ea(io_of(0x1C200000) + 0x1FFFFC) == 0x1C3FFFFC);
}

static void freed_io_is_reused_first_fit(void)
{
    reset_io(0x11000000, 0x100000);
    assert(cellGcmMapMainMemory(0x20000000, 0x200000, (u32*)(uintptr_t)OUT_EA) == CELL_OK);
    assert(vm_read32(OUT_EA) == 0x100000);
    assert(cellGcmMapMainMemory(0x21000000, 0x100000, (u32*)(uintptr_t)OUT_EA) == CELL_OK);
    assert(vm_read32(OUT_EA) == 0x300000);
    assert(cellGcmUnmapIoAddress(0x100000) == CELL_OK);
    assert(cellGcmMapMainMemory(0x22000000, 0x100000, (u32*)(uintptr_t)OUT_EA) == CELL_OK);
    assert(vm_read32(OUT_EA) == 0x100000);
    assert(cellGcmMapMainMemory(0x23000000, 0x200000, (u32*)(uintptr_t)OUT_EA) == CELL_OK);
    assert(vm_read32(OUT_EA) == 0x400000);
    assert(gcm_io2ea(0) == 0x11000000);
    check_tables_agree();
}

static void full_io_space_is_refused(void)
{
    reset_io(0x11000000, 0x100000);
    assert(cellGcmMapMainMemory(0x30000000, GCM_IO_SPACE_BYTES - 0x100000, (u32*)(uintptr_t)OUT_EA) == CELL_OK);
    assert(cellGcmMapMainMemory(0x50000000, 0x100000, (u32*)(uintptr_t)OUT_EA) == CELL_GCM_ERROR_NO_IO_PAGE_TABLE);
    check_tables_agree();
}

int main(void)
{
    vm_base = calloc(1, VM_BYTES);
    assert(vm_base);
    ppu_vm_size = VM_BYTES;
    ppu_hle_inject_base = HLE_BASE;
    gowa_streaming_sequence_keeps_mapping();
    freed_io_is_reused_first_fit();
    full_io_space_is_refused();
    puts("GCM IO mapping checks passed");
    return 0;
}
