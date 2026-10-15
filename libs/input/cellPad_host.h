/* Optional host adapter. Register before guest threads start; callbacks must
 * be thread-safe and must not pump window events from a guest thread. */
#ifndef PS3RECOMP_CELL_PAD_HOST_H
#define PS3RECOMP_CELL_PAD_HOST_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CellPadHostSample {
    int connected;
    uint16_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
} CellPadHostSample;
typedef void (*CellPadHostRead)(unsigned port, CellPadHostSample* sample);
typedef void (*CellPadHostRumble)(unsigned port, uint8_t small, uint8_t large);
void cellPad_set_host_adapter(CellPadHostRead read, CellPadHostRumble rumble);
#ifdef __cplusplus
}
#endif
#endif
