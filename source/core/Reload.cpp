/* Reload.cpp — WiiFin started again from its own DOL (the HOME menu's Reset
 * when a loader such as the Homebrew Channel launched it: no channel to
 * launch again, and exit() only goes back to the loader).
 *
 * The DOL is read whole into the heap (above the program's sections, which
 * the new one, the same file, takes again), then a small routine copied
 * there too, out of the way, puts each section where it goes, clears the
 * BSS, flushes the caches, hands the DOL its path (libogc's "_arg" block
 * by its entry: argv[0], for its folder) and jumps to its entry.  It
 * works in registers only: the stack is in the BSS it clears. */
#include "Reload.h"
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

/* r3: the DOL in memory; r4: the six words of its "_arg" block, or 0 */
extern "C" void reloadTrampoline(const void* dol, const void* argv);
extern "C" char reloadTrampolineEnd[];

asm(R"(
    .section .text
    .balign 32
    .globl reloadTrampoline
    .globl reloadTrampolineEnd
reloadTrampoline:
    mfmsr   5
    rlwinm  5, 5, 0, 17, 15         # interrupts off
    mtmsr   5
    li      6, 0                    # the 18 sections (7 text, 11 data)
1:  lwzx    7, 3, 6                 # its offset in the file
    addi    8, 6, 0x48
    lwzx    8, 3, 8                 # its address
    addi    9, 6, 0x90
    lwzx    9, 3, 9                 # its size
    cmpwi   9, 0
    beq     4f
    add     7, 7, 3
    mr      10, 8
    mr      11, 9
2:  lwz     12, 0(7)
    stw     12, 0(10)
    addi    7, 7, 4
    addi    10, 10, 4
    addic.  11, 11, -4
    bgt     2b
    mr      10, 8
    add     11, 8, 9
3:  dcbf    0, 10                   # to memory, out of the instruction cache
    icbi    0, 10
    addi    10, 10, 32
    cmplw   10, 11
    blt     3b
4:  addi    6, 6, 4
    cmpwi   6, 72
    blt     1b
    lwz     8, 0xD8(3)              # the BSS, cleared
    lwz     9, 0xDC(3)
    cmpwi   9, 0
    beq     6f
    li      12, 0
    mr      10, 8
    mr      11, 9
5:  stw     12, 0(10)
    addi    10, 10, 4
    addic.  11, 11, -4
    bgt     5b
    mr      10, 8
    add     11, 8, 9
55: dcbf    0, 10
    addi    10, 10, 32
    cmplw   10, 11
    blt     55b
6:  sync
    isync
    lwz     8, 0xE0(3)              # its entry
    cmpwi   4, 0
    beq     8f
    lis     10, 0x5f61              # "_arg" after its first branch (or its
    ori     10, 10, 0x7267          # second: WiiFin's crt0_pre), its block after
    addi    5, 8, 4
    lwz     9, 0(5)
    cmpw    9, 10
    beq     65f
    addi    5, 8, 8
    lwz     9, 0(5)
    cmpw    9, 10
    bne     8f
65: addi    5, 5, 4
    li      6, 0
7:  lwzx    9, 4, 6
    stwx    9, 5, 6
    addi    6, 6, 4
    cmpwi   6, 24
    blt     7b
    dcbf    0, 5
    addi    10, 5, 24
    dcbf    0, 10
    sync
    icbi    0, 5
    isync
8:  mtctr   8
    bctr
reloadTrampolineEnd:
    .previous
)");

namespace {
/* argv[0], where the new program finds it: high in MEM2, out of reach of its
 * first allocations */
char* const ARG_AREA = (char*)0x93300000;
}

bool Reload::self(const char* path)
{
    if (!path || !path[0]) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x100) { fclose(f); return false; }
    u8* dol = (u8*)memalign(32, (size + 31) & ~31);
    if (!dol) { fclose(f); return false; }
    const bool read = fread(dol, 1, size, f) == (size_t)size;
    fclose(f);
    const u32 entry = *(u32*)(dol + 0xE0);
    if (!read || (entry & 0xF0000000) != 0x80000000) { free(dol); return false; }
    DCFlushRange(dol, (size + 31) & ~31);

    /* the routine, copied out of the sections about to be overwritten */
    const size_t len = (size_t)(reloadTrampolineEnd - (char*)reloadTrampoline);
    void* code = memalign(32, (len + 31) & ~31);
    if (!code) { free(dol); return false; }
    memcpy(code, (const void*)reloadTrampoline, len);
    DCFlushRange(code, (len + 31) & ~31);
    ICInvalidateRange(code, (len + 31) & ~31);

    /* its "_arg" block: magic, command line, its length, argc, argv, end */
    const size_t pl = strlen(path) + 1;
    memcpy(ARG_AREA + 64, path, pl);
    u32* arg = (u32*)ARG_AREA;
    arg[0] = 0x5f617267;
    arg[1] = (u32)(ARG_AREA + 64);
    arg[2] = (u32)pl;
    arg[3] = 0;
    arg[4] = 0;
    arg[5] = 0;
    DCFlushRange(ARG_AREA, 64 + pl);

    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
    u32 level;
    _CPU_ISR_Disable(level);
    (void)level;
    ((void (*)(const void*, const void*))code)(dol, arg);
    return false;   /* never back */
}
