/*
 * dispatch.c - indirect calls (function pointers, interrupt handlers...).
 *
 * gDispatch (generated) maps each function's address (| 1 for Thumb) to its
 * C translation. Overlays share address ranges: among several candidates the
 * one whose overlay is loaded is chosen by comparing the bytes in memory with
 * that overlay's image from the ROM.
 */
#include "rt.h"
#include "dispatch.h"

#include <stdlib.h>
#include <string.h>

static int overlay_loaded_at(int ov, u32 addr) {
    u32 size;
    const u8 *img = overlay_image(ov, &size);
    RomOverlay *o = &gOverlays[ov];
    u32 off = (addr & ~1u) - o->ram;
    if (!img || off + 16 > size) {
        return 0;
    }
    return memcmp((const void *)(uintptr_t)(addr & ~1u), img + off, 16) == 0;
}

/* crt0 copies tiny ARM routines into ITCM and runs them to synchronize the
 * boot with the ARM7 (lib/asm/crt0.s, _02004FD4 / _02004F94 / _02004FFC):
 * they are data for the recompiler, so they are reproduced here, with the
 * ARM7's part of the handshake. */
static int boot_snippet(u32 addr) {
    if ((addr & ~1u) == 0x01FF8000) {
        u32 w = *(volatile u32 *)(uintptr_t)0x01FF8000;
        if (w == 0xE1D120B0) {               /* counters: ARM9 +1, wait ARM7, ARM9 +1 */
            volatile u16 *a9 = (volatile u16 *)(uintptr_t)R(0), *a7 = (volatile u16 *)(uintptr_t)R(1);
            *a9 = (u16)(*a9 + 1);
            *a7 = (u16)(*a7 + 1);            /* the ARM7 answers */
            *a9 = (u16)(*a9 + 1);
            return 1;
        }
        if (w == 0xE1D020B0) {               /* wait until [r0] == r1 (boot stage) */
            *(volatile u16 *)(uintptr_t)R(0) = (u16)R(1);
            return 1;
        }
        rt_fatal("unknown boot routine in ITCM (0x%08X)", w);
    }
    if ((addr & ~1u) == 0x01FF801C) {        /* DSi clock switch: DS mode, nothing */
        R(0) &= 1;
        return 1;
    }
    return 0;
}

void call_addr(u32 addr) {
    if ((addr >> 16) == 0x01FF && boot_snippet(addr)) {
        return;
    }
    /* binary search of the first entry with this address */
    unsigned lo = 0, hi = gDispatchCount;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (gDispatch[mid].addr < addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= gDispatchCount || gDispatch[lo].addr != addr) {
        /* maybe called with the wrong mode bit */
        u32 alt = addr ^ 1;
        lo = 0;
        hi = gDispatchCount;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if (gDispatch[mid].addr < alt) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo >= gDispatchCount || gDispatch[lo].addr != alt) {
            rt_fatal("call to 0x%08X: no function there", addr);
        }
        addr = alt;
    }
    const DispatchEntry *first = &gDispatch[lo];
    if (lo + 1 >= gDispatchCount || gDispatch[lo + 1].addr != addr) {
        first->fn();
        return;
    }
    /* PB2_TRACECALL=hex: log the overlay chosen for calls to that address */
    static int traceInit;
    static u32 traceAddr;
    if (!traceInit) {
        traceInit = 1;
        const char *t = getenv("PB2_TRACECALL");
        traceAddr = t ? (u32)strtoul(t, NULL, 16) : 0;
    }
    for (unsigned i = lo; i < gDispatchCount && gDispatch[i].addr == addr; i++) {
        if (gDispatch[i].overlay < 0 || overlay_loaded_at(gDispatch[i].overlay, addr)) {
            if (traceAddr && (addr & ~1u) == (traceAddr & ~1u)) {
                rt_log("call 0x%08X -> overlay %d (lr 0x%08X)", addr, gDispatch[i].overlay, LR);
            }
            gDispatch[i].fn();
            return;
        }
    }
    rt_fatal("call to 0x%08X: no loaded overlay has this function", addr);
}

/* Black 2 addresses known to the runtime (thread info, ARM7 handshake...)
 * in the game that was compiled: identity for Black 2; for White 2 the code
 * moved after the version blocks (tools/recomp/w2addr.py, rc_addrmap.c) */
extern const u32 gAddrMap[][2];
extern const unsigned gAddrMapCount;
extern const u32 gBssStart, gBssEnd;
extern const s32 gBssDelta;

u32 pb2_addr(u32 b2) {
    if (b2 >= gBssStart && b2 < gBssEnd) {
        return (u32)((s32)b2 + gBssDelta);
    }
    unsigned lo = 0, hi = gAddrMapCount;            /* last function starting at or before b2 */
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (gAddrMap[mid][0] <= b2) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo ? b2 + (gAddrMap[lo - 1][1] - gAddrMap[lo - 1][0]) : b2;
}

u32 pb2_b2addr(u32 a) {                             /* the other way (code addresses) */
    unsigned lo = 0, hi = gAddrMapCount;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (gAddrMap[mid][1] <= a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo ? a - (gAddrMap[lo - 1][1] - gAddrMap[lo - 1][0]) : a;
}
