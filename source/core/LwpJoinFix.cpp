/*
 * LwpJoinFix.cpp — work around a use-after-free in libogc 3.1's
 * LWP_JoinThread (linked in with -Wl,--wrap=LWP_JoinThread, so libogc's own
 * callers such as MP3Player_Stop and libmplayer's threads get it too).
 *
 * libogc saves the floating-point registers lazily: SPRG3 points to the
 * context of the thread that last used the FPU, and the FPU-unavailable
 * exception stores ~0x200 bytes of FPR/PS/FPSCR into that context when
 * another thread needs the FPU.  KThreadExit does not clear SPRG3, and the
 * stock LWP_JoinThread frees the KThread right after joining.  If the thread
 * that just ended owned the FPU, the next floating-point instruction anywhere
 * writes into the freed block and corrupts the heap (seen as a crash in
 * free() writing to 0x0000000c, e.g. after opening the HOME menu twice).
 *
 * The wrapper does what the stock version does, but drops the dead thread's
 * FPU ownership before freeing it.  Its FPU state is not needed any more.
 */
#include <gccore.h>
#include <stdlib.h>
#include <tuxedo/thread.h>
#include <ogc/machine/processor.h>

extern "C" s32 __wrap_LWP_JoinThread(lwp_t thethread, void** value_ptr)
{
    if (!thethread || thethread == LWP_THREAD_NULL)
        return -1;

    KThread* t = (KThread*)~thethread;     /* same encoding as libogc's lwpcompat */
    sptr rc = KThreadJoin(t);
    if (value_ptr) *value_ptr = (void*)rc;

    u32 level;
    _CPU_ISR_Disable(level);
    u32 fpuOwner;
    asm volatile("mfsprg3 %0" : "=r"(fpuOwner));
    if (fpuOwner == (u32)&t->ctx)
        asm volatile("mtsprg3 %0" : : "r"(0));
    _CPU_ISR_Restore(level);

    free(t);
    return 0;
}
