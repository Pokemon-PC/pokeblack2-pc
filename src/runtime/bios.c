/* bios.c - the ARM9 BIOS calls (SWI) the game uses, in C (GBATEK numbering). */
#include "rt.h"

#include <string.h>

static void lz77(u32 src, u32 dst, int wide) {
    u32 head = rd32(src);
    u32 size = head >> 8;
    src += 4;
    u32 out = 0;
    u32 pending = 0, have_low = 0;
    while (out < size) {
        u32 flags = rd8(src++);
        for (int i = 0; i < 8 && out < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 b1 = rd8(src++), b2 = rd8(src++);
                u32 n = (b1 >> 4) + 3, disp = (((b1 & 0xF) << 8) | b2) + 1;
                while (n-- && out < size) {
                    u32 v = rd8(dst + out - disp);
                    if (!wide) {
                        wr8(dst + out, v);
                    } else if (out & 1) {
                        wr16(dst + out - 1, pending | v << 8);
                    } else {
                        pending = v;
                    }
                    if (wide && !(out & 1)) {
                        wr8(dst + out, v);    /* readable for later back-references */
                    }
                    out++;
                }
            } else {
                u32 v = rd8(src++);
                if (!wide) {
                    wr8(dst + out, v);
                } else if (out & 1) {
                    wr16(dst + out - 1, pending | v << 8);
                } else {
                    pending = v;
                    wr8(dst + out, v);
                }
                out++;
            }
        }
    }
    (void)have_low;
}

static u16 crc16(u16 crc, u32 p, u32 len) {
    static const u16 val[8] = { 0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001 };
    for (u32 i = 0; i < len; i++) {
        crc ^= (u16)rd8(p + i);
        for (int j = 0; j < 8; j++) {
            int carry = crc & 1;
            crc >>= 1;
            if (carry) {
                crc ^= (u16)(val[j] << (7 - j));
            }
        }
    }
    return crc;
}

void swi_call(u32 n) {
    switch (n) {
    case 0x03:                       /* WaitByLoop: the other CPU gets a turn */
        arm7_on_wait();
        return;
    case 0x04: {                     /* IntrWait(discard, flags) */
        u32 flags = R(1);
        u32 *check = (u32 *)(uintptr_t)(DTCM_BASE + 0x3FF8);
        if (R(0)) {
            *check &= ~flags;
        }
        for (int guard = 0; !(*check & flags); guard++) {
            time_wait_irq();
            if (guard > 600) {
                rt_log("IntrWait(%08X): nothing after 600 waits", flags);
                break;
            }
        }
        *check &= ~flags;
        return;
    }
    case 0x05:                       /* VBlankIntrWait */
        R(0) = 1;
        R(1) = 1;
        swi_call(0x04);
        return;
    case 0x06:                       /* Halt */
        time_wait_irq();
        return;
    case 0x09: {                     /* Div */
        s32 num = (s32)R(0), den = (s32)R(1);
        if (den == 0) {
            rt_log("BIOS Div by zero");
            return;
        }
        s32 q = num / den;
        R(1) = (u32)(num % den);
        R(0) = (u32)q;
        R(3) = (u32)(q < 0 ? -q : q);
        return;
    }
    case 0x0B: {                     /* CpuSet(src, dst, cnt) */
        u32 src = R(0), dst = R(1), cnt = R(2);
        u32 count = cnt & 0x1FFFFF;
        int fill = (cnt >> 24) & 1, wide = (cnt >> 26) & 1;
        u32 fillv = wide ? rd32(src) : rd16(src);
        for (u32 i = 0; i < count; i++) {
            if (wide) {
                wr32(dst + 4 * i, fill ? fillv : rd32(src + 4 * i));
            } else {
                wr16(dst + 2 * i, fill ? fillv : rd16(src + 2 * i));
            }
        }
        return;
    }
    case 0x0C: {                     /* CpuFastSet */
        u32 src = R(0), dst = R(1), cnt = R(2);
        u32 count = (cnt & 0x1FFFFF + 7) & ~7u;
        count = ((cnt & 0x1FFFFF) + 7) & ~7u;
        int fill = (cnt >> 24) & 1;
        u32 fillv = rd32(src);
        for (u32 i = 0; i < count; i++) {
            wr32(dst + 4 * i, fill ? fillv : rd32(src + 4 * i));
        }
        return;
    }
    case 0x0D: {                     /* Sqrt */
        u32 v = R(0), r = 0, bit = 1u << 30;
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
        R(0) = r;
        return;
    }
    case 0x0E:                       /* GetCRC16(init, ptr, len) */
        R(0) = crc16((u16)R(0), R(1), R(2));
        return;
    case 0x0F:                       /* IsDebugger */
        R(0) = 0;
        return;
    case 0x11:                       /* LZ77UnCompReadNormalWrite8bit */
        lz77(R(0), R(1), 0);
        return;
    }
    rt_fatal("BIOS call 0x%02X not done yet", n);
}
