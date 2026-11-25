/*
 * arm7.c - the ARM7 side, reproduced: what the ARM7 firmware answers to the
 * game's PXI messages (IPC FIFO). A message is a word: bits 0-4 tag, bit 5
 * error, bits 6-31 data (NitroSDK/TwlSDK pxi). Services are added as the boot
 * needs them; every unknown message is logged.
 */
#include "rt.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REG16(a) (*(volatile u16 *)(uintptr_t)(a))
#define REG32(a) (*(volatile u32 *)(uintptr_t)(a))

static u32 sRecv[64];
static int sHead, sCount;
static u32 sA7Status = 5;            /* handshake: counts down to 0 */

static void sync_update(void) {
    u16 s = REG16(0x04000180);
    REG16(0x04000180) = (u16)((s & ~0xFu) | (sA7Status & 0xF));
}

void arm7_init(void) {
    sync_update();
    /* OSSystemWork.pxiHandleChecker[ARM7] (0x027FFF8C): one bit per PXI tag
     * whose ARM7 service is ready; the ARM9 waits for them
     * (PXI_IsCallbackReady, FUN_0207CA48). Every service answers here. */
    *(volatile u32 *)(uintptr_t)0x027FFF8C = 0xFFFFFFFFu;
}

void arm7_ipc_sync_write(u32 v) {
    u32 a9 = (v >> 8) & 0xF;
    /* PXI_InitFifo: the ARM7 mirrors the ARM9 and counts down to 0 */
    if (sA7Status > 0 && a9 == sA7Status) {
        sA7Status--;
    }
    sync_update();
    if ((v & 0x2000) && gTrace) {
        rt_log("ARM9 -> ARM7 sync IRQ (value %X)", a9);
    }
}

static void reply(u32 w) {
    if (sCount < 64) {
        sRecv[(sHead + sCount++) % 64] = w;
    }
    REG16(0x04000184) &= ~0x0100;                    /* receive FIFO not empty */
    if (REG16(0x04000184) & 0x0400) {
        irq_raise(1u << 18);
    }
}

u32 arm7_fifo_recv(void) {
    if (!sCount) {
        REG16(0x04000184) |= 0x4000;                 /* error: read while empty */
        return 0;
    }
    u32 w = sRecv[sHead];
    sHead = (sHead + 1) % 64;
    if (--sCount == 0) {
        REG16(0x04000184) |= 0x0100;                 /* empty */
    }
    return w;
}

int arm7_fifo_empty(void) {
    return sCount == 0;
}

#define TAG_PM 8

static void send_reply(u32 tag, u32 data) {
    reply(tag | (data << 6));
}

/* PM (power management, NitroSDK pm.c): words with bit 25 = first, bit 24 =
 * last, command in bits 8-15; the answer is (command << 8) | result, except
 * register reads (0x65 + reg -> 0x70 + reg, value). */
static void pm_command(u32 data) {
    static u32 cmd;
    if (data & 0x02000000) {
        cmd = (data >> 8) & 0xFF;
    }
    if (!(data & 0x01000000)) {
        return;
    }
    if (cmd == 0x65) {
        u32 reg = data & 0xFF;
        static const u8 regs[5] = { 0x0D, 0x00, 0x01, 0x00, 0x03 };   /* control, battery ok, mic amp, gain, backlight */
        send_reply(TAG_PM, ((0x70 + (reg & 7)) << 8) | (reg < 5 ? regs[reg] : 0));
    } else {
        send_reply(TAG_PM, cmd << 8);            /* success */
    }
}

/*
 * Card backup (save memory), PXI tag 11 (NitroSDK card_common): the ARM9
 * sends a command number, and at init the address of its shared
 * CARDiCommandArg {result, type, id, src, dst, len, spec...}; the ARM7 runs
 * the command on the save chip, writes result and answers with the error bit
 * set ("done"). Here the chip is a 512 KiB file, pokeblack2.sav by default
 * (PB2_SAVE).
 */
#define TAG_FS 11
#define SAVE_SIZE (512u * 1024u)
static u8 *sSave;
static u32 sCardArg;
static const char *save_path(void) {
    /* PB2_SAVE=file, else pokeblack2.sav next to the exe */
    const char *e = getenv("PB2_SAVE");
    if (e) {
        return e;
    }
    static char p[1100];
    if (!p[0]) {
        const char *config_exe_dir(void);
        snprintf(p, sizeof p, "%s\\" PB2_FILE_BASE ".sav", config_exe_dir());
    }
    return p;
}
static void save_load(void) {
    if (sSave) {
        return;
    }
    sSave = malloc(SAVE_SIZE);
    memset(sSave, 0xFF, SAVE_SIZE);
    FILE *f = fopen(save_path(), "rb");
    if (f) {
        size_t n = fread(sSave, 1, SAVE_SIZE, f);
        fclose(f);
        rt_log("save: %s (%u bytes)", save_path(), (unsigned)n);
    }
}
/* the file is written when the game has stopped writing for half a second
 * (a save is thousands of page writes) and at exit; through a temporary file
 * so that a crash never leaves half a save */
static int sSaveDirty;
static u32 sSaveDirtyFrame;
static void save_write_file(void) {
    if (!sSaveDirty) {
        return;
    }
    sSaveDirty = 0;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", save_path());
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        rt_log("save: cannot write %s", tmp);
        return;
    }
    size_t n = fwrite(sSave, 1, SAVE_SIZE, f);
    if (fclose(f) != 0 || n != SAVE_SIZE) {
        rt_log("save: write error on %s", tmp);
        return;
    }
    remove(save_path());
    if (rename(tmp, save_path()) != 0) {
        rt_log("save: cannot rename %s", tmp);
    }
}
static void save_flush(void) {
    static int hooked;
    if (!hooked) {
        hooked = 1;
        atexit(save_write_file);
    }
    sSaveDirty = 1;
    sSaveDirtyFrame = gFrame;
}
static void save_idle(void) {
    if (sSaveDirty && gFrame - sSaveDirtyFrame >= 30) {
        save_write_file();
    }
}
static void card_backup_command(u32 cmd) {
    save_load();
    volatile u32 *a = (volatile u32 *)(uintptr_t)sCardArg;
    u32 result = 0;
    if (a) {
        u32 src = a[3], dst = a[4], len = a[5];
        static int said;
        if (said++ < 40) {
            rt_log("card backup command %u src %08X dst %08X len %X", cmd, src, dst, len);
        }
        /* TwlSDK CARD_REQ_*: 0 init, 1 ack, 2 identify, 3 read id, 4/5 rom,
         * 6 read, 7 write, 8 program, 9 verify, 10 erase page, 11 erase
         * sector, 12 erase chip, 13 read status, 14 write status, 15 erase
         * subsector */
        switch (cmd) {
        case 6:                                 /* read backup: chip src -> RAM dst */
            for (u32 i = 0; i < len; i++) {
                wr8(dst + i, sSave[(src + i) % SAVE_SIZE]);
            }
            break;
        case 7:
        case 8:                                 /* write / program: RAM src -> chip dst */
            for (u32 i = 0; i < len; i++) {
                sSave[(dst + i) % SAVE_SIZE] = rd8(src + i);
            }
            save_flush();
            break;
        case 9:                                 /* verify */
            for (u32 i = 0; i < len; i++) {
                if (sSave[(dst + i) % SAVE_SIZE] != rd8(src + i)) {
                    result = 1;
                    break;
                }
            }
            break;
        case 10:
        case 11:
        case 15:                                /* erase */
            for (u32 i = 0; i < len; i++) {
                sSave[(dst + i) % SAVE_SIZE] = 0xFF;
            }
            save_flush();
            break;
        case 12:
            memset(sSave, 0xFF, SAVE_SIZE);
            save_flush();
            break;
        case 13:                                /* read status: ready */
            if (dst >= 0x02000000) {
                wr8(dst, 0);
            }
            break;
        default:                                /* init, ack, identify, write status */
            break;
        }
        a[0] = result;
    }
    reply(TAG_FS | 0x20 | (cmd << 6));
}
static void fs_word(u32 w) {
    static int expect_arg;
    u32 data = w >> 6;
    if (expect_arg) {
        expect_arg = 0;
        sCardArg = data;
        card_backup_command(0);
        return;
    }
    if (data == 0) {                            /* init: the next word is the argument address */
        expect_arg = 1;
        return;
    }
    card_backup_command(data);
}

/*
 * NVRAM (the firmware flash, PXI tag 4, NitroSDK spi): six 16-bit words
 * {0x23 read | addr >> 16, addr, size >> 16, size, dst >> 16, dst}, bit 25 on
 * the first, bit 24 on the last, index in bits 16-23; answer 0 = success.
 * The firmware is synthesized: header + user settings (two copies, CRC16).
 */
#define TAG_NVRAM 4
#define FW_SIZE (256u * 1024u)
static u8 *sFw;
static u16 crc16(u16 crc, const u8 *p, u32 n) {
    static const u16 t[8] = { 0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001 };
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            int carry = crc & 1;
            crc >>= 1;
            if (carry) {
                crc ^= (u16)(t[j] << (7 - j));
            }
        }
    }
    return crc;
}
static void fw_build(void) {
    if (sFw) {
        return;
    }
    sFw = calloc(1, FW_SIZE);
    sFw[0x20] = 0xC0;                           /* user settings at 0x7FC0 * 8 = 0x3FE00 */
    sFw[0x21] = 0x7F;
    for (int copy = 0; copy < 2; copy++) {
        u8 *u = sFw + 0x3FE00 + copy * 0x100;
        u[0x00] = 5;                            /* version */
        u[0x02] = 11;                           /* favourite colour */
        u[0x03] = 1;                            /* birthday month */
        u[0x04] = 1;                            /* birthday day */
        static const char name[] = "Player";
        for (int i = 0; name[i]; i++) {
            u[0x06 + 2 * i] = (u8)name[i];
        }
        u[0x1A] = 6;                            /* nickname length */
        /* touch calibration: ADC 0x200 at pixel (32, 32), 0xE00 at (224, 160);
         * screen.c converts mouse positions with the same line */
        u[0x58] = 0x00; u[0x59] = 0x02;
        u[0x5A] = 0x00; u[0x5B] = 0x02;
        u[0x5C] = 32;   u[0x5D] = 32;
        u[0x5E] = 0x00; u[0x5F] = 0x0E;
        u[0x60] = 0x00; u[0x61] = 0x0E;
        u[0x62] = 224;  u[0x63] = 160;
        u[0x64] = 0x01 | 0x08;                  /* language English, ... */
        u[0x65] = 0x0C;                         /* backlight etc. */
        u[0x70] = (u8)copy;                     /* update counter */
        u16 c = crc16(0xFFFF, u, 0x70);
        u[0x72] = (u8)c;
        u[0x73] = (u8)(c >> 8);
    }
}
static void nvram_word(u32 data) {
    static u16 w[8];
    if (data & 0x02000000) {
        memset(w, 0, sizeof w);
    }
    u32 idx = (data >> 16) & 0xFF;
    if (idx < 8) {
        w[idx] = (u16)data;
    }
    if (!(data & 0x01000000)) {
        return;
    }
    fw_build();
    u32 cmd = w[0] >> 8;
    u32 addr = ((u32)(w[0] & 0xFF) << 16) | w[1];
    u32 size = ((u32)w[2] << 16) | w[3];
    u32 dst = ((u32)w[4] << 16) | w[5];
    static int said;
    if (said++ < 200) {
        rt_log("NVRAM command %02X words %04X %04X %04X %04X %04X %04X", cmd, w[0], w[1], w[2], w[3], w[4], w[5]);
    }
    if (cmd == 0x23) {                          /* read: addr, size, dst */
        for (u32 i = 0; i < size; i++) {
            wr8(dst + i, sFw[(addr + i) % FW_SIZE]);
        }
    } else if (cmd == 0x22) {                   /* read status register -> byte at w0:w1:w2 */
        u32 sdst = ((u32)(w[0] & 0xFF) << 24) | ((u32)w[1] << 8) | (w[2] >> 8);
        wr8(sdst, 0);                           /* not busy, write disabled */
    } else if (cmd == 0x24 || cmd == 0x25) {    /* page write / program: addr, size (16), src */
        u32 n = w[2], src = ((u32)w[3] << 16) | w[4];
        for (u32 i = 0; i < n; i++) {
            sFw[(addr + i) % FW_SIZE] = (u8)rd8(src + i);
        }
    }
    send_reply(TAG_NVRAM, 0);
}

/*
 * RTC (PXI tag 5): command in bits 8-14; the ARM7 updates the date/time in
 * the system area (0x027FFDE8: year, month, day, weekday; 0x027FFDEC: hour,
 * minute, second, BCD) and answers (command << 8) | 0.
 */
#define TAG_RTC 5
static u8 bcd(int v) {
    return (u8)(((v / 10) << 4) | (v % 10));
}
static void rtc_command(u32 data) {
    u32 cmd = (data >> 8) & 0x7F;
    time_t now = time(NULL);
    /* PB2_RTC=YYYY-MM-DD-hh:mm (tests): a fixed start that advances with the
     * frames, so that a run is the same every time */
    static int fixed = -1;
    static time_t base;
    if (fixed < 0) {
        const char *e = getenv("PB2_RTC");
        struct tm b;
        memset(&b, 0, sizeof b);
        fixed = e && sscanf(e, "%d-%d-%d-%d:%d", &b.tm_year, &b.tm_mon, &b.tm_mday, &b.tm_hour, &b.tm_min) == 5;
        if (fixed) {
            b.tm_year -= 1900;
            b.tm_mon -= 1;
            b.tm_isdst = -1;
            base = mktime(&b);
        }
    }
    if (fixed) {
        now = base + gFrame / 60;
    }
    struct tm *t = localtime(&now);
    volatile u8 *d = (volatile u8 *)(uintptr_t)0x027FFDE8;
    d[0] = bcd(t->tm_year % 100);
    d[1] = bcd(t->tm_mon + 1);
    d[2] = bcd(t->tm_mday);
    d[3] = (u8)t->tm_wday;
    d[4] = bcd(t->tm_hour) | (t->tm_hour >= 12 ? 0x40 : 0);
    d[5] = bcd(t->tm_min);
    d[6] = bcd(t->tm_sec);
    d[7] = 0;
    send_reply(TAG_RTC, cmd << 8);
}

/*
 * Touch panel (PXI tag 6): bit 25/24 first/last, command in bits 8-14;
 * answer bit 24 | (command << 8) | 0. Sampling writes the raw value at
 * 0x027FFFAA (x 12 bits, y 12 bits, bit 24 touch, bits 25-26 validity).
 */
#define TAG_TP 6
u32 gTouchRaw;                                  /* set by the input layer */
static void tp_command(u32 data) {
    u32 cmd = (data >> 8) & 0x7F;
    if (!(data & 0x01000000)) {
        return;
    }
    volatile u16 *t = (volatile u16 *)(uintptr_t)0x027FFFAA;
    t[0] = (u16)gTouchRaw;
    t[1] = (u16)(gTouchRaw >> 16);
    send_reply(TAG_TP, 0x01000000 | (cmd << 8));
}

/* Sound (PXI tag 7): the ARM9 sends the address of a linked list of SND
 * commands; the driver in src/audio/snd.c runs them. Alarms come back on the
 * same tag. */
#define TAG_SOUND 7
void snd_commands(u32 list);
void arm7_snd_reply(u32 data) {
    send_reply(TAG_SOUND, data);
}

static int sTagCount[32];
void arm7_fifo_send(u32 w) {
    u32 tag = w & 0x1F, data = w >> 6;
    if (sTagCount[tag]++ < 8) {
        rt_log("PXI ARM9->ARM7 tag %2u data %07X%s", tag, data, (w & 0x20) ? " (err)" : "");
    }
    switch (tag) {
    case TAG_PM:
        pm_command(data);
        break;
    case TAG_FS:
        fs_word(w);
        break;
    case TAG_TP:
        tp_command(data);
        break;
    case TAG_SOUND:
        snd_commands(data);
        break;
    case TAG_NVRAM:
        nvram_word(data);
        break;
    case TAG_RTC:
        rtc_command(data);
        break;
    }
}

/*
 * Shared-memory handshake (FUN_02079F5C, run by both CPUs at boot): a 4-byte
 * struct {counter|0x80, echo, done, ack}. The side "A" (r0 = 0) writes the
 * counter and waits for its echo; side "B" (r0 = 1) echoes, sums the values
 * and, past 0x300, sets done and waits for ack. The ARM9 plays A then B: the
 * ARM7 plays the other side each time the ARM9 waits (SVC_WaitByLoop).
 */
static void handshake_step(u32 lr, u32 p) {
    volatile u8 *s = (volatile u8 *)(uintptr_t)p;
    static u32 sum, counter, last;
    if (p != last) {
        last = p;
        sum = 0;
        counter = 0;
    }
    switch (lr & ~1u) {
    case 0x02079F7E:                 /* ARM9 is A, waits for the echo: ARM7 is B */
        if (s[1] != s[0]) {
            s[1] = s[0];
            sum += s[0];
        }
        if (sum >= 0x300) {
            s[3] = 0;
            s[2] = 1;
        }
        break;
    case 0x02079FBE:                 /* ARM9 is B, waits for a new counter: ARM7 is A */
        if (s[1] == s[0]) {
            s[0] = (u8)(0x80 | (++counter & 0xF));
        }
        break;
    case 0x02079FDA:                 /* ARM9 (B) waits for the ack */
        s[3] = 1;
        break;
    }
}

void arm7_on_wait(void) {
    u32 lr = pb2_b2addr(LR);                         /* compared with Black 2 addresses */
    if (lr >= 0x02079F5C && lr < 0x02079FE4) {
        handshake_step(lr, R(5));
        return;
    }
    static int said;
    if (!said++) {
        rt_log("WaitByLoop from 0x%08X (no ARM7 counterpart known)", lr);
    }
    time_advance_line();
}

void arm7_step(void) {
    save_idle();
    REG16(0x04000184) |= 0x0001;                     /* our send FIFO is always empty */
    if (!sCount) {
        REG16(0x04000184) |= 0x0100;
    }
}
