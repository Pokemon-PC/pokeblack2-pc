/* card.c - the game card bus seen from the ARM9 (KEY2 "main data" mode):
 * 0xB7 reads 0x200 bytes of the ROM, 0xB8 returns the chip ID. */
#include "rt.h"

#include <string.h>

#define REG32(a) (*(volatile u32 *)(uintptr_t)(a))
#define REG8(a)  (*(volatile u8 *)(uintptr_t)(a))

static u8 sBuf[0x4000];
static u32 sLen, sPos;

void dma_trigger_card(void);

void card_write_cmd(void) {
    u8 cmd[8];
    for (int i = 0; i < 8; i++) {
        cmd[i] = REG8(0x040001A8 + i);
    }
    u32 ctrl = REG32(0x040001A4);
    u32 bs = (ctrl >> 24) & 7;
    sLen = bs == 0 ? 0 : bs == 7 ? 4 : (0x100u << bs);
    sPos = 0;
    memset(sBuf, 0xFF, sizeof sBuf);
    if (cmd[0] == 0xB7) {
        u32 addr = (u32)cmd[1] << 24 | (u32)cmd[2] << 16 | (u32)cmd[3] << 8 | cmd[4];
        for (u32 i = 0; i < sLen; i++) {
            u32 a = addr + i;
            if (a < 0x8000) {
                a = 0x8000 + (a & 0x1FF);
            }
            sBuf[i] = a < gRomSize ? gRom[a] : 0xFF;
        }
    } else if (cmd[0] == 0xB8) {
        u32 id = 0x00003FC2;          /* chip ID: Macronix, 512 MiB-ish */
        memcpy(sBuf, &id, 4);
    } else {
        static int said;
        if (said++ < 5) {
            rt_log("card command %02X not emulated", cmd[0]);
        }
    }
    if (sLen == 0) {
        REG32(0x040001A4) &= ~0x80800000u;
        return;
    }
    REG32(0x040001A4) |= 0x00800000;       /* data ready */
    dma_trigger_card();
}

u32 card_read_data(void) {
    u32 v = 0;
    if (sPos < sLen) {
        memcpy(&v, sBuf + sPos, 4);
        sPos += 4;
    }
    if (sPos >= sLen) {
        REG32(0x040001A4) &= ~0x80800000u;  /* done */
        if (REG32(0x040001A0) & 0x4000) {
            irq_raise(1u << 19);
        }
    }
    return v;
}
