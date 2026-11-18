/*
 * usbdfc: XIAO RP2350, MMC5983MA magnetometer streamed to a downstream
 * USB CDC device through a PIO-USB host port.
 *
 * Shell only. Planned structure (docs/REQUIREMENTS.md):
 *   core 0: magnetometer tick, DMA transactions, epoch loop that produces
 *           one record per output period into an inter-core ring;
 *   core 1: dedicated PIO-USB host; SOF locked to the magnetometer tick grid;
 *           one bulk OUT record per output period.
 */
#include "pico/stdlib.h"

int main(void) {
    for (;;) {
        tight_loop_contents();
    }
}
