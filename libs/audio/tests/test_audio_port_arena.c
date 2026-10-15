#include "../cellAudio.c"
#include <assert.h>
#include <stdlib.h>

#define ARENA 0xE0000000ull

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static s32 open_port(void)
{
    vm_write64(0x100, CELL_AUDIO_PORT_8CH);
    vm_write64(0x108, CELL_AUDIO_BLOCK_8);
    vm_write64(0x110, 0);
    vm_write32(0x200, 0xFFFFFFFFu);
    return cellAudioPortOpen((void*)0x100, (void*)0x200);
}

int main(void)
{
    uint8_t* raw = mmap(NULL, ARENA + 0x2000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(raw != MAP_FAILED);
    mutex_init(&s_audio_mutex);
    s_audio_initialized = 1;
    ppu_vm_size = (uint32_t)ARENA;

    vm_base = raw + 16;
    assert(open_port() == (s32)CELL_AUDIO_ERROR_AUDIOSYSTEM);
    assert(!s_ports[0].in_use);

    vm_base = raw;
    assert(open_port() == CELL_OK);
    assert(vm_read32(0x200) == 0);
    assert(cellAudioGetPortConfig(0, (void*)0x300) == CELL_OK);
    assert(vm_read32(0x300) == 0x58100000u);
    assert(vm_read32(0x304) == CELL_AUDIO_STATUS_READY);
    assert(vm_read32(0x31C) == 0x58000000u);
    assert(cellAudioPortStart(0) == CELL_OK);
    assert(cellAudioGetPortConfig(0, (void*)0x300) == CELL_OK);
    assert(vm_read32(0x304) == CELL_AUDIO_STATUS_RUN);

    munmap(raw, ARENA + 0x2000);
    puts("audio port arena: unaligned arena refused, page-aligned arena opens and starts");
    return 0;
}
