/*
 * cpu.h - state and helpers used by the recompiled ARM9 code of Pokemon
 * Black 2 / White 2 (tools/recomp/recomp.py).
 *
 * Every ARM/Thumb function of the game becomes a C function working on this
 * CPU state. Memory is the DS memory map at its native addresses (reserved by
 * the runtime, as in the HeartGold port); I/O registers go through io_*.
 */
#ifndef PB2_CPU_H
#define PB2_CPU_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

typedef struct Cpu {
    u32 r[16];
    u32 n, z, c, v;      /* flags, 0 or 1 */
    u32 cpsr_ctl;        /* mode bits, I/F bits, T bit (low byte of CPSR) */
    u32 spsr;
} Cpu;

extern Cpu g;

/* PB2_FNTRACE: the last function entries (address, lr), shown by the
 * watchdog and the crash handler */
#define FNTRACE_N 256
extern int gFnTrace;
extern u32 gFnRing[FNTRACE_N][2];
extern unsigned gFnPos;
#define FN_ENTER(addr) do { if (gFnTrace) { gFnRing[gFnPos % FNTRACE_N][0] = (addr); \
    gFnRing[gFnPos % FNTRACE_N][1] = g.r[14]; gFnPos++; } } while (0)

/* backward branches: every LOOP_BUDGET iterations a scanline passes and
 * interrupts are delivered, so that loops waiting for an IRQ-driven flag end */
extern int gLoopBudget;
void loop_tick(void);
#define LOOP_TICK() do { if (--gLoopBudget < 0) loop_tick(); } while (0)

#define R(i) g.r[i]
#define SP g.r[13]
#define LR g.r[14]

/* ---- memory ------------------------------------------------------------- */
u32 io_r32(u32 a);
u32 io_r16(u32 a);
u32 io_r8(u32 a);
void io_w32(u32 a, u32 v);
void io_w16(u32 a, u32 v);
void io_w8(u32 a, u32 v);

#define IS_IO(a) (((a) >> 24) == 4)
static inline u32 rd32(u32 a) { if (IS_IO(a)) return io_r32(a); a &= ~3u; return *(volatile u32 *)(uintptr_t)a; }
static inline u32 rd16(u32 a) { if (IS_IO(a)) return io_r16(a); a &= ~1u; return *(volatile u16 *)(uintptr_t)a; }
static inline u32 rd8(u32 a)  { if (IS_IO(a)) return io_r8(a); return *(volatile u8 *)(uintptr_t)a; }
static inline s32 rds16(u32 a) { return (s16)rd16(a); }
static inline s32 rds8(u32 a)  { return (s8)rd8(a); }
static inline void wr32(u32 a, u32 v) { if (IS_IO(a)) { io_w32(a, v); return; } *(volatile u32 *)(uintptr_t)(a & ~3u) = v; }
static inline void wr16(u32 a, u32 v) { if (IS_IO(a)) { io_w16(a, v); return; } *(volatile u16 *)(uintptr_t)(a & ~1u) = (u16)v; }
static inline void wr8(u32 a, u32 v)  { if (IS_IO(a)) { io_w8(a, v); return; } *(volatile u8 *)(uintptr_t)a = (u8)v; }
/* ARM LDR of an unaligned word rotates it */
static inline u32 rd32r(u32 a) { u32 v = rd32(a); u32 s = (a & 3) * 8; return s ? (v >> s) | (v << (32 - s)) : v; }

/* ---- flags -------------------------------------------------------------- */
#define SET_NZ(x) do { u32 t_ = (x); g.n = t_ >> 31; g.z = (t_ == 0); } while (0)
static inline u32 add_f(u32 a, u32 b) { u32 r = a + b; g.n = r >> 31; g.z = !r; g.c = r < a; g.v = ((~(a ^ b) & (a ^ r)) >> 31) & 1; return r; }
static inline u32 sub_f(u32 a, u32 b) { u32 r = a - b; g.n = r >> 31; g.z = !r; g.c = a >= b; g.v = (((a ^ b) & (a ^ r)) >> 31) & 1; return r; }
static inline u32 adc_f(u32 a, u32 b) { u64 w = (u64)a + b + g.c; u32 r = (u32)w; g.n = r >> 31; g.z = !r; g.c = (u32)(w >> 32); g.v = ((~(a ^ b) & (a ^ r)) >> 31) & 1; return r; }
static inline u32 sbc_f(u32 a, u32 b) { u64 w = (u64)a - b - !g.c; u32 r = (u32)w; g.n = r >> 31; g.z = !r; g.c = !(w >> 32); g.v = (((a ^ b) & (a ^ r)) >> 31) & 1; return r; }
static inline u32 adc(u32 a, u32 b) { return a + b + g.c; }
static inline u32 sbc(u32 a, u32 b) { return a - b - !g.c; }

/* shifts with the barrel shifter's carry out (sets g.c when f) */
static inline u32 lsl_c(u32 x, u32 s, int f) { s &= 0xff; if (!s) return x; if (s < 32) { if (f) g.c = (x >> (32 - s)) & 1; return x << s; } if (f) g.c = (s == 32) ? (x & 1) : 0; return 0; }
static inline u32 lsr_c(u32 x, u32 s, int f) { s &= 0xff; if (!s) return x; if (s < 32) { if (f) g.c = (x >> (s - 1)) & 1; return x >> s; } if (f) g.c = (s == 32) ? (x >> 31) : 0; return 0; }
static inline u32 asr_c(u32 x, u32 s, int f) { s &= 0xff; if (!s) return x; if (s < 32) { if (f) g.c = (x >> (s - 1)) & 1; return (u32)((s32)x >> s); } if (f) g.c = x >> 31; return (u32)((s32)x >> 31); }
static inline u32 ror_c(u32 x, u32 s, int f) { s &= 0xff; if (!s) return x; s &= 31; if (!s) { if (f) g.c = x >> 31; return x; } u32 r = (x >> s) | (x << (32 - s)); if (f) g.c = r >> 31; return r; }
static inline u32 rrx_c(u32 x, int f) { u32 r = (x >> 1) | (g.c << 31); if (f) g.c = x & 1; return r; }
/* immediate shift encodings: LSR/ASR #32 are written #0 in the encoding */
static inline u32 lsr_i(u32 x, u32 s, int f) { return lsr_c(x, s ? s : 32, f); }
static inline u32 asr_i(u32 x, u32 s, int f) { return asr_c(x, s ? s : 32, f); }

/* condition codes */
#define C_EQ (g.z)
#define C_NE (!g.z)
#define C_HS (g.c)
#define C_LO (!g.c)
#define C_MI (g.n)
#define C_PL (!g.n)
#define C_VS (g.v)
#define C_VC (!g.v)
#define C_HI (g.c && !g.z)
#define C_LS (!g.c || g.z)
#define C_GE (g.n == g.v)
#define C_LT (g.n != g.v)
#define C_GT (!g.z && g.n == g.v)
#define C_LE (g.z || g.n != g.v)

static inline u32 clz32(u32 x) { return x ? (u32)__builtin_clz(x) : 32; }
static inline s32 sat32(s64 x) { return x > 0x7fffffff ? 0x7fffffff : x < -0x7fffffffll - 1 ? (s32)0x80000000 : (s32)x; }

/* ---- runtime services ---------------------------------------------------- */
void call_addr(u32 addr);                 /* indirect call (dispatch.c) */
void swi_call(u32 n);                     /* BIOS (bios.c) */
u32 cp15_read(u32 crn, u32 crm, u32 op2);
void cp15_write(u32 crn, u32 crm, u32 op2, u32 v);
u32 cpsr_read(void);
void cpsr_write(u32 v, u32 mask);
void unimpl(const char *where, const char *text);
void dsprot_call(const char *name, u32 tablePool, u32 indexPool);
#define UNIMPL(w, t) unimpl(w, t)

#endif
