/*
 * OgcCompat.c — libogc symbols the prebuilt libraries still link to.
 *
 * libogc 3.2 made udelay() an inline of <ogc/lwp_watchdog.h> (over
 * PPCBusyWaitTicks); libmplayer.a, built against 3.1, calls it as a
 * function (osdep/ave-rvl.c).  Only <tuxedo/ppc/clock.h> here: the inline
 * of the same name stays out of this file.  libogc 3.1 has no tuxedo and
 * its own udelay(): nothing here then.
 */
#if __has_include(<tuxedo/ppc/clock.h>)
#include <tuxedo/ppc/clock.h>

void udelay(unsigned us)
{
	PPCBusyWaitTicks(PPCUsToTicks(us));
}

/* libogc 3.2 turns on timeslicing as the first thread is made (a tick task
 * switching threads of the same priority every 10 ms); WiiFin's threads are
 * written for 3.1's scheduling, a thread running until it waits.  Kept off:
 * the weak SYS_SetTimeslice() replaced by nothing */
void SYS_SetTimeslice(unsigned ticks)
{
	(void)ticks;
}
#endif

/* libogc 3.2: an IOS reply's cache invalidation can undo the conversion back
 * of a pointer sharing a cache line with the buffer invalidated (a USB
 * message's small buffers: the Wii Remotes' Bluetooth goes through USB), and
 * __usbv0_messageCB then frees a buffer at its physical address: a DSI in
 * __lwp_heap_free as a Wii Remote is turned on (a tester's Wii).  Freed at
 * its cached address instead (-Wl,--wrap=iosFree) */
void __real_iosFree(int hid, void* ptr);
void __wrap_iosFree(int hid, void* ptr)
{
	if (ptr && ((unsigned)ptr & 0xE0000000) == 0) ptr = (void*)((unsigned)ptr | 0x80000000);
	__real_iosFree(hid, ptr);
}
