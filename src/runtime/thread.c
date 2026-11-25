/*
 * thread.c - the game's OS threads on host fibers.
 *
 * OSContext (at offset 0 of OSThread): +0 cpsr, +4..+0x3C r0..r14,
 * +0x40 pc+4, +0x44 svc sp, +0x48 CP context (divider/sqrt state).
 * OSThreadInfo at 0x0214C22C: +0 need-reschedule (u16), +4 current thread,
 * +8 thread list (by priority, OSThread.next at +0x68, state at +0x64:
 * 1 = ready), +0xC switch callback.
 *
 * Each game thread runs on its own fiber (own C stack, own copy of the CPU
 * state). OS_SaveContext binds the context to the running fiber;
 * OS_LoadContext and the reschedule at the end of an interrupt switch to
 * the context's fiber, creating it the first time (the thread then starts at
 * its entry point). A fiber resumed after a save continues as the "returned
 * 1" path, with the registers it had when it saved.
 */
#include "rt.h"
#include "rc_decls.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_THREADS 48
#define INFO pb2_addr(0x0214C22Cu)     /* Black 2 address, moved in White 2 */

typedef struct {
    u32 ctx;
    void *fiber;
    Cpu cpu;
    int in_irq;
    u32 saved_pc;     /* context pc word when the fiber was left */
} Fiber;

static Fiber sFibers[MAX_THREADS];
static int sCount;
static Fiber *sCur;
extern int gInIrq;

static Fiber *fiber_of(u32 ctx, int create);

static void ctx_store(u32 ctx, u32 resume) {
    volatile u32 *c = (volatile u32 *)(uintptr_t)ctx;
    c[0] = cpsr_read();
    for (int i = 0; i < 15; i++) {
        c[1 + i] = R(i);
    }
    c[1] = 1;               /* r0 when resumed */
    c[16] = resume;
}

static void switch_to(Fiber *t) {
    if (t == sCur) {
        return;
    }
    Fiber *from = sCur;
    if (from->ctx) {
        from->saved_pc = ((volatile u32 *)(uintptr_t)from->ctx)[16];
    }
    from->cpu = g;
    from->in_irq = gInIrq;
    sCur = t;
    g = t->cpu;
    gInIrq = t->in_irq;
    SwitchToFiber(t->fiber);
    /* back on "from": its state was restored by whoever switched here */
}

static VOID CALLBACK thread_entry(LPVOID arg) {
    Fiber *self = arg;
    volatile u32 *c = (volatile u32 *)(uintptr_t)self->ctx;
    u32 cpsr = c[0];
    u32 pc = c[16] - 4;
    if (cpsr & 0x20) {
        pc |= 1;
    }
    rt_log("thread 0x%08X starts at 0x%08X (sp 0x%08X)", self->ctx, pc, c[14]);
    u32 lr = c[15];
    call_addr(pc);
    /* the entry function returned: like the DS, continue at its lr (exit) */
    call_addr(lr);
    rt_fatal("thread 0x%08X ended", self->ctx);
}

static Fiber *fiber_of(u32 ctx, int create) {
    Fiber *f = NULL;
    for (int i = 0; i < sCount; i++) {
        if (sFibers[i].ctx == ctx) {
            f = &sFibers[i];
            break;
        }
    }
    if (f) {
        /* OS_CreateThread may reuse the OSThread of a thread that ended (or
         * was destroyed): its context no longer is the one the fiber left,
         * so the thread starts over on a new fiber */
        if (!create || f == sCur || ((volatile u32 *)(uintptr_t)ctx)[16] == f->saved_pc) {
            return f;
        }
        rt_log("thread 0x%08X was created again: new fiber", ctx);
        DeleteFiber(f->fiber);
    } else {
        if (!create) {
            return NULL;
        }
        if (sCount == MAX_THREADS) {
            rt_fatal("too many threads");
        }
        f = &sFibers[sCount++];
    }
    f->ctx = ctx;
    /* a thread that never ran: its registers are in its context */
    volatile u32 *c = (volatile u32 *)(uintptr_t)ctx;
    memset(&f->cpu, 0, sizeof f->cpu);
    for (int i = 0; i < 15; i++) {
        f->cpu.r[i] = c[1 + i];
    }
    f->cpu.n = c[0] >> 31;
    f->cpu.z = (c[0] >> 30) & 1;
    f->cpu.c = (c[0] >> 29) & 1;
    f->cpu.v = (c[0] >> 28) & 1;
    f->cpu.cpsr_ctl = c[0] & 0xFF;
    f->in_irq = 0;
    f->saved_pc = c[16];
    f->fiber = CreateFiber(1 << 20, thread_entry, f);
    if (!f->fiber) {
        rt_fatal("CreateFiber failed");
    }
    return f;
}

void thread_init(void) {
    if (!sCur) {
        sFibers[0].ctx = 0;            /* bound at the first save */
        sFibers[0].fiber = ConvertThreadToFiber(NULL);
        sCount = 1;
        sCur = &sFibers[0];
    }
}

/* PB2_SELFTEST: check the game's division helpers once the code is in place */
static void selftest(void) {
    static int done;
    if (done++ || !getenv("PB2_SELFTEST")) {
        return;
    }
    Cpu keep = g;
    static const u32 t[][2] = { { 1000, 7 }, { 0xFFFFFFFF, 3 }, { 5, 9 }, { 0x12345678, 0x1234 }, { 100, 1 }, { 0x80000000, 0x10 } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        R(0) = t[i][0]; R(1) = t[i][1]; LR = 0;
        FUN_0208D868();
        rt_log("selftest udiv %08X / %08X = %08X r %08X (expected %08X r %08X)", t[i][0], t[i][1], R(0), R(1),
               t[i][0] / t[i][1], t[i][0] % t[i][1]);
        R(0) = t[i][0]; R(1) = t[i][1]; LR = 0;
        FUN_0208D65C();
        rt_log("selftest sdiv %d / %d = %d r %d (expected %d r %d)", (s32)t[i][0], (s32)t[i][1], (s32)R(0), (s32)R(1),
               (s32)t[i][0] / (s32)t[i][1], (s32)t[i][0] % (s32)t[i][1]);
    }
    g = keep;
}

/* OS_SaveContext(ctx): FUN_0207AB70 */
void hle_FUN_0207AB70(void) {
    thread_init();
    selftest();
    u32 ctx = R(0);
    u32 lr = LR;
    R(0) = ctx + 0x48;
    FUN_0207002C();                    /* CP context (divider state) */
    R(0) = ctx;
    LR = lr;
    Fiber *other = fiber_of(ctx, 0);
    if (other && other != sCur) {
        other->ctx = 0;                /* that context now belongs to this fiber */
    }
    sCur->ctx = ctx;
    ctx_store(ctx, lr + 4);
    sCur->cpu = g;
    sCur->cpu.r[0] = 1;                /* when resumed: "returned 1" */
    R(0) = 0;
}

/* OS_LoadContext(ctx): FUN_0207ABBC */
void thread_load_context(u32 ctx) {
    thread_init();
    Fiber *t = fiber_of(ctx, 1);
    u32 save_r0 = R(0);
    R(0) = ctx + 0x48;
    FUN_0207006C();                    /* CP context */
    R(0) = save_r0;
    switch_to(t);
}

/* end of interrupt (FUN_01FF8CE0): wake the threads queued by interrupts,
 * then switch to the highest priority ready thread if needed */
void thread_switch_context(u32 unused0, u32 unused1) {
    (void)unused0;
    (void)unused1;
    thread_init();
    volatile u32 *wq = (volatile u32 *)(uintptr_t)(DTCM_BASE + 0xA0);
    volatile u16 *need = (volatile u16 *)(uintptr_t)INFO;
    if (wq[0]) {
        for (u32 t = wq[0]; t;) {
            volatile u32 *th = (volatile u32 *)(uintptr_t)t;
            th[0x64 / 4] = 1;
            th[0x78 / 4] = 0;
            th[0x7C / 4] = 0;
            u32 nx = th[0x80 / 4];
            th[0x80 / 4] = 0;
            t = nx;
        }
        wq[0] = 0;
        wq[1] = 0;
        *need = 1;
    }
    if (!*need) {
        return;
    }
    *need = 0;
    volatile u32 *info = (volatile u32 *)(uintptr_t)INFO;
    u32 next = info[2];
    while (next && *(volatile u16 *)(uintptr_t)(next + 0x64) != 1) {
        next = *(volatile u32 *)(uintptr_t)(next + 0x68);
    }
    u32 cur = info[1];
    if (!next || next == cur) {
        return;
    }
    u32 cb = info[3];
    if (cb) {
        static int said;
        if (said++ < 3) {
            rt_log("reschedule: info %08X %08X %08X %08X, cur %08X next %08X", info[0], info[1], info[2], info[3], cur, next);
        }
        Cpu keep = g;
        R(0) = cur;
        R(1) = next;
        call_addr(cb);
        g = keep;
    }
    info[1] = next;
    /* the interrupted thread will continue from here when it is loaded again */
    if (cur) {
        if (!fiber_of(cur, 0)) {
            sCur->ctx = cur;
        }
        ctx_store(cur, 0);
    }
    Fiber *t = fiber_of(next, 1);
    switch_to(t);
}

/* for the watchdog: where every thread stopped */
void thread_dump(void) {
    volatile u32 *info = (volatile u32 *)(uintptr_t)INFO;
    rt_log("threads: current 0x%08X (fiber ctx 0x%08X)", info[1], sCur ? sCur->ctx : 0);
    volatile u32 *os = (volatile u32 *)(uintptr_t)pb2_addr(0x0214C210u);
    rt_log("  OS 0214C210: %08X %08X %08X %08X %08X %08X %08X %08X | info %08X %08X %08X %08X", os[0], os[1], os[2], os[3],
           os[4], os[5], os[6], os[7], info[0], info[1], info[2], info[3]);
    for (u32 t = info[2], k = 0; t && k < 32; t = *(volatile u32 *)(uintptr_t)(t + 0x68), k++) {
        rt_log("  list: thread %08X state %u prio %u id %u", t, *(volatile u32 *)(uintptr_t)(t + 0x64),
               *(volatile u32 *)(uintptr_t)(t + 0x70), *(volatile u32 *)(uintptr_t)(t + 0x6C));
    }
    for (int i = 0; i < sCount; i++) {
        Fiber *f = &sFibers[i];
        u32 st = f->ctx ? *(volatile u16 *)(uintptr_t)(f->ctx + 0x64) : 0;
        rt_log("  fiber %d ctx 0x%08X state %u lr 0x%08X sp 0x%08X r0 0x%08X%s", i, f->ctx, st,
               f->cpu.r[14], f->cpu.r[13], f->cpu.r[0], f == sCur ? "  <- running" : "");
        /* return addresses on its game stack (code addresses 0x0200xxxx-0x021FFFFF) */
        char buf[400];
        int n = 0;
        for (u32 k = 0; k < 48 && n < (int)sizeof buf - 12; k++) {
            u32 w = *(volatile u32 *)(uintptr_t)(f->cpu.r[13] + 4 * k);
            if (w >= 0x02004000 && w < 0x02200000 && (w & 1)) {
                n += snprintf(buf + n, sizeof buf - (size_t)n, " %08X", w);
            }
        }
        buf[n] = 0;
        rt_log("    stack:%s", buf);
    }
}

void sysarea_dump(void) {
    for (u32 a = 0x027FFF80; a < 0x02800000; a += 16) {
        volatile u32 *p = (volatile u32 *)(uintptr_t)a;
        rt_log("  %08X: %08X %08X %08X %08X", a, p[0], p[1], p[2], p[3]);
    }
}
