#pragma once
/*
 * DEAD_AT_EXIT — for big static buffers that hold nothing once WiiFin
 * starts shutting down: stacks of threads that are stopped by then, the
 * start-up GX FIFO, the HTTP exchange buffer.
 *
 * When WiiFin returns to the Wii Menu, App copies the NAND-loader stub back
 * to 0x80804000-0x80831E40, overwriting whatever lives there.  The linker
 * script (tools/wii_wiifin.ld) puts these buffers first in BSS, right under
 * that zone, and starts every other BSS object past its end, so the copy
 * can only land on memory nobody uses any more.
 */
#define DEAD_AT_EXIT __attribute__((section(".bss.dead_at_exit")))
