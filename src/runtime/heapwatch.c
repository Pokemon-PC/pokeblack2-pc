/* PB2_HEAPWATCH=n: every n frames, the state of the game's heaps (NitroSystem
 * expanded heaps behind the game's heap table at 0x0214181C in Black 2):
 * per heap, its allocation count, total and largest free block, number of
 * used blocks. Finds leaks: a heap whose free space only goes down. */
#include "rt.h"

#include <stdlib.h>

static void heap_report(int detail);

/* called when the game gives up on an allocation: every heap, with blocks */
void heapwatch_dump(void) {
    heap_report(1);
}

void heapwatch_frame(u32 frame) {
    static int init;
    static u32 every;
    if (!init) {
        init = 1;
        every = getenv("PB2_HEAPWATCH") ? (u32)atol(getenv("PB2_HEAPWATCH")) : 0;
    }
    if (!every || frame % every) {
        return;
    }
    heap_report(0);
}

static void heap_report(int detail) {
    u32 sys = pb2_addr(0x0214181C);
    u32 table = rd32(sys), map = rd32(sys + 4);
    u32 count = rd32(sys + 8) & 0xFFFF;
    if (!table || !map) {
        return;
    }
    for (u32 id = 0; id < count && id < 0x80; id++) {
        u32 idx = rd8(map + id);
        if (idx == 0xFF) {
            continue;
        }
        u32 h = rd32(table + idx * 16);
        if (!h || rd32(h) != 0x45585048) {           /* 'EXPH' */
            continue;
        }
        u32 freeTotal = 0, freeMax = 0, freeN = 0, usedN = 0, usedTotal = 0;
        for (u32 b = rd32(h + 0x24); b && freeN < 100000; b = rd32(b + 0xC)) {
            u32 sz = rd32(b + 4);
            freeTotal += sz;
            if (sz > freeMax) {
                freeMax = sz;
            }
            freeN++;
        }
        for (u32 b = rd32(h + 0x2C); b && usedN < 100000; b = rd32(b + 0xC)) {
            usedTotal += rd32(b + 4);
            usedN++;
        }
        rt_log("heap %2u: %08X-%08X free %7u (largest %7u, %u blocks) used %7u in %u blocks, count %u",
               id, rd32(h + 0x18), rd32(h + 0x1C), freeTotal, freeMax, freeN, usedTotal, usedN,
               rd16(table + idx * 16 + 0xC));
        for (u32 b = rd32(h + 0x2C), n = 0; detail && b && n < 400; b = rd32(b + 0xC), n++) {
            /* the game's header after NitroSystem's: heap id, magic 0x194E, then
             * (from 0x0203A2E0) the size asked and the caller */
            char tag[0x19];
            for (u32 i = 0; i < 0x18; i++) {
                u8 c = (u8)rd8(b + 0x14 + i);
                tag[i] = c >= 0x20 && c < 0x7F ? (char)c : '.';
            }
            tag[0x18] = 0;
            rt_log("    used %08X size %6u | id %04X magic %04X | %s", b + 0x10, rd32(b + 4),
                   rd16(b + 0x10), rd16(b + 0x12), tag);
        }
    }
}
