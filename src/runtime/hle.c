/* hle.c - game/SDK functions replaced by the runtime (tools/recomp/recomp.py,
 * table HLE): what only makes sense with a real ARM7 or banked registers. */
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include "rc_decls.h"

/* PXI_IsCallbackReady(tag, proc): the ARM9 spins until the other CPU has
 * registered its service for a tag. The ARM7 here serves every tag. */
void hle_FUN_0207CA48(void) {
    u32 tag = R(0), proc = R(1);
    if (proc == 1) {
        R(0) = 1;
        return;
    }
    u32 bits = *(volatile u32 *)(uintptr_t)(0x027FFF88 + 4 * proc);
    R(0) = (bits >> tag) & 1;
}

void hle_FUN_01FF8CE0(void) {
    /* the IRQ handler (FUN_01FF8C88) pushed its lr; the original pops it
     * with its final "ldmia sp!, {pc}" */
    R(13) += 4;
    thread_switch_context(R(0), R(1));
}

void hle_FUN_0207ABBC(void) {
    thread_load_context(R(0));
}

/* FUN_0203CB94(file, line, fmt, ...): the game's assertion; on the DS it
 * shows an error screen and stops. Here: the message in the log, then stop. */
void hle_FUN_0203CB94(void) {
    char fmt[200], msg[400];
    u32 f = R(2);
    int i = 0;
    for (; i < (int)sizeof fmt - 1 && f && rd8(f + (u32)i); i++) {
        fmt[i] = (char)rd8(f + (u32)i);
    }
    fmt[i] = 0;
    u32 a[4] = { R(3), rd32(R(13)), rd32(R(13) + 4), rd32(R(13) + 8) };
    snprintf(msg, sizeof msg, fmt, a[0], a[1], a[2], a[3]);
    rt_log("game assertion (lr 0x%08X, r4 %08X r5 %08X r6 %08X r7 %08X): %s", LR, R(4), R(5), R(6), R(7), msg);
    for (u32 k = 0; k < 0x60; k += 16) {
        rt_log("  r5+%02X: %08X %08X %08X %08X", k, rd32(R(5) + k), rd32(R(5) + k + 4), rd32(R(5) + k + 8), rd32(R(5) + k + 12));
    }
    rt_fatal("stopped at the game's assertion");
}

/* OS_Terminate: the game stops for good (on the DS: halt with interrupts
 * off). Logged with the scheduler state, then halted. */
void hle_OS_Terminate(void) {
    void thread_dump(void);
    rt_log("OS_Terminate (lr 0x%08X, cpsr %08X)", LR, cpsr_read());
    rt_log("  r4-r7 %08X %08X %08X %08X (heap alloc failure: r6 heap, r5 size)", R(4), R(5), R(6), R(7));
    thread_dump();
    void heapwatch_dump(void);
    heapwatch_dump();
    for (;;) {
        time_wait_irq();
    }
}

/* the game starts an overlay (FUN_0207113C, called once it is in memory):
 * PB2_LOGOVERLAYS=1 logs it (which code is where, for "call to ...: no
 * loaded overlay" or a wrong function picked by call_addr) */
void hook_overlay_start(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("PB2_LOGOVERLAYS") != NULL;
    }
    if (on) {
        rt_log("overlay %u started (%08X-%08X)", rd32(R(0)), rd32(R(0) + 4), rd32(R(0) + 4) + rd32(R(0) + 8));
    }
}
