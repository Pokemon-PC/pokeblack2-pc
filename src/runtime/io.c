/*
 * io.c - ARM9 I/O registers with their side effects, time and interrupts.
 *
 * Registers live in memory at 0x04000000 (the 2D/3D renderers can read them
 * there). Time advances by scanlines (263 per frame) when the game waits or
 * polls a timing register; interrupts are delivered at those points.
 */
#include "rt.h"
#include "../video/video.h"
#include "../video/gpu3d.h"
#include "../video/vram.h"

#include <string.h>

#define REG8(a)  (*(volatile u8 *)(uintptr_t)(a))
#define REG16(a) (*(volatile u16 *)(uintptr_t)(a))
#define REG32(a) (*(volatile u32 *)(uintptr_t)(a))

#define LINES 263
u32 gFrame;
static u32 sLine;
int gInIrq;
#define sInIrq gInIrq
static u64 sTimerCycles[4];

/* unknown registers: logged once each */
static u8 sSeen[0x2000];
static void note(u32 a, int write, u32 v) {
    u32 k = (a & 0xFFFF) >> 1;
    if (k < sizeof sSeen && !sSeen[k]) {
        sSeen[k] = 1;
        if (gTrace) {
            rt_log("io %s %08X = %08X", write ? "write" : "read ", a, v);
        }
    }
}

/* ------------------------------------------------------------ interrupts */
void irq_raise(u32 bits) {
    REG32(0x04000214) |= bits;
    irq_check();
}

void irq_check(void) {
    /* level-triggered: the geometry FIFO condition (always true here) */
    u32 gxmode = REG32(0x04000600) >> 30;
    if (gxmode == 1 || gxmode == 2) {
        REG32(0x04000214) |= 1u << 21;
    }
    if (sInIrq || !(REG32(0x04000208) & 1) || (g.cpsr_ctl & 0x80)) {
        return;
    }
    u32 pending = REG32(0x04000210) & REG32(0x04000214);
    if (!pending) {
        return;
    }
    u32 handler = REG32(DTCM_BASE + 0x3FFC);
    if (!handler) {
        return;
    }
    {
        static u32 seen;
        u32 nb = pending & ~seen;
        if (nb) {
            seen |= nb;
            for (int b = 0; b < 32; b++) {
                if (nb & (1u << b)) {
                    rt_log("irq bit %d first raised, handler table entry %08X", b, REG32(DTCM_BASE + 0x20 + 4 * b));
                }
            }
        }
    }
    Cpu saved;
    cpu_save(&saved);
    sInIrq = 1;
    u32 spsr = cpsr_read();
    cpsr_write((spsr & ~0xFFu) | 0x92, 0xFF);           /* IRQ mode, IRQ off, ARM */
    g.spsr = spsr;
    R(0) = 0x04000000;
    LR = 0xFFFF0290;                                     /* BIOS return address */
    call_addr(handler);
    sInIrq = 0;
    cpu_restore(&saved);
}

/* ------------------------------------------------------------------- DMA */
static void dma_run(int ch) {
    u32 base = 0x040000B0 + 12 * ch;
    u32 src = REG32(base), dst = REG32(base + 4), cnt = REG32(base + 8);
    u32 n = cnt & 0x1FFFFF;
    if (!n) {
        n = 0x200000;
    }
    int wide = (cnt >> 26) & 1;
    int dmode = (cnt >> 21) & 3, smode = (cnt >> 23) & 3;
    int step = wide ? 4 : 2;
    int timing = (cnt >> 27) & 7;
    if (timing == 7) {          /* geometry FIFO: 112 words per burst, here all at once */
        dmode = 2;
    }
    for (u32 i = 0; i < n; i++) {
        /* below 0x01000000 is the BIOS: reads as 0 here, writes are ignored */
        u32 v = src < 0x01000000 ? 0 : wide ? rd32(src) : rd16(src);
        if (dst >= 0x01000000) {
            if (wide) {
                wr32(dst, v);
            } else {
                wr16(dst, v);
            }
        }
        src += smode == 0 ? step : smode == 1 ? -step : 0;
        dst += dmode == 0 || dmode == 3 ? step : dmode == 1 ? -step : 0;
    }
    if (!(cnt & (1u << 25)) || timing == 0) {      /* not repeating: done */
        REG32(base + 8) = cnt & ~0x80000000u;
    } else {
        REG32(base) = src;
        if (dmode != 3) {
            REG32(base + 4) = dst;
        }
    }
    if (cnt & (1u << 30)) {
        static int said;
        if (said++ < 10) {
            rt_log("DMA %d done with IRQ (cnt %08X dst %08X) IE %08X IME %u cpsr %02X inirq %d", ch, cnt, REG32(base + 4),
                   REG32(0x04000210), REG32(0x04000208) & 1, g.cpsr_ctl, sInIrq);
        }
        irq_raise(1u << (8 + ch));
    }
}

static void dma_trigger(int timing) {
    for (int ch = 0; ch < 4; ch++) {
        u32 cnt = REG32(0x040000B0 + 12 * ch + 8);
        if ((cnt & 0x80000000u) && (int)((cnt >> 27) & 7) == timing) {
            dma_run(ch);
        }
    }
}

/* ------------------------------------------------------------------ time */
static void timers_step(u32 cycles) {
    static const int presc[4] = { 1, 64, 256, 1024 };
    for (int t = 0; t < 4; t++) {
        u32 cr = REG16(0x04000102 + 4 * t);
        if (!(cr & 0x80) || (cr & 4)) {
            continue;                         /* stopped, or count-up (not emulated) */
        }
        sTimerCycles[t] += cycles;
        u32 ticks = (u32)(sTimerCycles[t] / presc[cr & 3]);
        sTimerCycles[t] %= presc[cr & 3];
        static u16 counter[4];
        u32 c = counter[t] + ticks;
        while (c > 0xFFFF) {
            c = c - 0x10000 + REG16(0x04000100 + 4 * t);
            if (cr & 0x40) {
                irq_raise(1u << (3 + t));
            }
        }
        counter[t] = (u16)c;
    }
}

int gLoopBudget = 256;
void loop_tick(void) {
    gLoopBudget = 256;
    time_advance_line();
    irq_check();
}

static void gx_fifo_irq(void);
void time_advance_line(void) {
    sLine++;
    if (sLine >= LINES) {
        sLine = 0;
        gFrame++;
    }
    REG16(0x04000006) = (u16)sLine;
    gx_fifo_irq();
    u16 stat = REG16(0x04000004);
    stat &= ~0x7;
    if (sLine >= 192 && sLine < LINES - 1) {
        stat |= 1;
    }
    u32 vmatch = (stat >> 8) | ((stat & 0x80) << 1);
    if (sLine == vmatch) {
        stat |= 4;
    }
    REG16(0x04000004) = stat;
    if (sLine == 192) {
        /* the ARM7 counts VBlanks in the system area (OS_GetVBlankCount) */
        (*(volatile u32 *)(uintptr_t)0x027FFC3C)++;
        video_frame();
        if (stat & 0x08) {
            REG32(0x04000214) |= 1;
        }
        dma_trigger(1);
    }
    if (sLine < 192) {
        dma_trigger(2);
        if (stat & 0x10) {
            REG32(0x04000214) |= 2;
        }
    }
    if ((stat & 0x20) && sLine == vmatch) {
        REG32(0x04000214) |= 4;
    }
    timers_step(2130);
    arm7_step();
    irq_check();
}

void time_wait_irq(void) {
    /* halt: run time until an interrupt is taken */
    for (int i = 0; i < LINES * 4; i++) {
        u32 if0 = REG32(0x04000214);
        time_advance_line();
        if (REG32(0x04000214) != if0 || (REG32(0x04000210) & REG32(0x04000214))) {
            if (REG32(0x04000208) & 1) {
                irq_check();
                return;
            }
        }
    }
}

/* ------------------------------------------------- hardware math (div/sqrt) */
static void math_div(void) {
    u32 cnt = REG16(0x04000280) & 3;
    s64 num = cnt == 0 ? (s64)(s32)REG32(0x04000290) : (s64)(REG32(0x04000290) | (u64)REG32(0x04000294) << 32);
    s64 den = cnt == 2 ? (s64)(REG32(0x04000298) | (u64)REG32(0x0400029C) << 32) : (s64)(s32)REG32(0x04000298);
    s64 q, r;
    if (den == 0) {
        q = num < 0 ? 1 : -1;
        r = num;
        REG16(0x04000280) |= 0x4000;
    } else {
        REG16(0x04000280) &= ~0x4000;
        q = num / den;
        r = num % den;
    }
    REG32(0x040002A0) = (u32)q;
    REG32(0x040002A4) = (u32)((u64)q >> 32);
    REG32(0x040002A8) = (u32)r;
    REG32(0x040002AC) = (u32)((u64)r >> 32);
}

static void math_sqrt(void) {
    u64 v = (REG16(0x040002B0) & 1) ? (REG32(0x040002B8) | (u64)REG32(0x040002BC) << 32) : REG32(0x040002B8);
    u64 r = 0, bit = 1ull << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    REG32(0x040002B4) = (u32)r;
}

/* geometry FIFO interrupt (IF bit 21): GXSTAT bits 30-31 = 1 "less than
 * half full" or 2 "empty". The FIFO is always empty here: the condition
 * holds as soon as it is enabled. */
static void gx_fifo_irq(void) {
    u32 mode = REG32(0x04000600) >> 30;
    if (mode == 1 || mode == 2) {
        irq_raise(1u << 21);
    }
}

/* ------------------------------------------------------------- accessors */
static void after_write(u32 a) {
    a &= ~3u;
    if (a >= 0x040000B8 && a <= 0x040000DC && ((a - 0x040000B8) % 12) == 0) {
        int ch = (int)((a - 0x040000B8) / 12);
        u32 cnt = REG32(a);
        if ((cnt & 0x80000000u)) {
            int timing = (cnt >> 27) & 7;
            if (timing == 0 || timing == 7) {
                static int said;
                if (timing == 7 && said++ < 5) {
                    rt_log("GX DMA ch %d src %08X n %X cnt %08X IE %08X IME %u cpsr %02X", ch, REG32(a - 8), cnt & 0x1FFFFF, cnt,
                           REG32(0x04000210), REG32(0x04000208) & 1, g.cpsr_ctl);
                }
                dma_run(ch);
            } else if (timing == 5) {
                /* card DMA: runs as the card delivers data (card.c) */
            }
        }
        return;
    }
    if (a == 0x04000600) {
        gx_fifo_irq();
        return;
    }
    if (a >= 0x04000240 && a <= 0x04000248) {      /* VRAMCNT A-I */
        pc_vram_sync();
        return;
    }
    if (a >= 0x04000280 && a <= 0x0400029C) {
        math_div();
        return;
    }
    if (a >= 0x040002B0 && a <= 0x040002BC) {
        math_sqrt();
        return;
    }
    if (a == 0x04000208 || a == 0x04000210) {
        irq_check();
    }
}

static int special_read(u32 a, u32 *v, int size) {
    switch (a & ~3u) {
    case 0x04000004:                 /* DISPSTAT / VCOUNT: let time pass */
        time_advance_line();
        return 0;
    case 0x04000130:
        *v = video_keys();                  /* screen.c, read at each V-Blank */
        return 1;
    case 0x04000180:
        return 0;
    case 0x04100000:
        *v = arm7_fifo_recv();
        return 1;
    case 0x04100010:
        *v = card_read_data();
        return 1;
    case 0x04000600:                 /* GXSTAT: geometry FIFO empty (and < half), engine idle */
        *v = (REG32(0x04000600) & 0xC000BF02u) | 0x06000000u;
        return 1;
    case 0x04004000:                 /* DSi SCFG: zero = DS mode */
    case 0x04004004:
    case 0x04004008:
    case 0x04004010:
        *v = 0;
        return 1;
    }
    if ((a & ~3u) == 0x04000214) {
        static u32 polls;
        if (++polls % 64 == 0) {
            time_advance_line();
        }
    }
    return 0;
}

u32 io_r32(u32 a) {
    u32 v = 0;
    if (special_read(a, &v, 32)) {
        return v;
    }
    note(a, 0, REG32(a & ~3u));
    return REG32(a & ~3u);
}

u32 io_r16(u32 a) {
    u32 v = 0;
    if (special_read(a, &v, 16)) {
        return (a & 2) ? v >> 16 : v & 0xFFFF;
    }
    note(a, 0, REG16(a & ~1u));
    return REG16(a & ~1u);
}

u32 io_r8(u32 a) {
    u32 v = 0;
    if (special_read(a, &v, 8)) {
        return (v >> (8 * (a & 3))) & 0xFF;
    }
    return REG8(a);
}

static int special_write(u32 a, u32 v, int size) {
    u32 w = a & ~3u;
    if (w == 0x04000214) {           /* IF: write 1 to clear */
        u32 bits = size == 32 ? v : size == 16 ? (v & 0xFFFF) << (8 * (a & 2)) : (v & 0xFF) << (8 * (a & 3));
        REG32(0x04000214) &= ~bits;
        return 1;
    }
    if (w == 0x04000180 && !(a & 2)) {
        u16 sync = REG16(0x04000180);
        sync = (u16)((sync & 0x000F) | (v & 0x6F00));
        REG16(0x04000180) = sync;
        arm7_ipc_sync_write(v);
        return 1;
    }
    if (w == 0x04000188) {
        arm7_fifo_send(v);
        return 1;
    }
    if (w == 0x04000184 && !(a & 2)) {
        /* IPCFIFOCNT: bits 2/10/15 are settings, 0/1/8/9 status (read-only),
         * writing 1 to bit 14 acknowledges an error, bit 3 flushes the send FIFO */
        u16 cur = REG16(0x04000184);
        u16 nv = (u16)((cur & 0x0303) | (v & 0x8404));
        if (!(v & 0x4000)) {
            nv |= cur & 0x4000;
        }
        REG16(0x04000184) = nv;
        return 1;
    }
    if (w == 0x040001A4 && size == 32 && (v & 0x80000000u)) {
        REG32(w) = v;
        card_write_cmd();
        return 1;
    }
    if (w >= 0x04000400 && w < 0x04000600) {       /* 3D geometry commands (gpu3d.c) */
        if (size == 32) {
            REG32(w) = v;
            pc_g3_write(w, v);
        } else {
            static int said;
            if (said++ < 5) {
                rt_log("3D register %08X written with %d bits (ignored)", a, size);
            }
        }
        return 1;
    }
    if (w == 0x04000600) {                          /* GXSTAT: only the IRQ mode is writable */
        u32 bits = size == 32 ? v : size == 16 ? (v & 0xFFFF) << (8 * (a & 2)) : (v & 0xFF) << (8 * (a & 3));
        if (bits & 0x8000) {
            REG32(w) &= ~0x8000u;                   /* acknowledge a matrix stack error */
        }
        if (size == 32 || (a & 3) >= 2) {
            REG32(w) = (REG32(w) & ~0xC0000000u) | (bits & 0xC0000000u);
        }
        gx_fifo_irq();
        return 1;
    }
    return 0;
}

void io_w32(u32 a, u32 v) {
    if (special_write(a, v, 32)) {
        return;
    }
    REG32(a & ~3u) = v;
    note(a, 1, v);
    after_write(a);
}

void io_w16(u32 a, u32 v) {
    if (special_write(a, v, 16)) {
        return;
    }
    REG16(a & ~1u) = (u16)v;
    note(a, 1, v);
    after_write(a);
}

void io_w8(u32 a, u32 v) {
    if (special_write(a, v, 8)) {
        return;
    }
    REG8(a) = (u8)v;
    after_write(a);
}

void io_init(void) {
    memset((void *)(uintptr_t)0x04000000, 0, 0x10000);
    pc_gpu3d_reset();
    video_init();
    REG16(0x04000130) = 0x03FF;
    REG32(0x04000304) = 0x0003;      /* POWCNT1: LCDs and 2D engine A on */
}

void dma_trigger_card(void) { dma_trigger(5); }
