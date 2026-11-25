/* cpu.c - CPU state, CPSR with banked registers, CP15, logging. */
#include "rt.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

Cpu g;
int gTrace;

static u32 sBankSP[32], sBankLR[32], sBankSPSR[32];

static int bank_of(u32 mode) {
    mode &= 0x1F;
    return mode == 0x1F ? 0x10 : (int)mode;      /* sys shares usr's registers */
}

void cpu_init(void) {
    memset(&g, 0, sizeof g);
    g.cpsr_ctl = 0xD3;                           /* svc, IRQ/FIQ disabled, ARM */
}

u32 cpsr_read(void) {
    return (g.n << 31) | (g.z << 30) | (g.c << 29) | (g.v << 28) | (g.cpsr_ctl & 0xFFFFFF);
}

void cpsr_write(u32 v, u32 mask) {
    if (mask & 0xFF000000) {
        g.n = v >> 31;
        g.z = (v >> 30) & 1;
        g.c = (v >> 29) & 1;
        g.v = (v >> 28) & 1;
    }
    if (mask & 0xFF) {
        u32 old = g.cpsr_ctl & 0x1F, nw = v & 0x1F;
        if (bank_of(old) != bank_of(nw)) {
            int ob = bank_of(old), nb = bank_of(nw);
            sBankSP[ob] = R(13);
            sBankLR[ob] = R(14);
            sBankSPSR[ob] = g.spsr;
            R(13) = sBankSP[nb];
            R(14) = sBankLR[nb];
            g.spsr = sBankSPSR[nb];
        }
        g.cpsr_ctl = (g.cpsr_ctl & ~0xFFu) | (v & 0xFF);
        if (!(v & 0x80)) {
            irq_check();                        /* interrupts enabled again */
        }
    }
}

void cpu_save(Cpu *c) {
    *c = g;
}

void cpu_restore(const Cpu *c) {
    u32 cur = g.cpsr_ctl & 0x1F, want = c->cpsr_ctl & 0x1F;
    if (bank_of(cur) != bank_of(want)) {
        sBankSP[bank_of(cur)] = R(13);
        sBankLR[bank_of(cur)] = R(14);
    }
    g = *c;
}

/* CP15: the game configures protection regions and TCMs; on PC the map is
 * fixed, so writes are remembered and reads return them (DTCM base...). */
static u32 sCp15[16][16][8];
u32 cp15_read(u32 crn, u32 crm, u32 op2) {
    if (crn == 0) {
        return op2 == 1 ? 0x0F0D2112u : 0x41059461u;   /* cache type / ARM946E-S */
    }
    if (crn == 9 && crm == 1 && op2 == 0 && sCp15[9][1][0] == 0) {
        return DTCM_BASE | 0x0A;
    }
    return sCp15[crn & 15][crm & 15][op2 & 7];
}

void cp15_write(u32 crn, u32 crm, u32 op2, u32 v) {
    sCp15[crn & 15][crm & 15][op2 & 7] = v;
    if (crn == 7 && crm == 0 && op2 == 4) {
        time_wait_irq();                        /* wait for interrupt (halt) */
    }
}

void unimpl(const char *where, const char *text) {
    rt_fatal("untranslated instruction in %s: %s", where, text);
}

void dsprot_call(const char *name) {
    /* Nintendo's anti-piracy checks: a genuine cartridge, no flash card */
    static int said;
    if (!said++) {
        rt_log("DSProt check %s: answered 'genuine cartridge'", name);
    }
    R(0) = 0;
}

void rt_write_err(const char *s, int n);   /* watchdog.c: no CRT lock */
void rt_log(const char *fmt, ...) {
    /* formatted in a local buffer and written at once without the CRT stream
     * lock: the watchdog may suspend the game thread at any point */
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(b, sizeof b, "[pb2 %6u] ", gFrame);
    int m = vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
    va_end(ap);
    n += m < 0 ? 0 : m > (int)(sizeof b - n - 2) ? (int)(sizeof b - n - 2) : m;
    b[n++] = '\n';
    rt_write_err(b, n);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[pb2 %6u] FATAL: ", gFrame);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fprintf(stderr, "  pc~? r0=%08X r1=%08X r2=%08X r3=%08X sp=%08X lr=%08X\n", R(0), R(1), R(2), R(3), R(13), R(14));
    fflush(stderr);
    exit(3);
}
