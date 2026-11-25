/* rt.h - runtime of the recompiled Black 2 (memory, ROM, I/O, BIOS, ARM7). */
#ifndef PB2_RT_H
#define PB2_RT_H

#include "cpu.h"
#include <stddef.h>
#include <stdio.h>

#define DTCM_BASE  0x02FE0000u
#define ITCM_BASE  0x01FF8000u

/* log */
void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
extern int gTrace;

/* memory map (mem.c) */
void mem_init(void);

/* ROM (rom.c) */
extern const u8 *gRom;
extern size_t gRomSize;
void rom_load(const char *path);
u32 rom_r32(u32 off);
/* overlay table entry of the ROM */
typedef struct { u32 id, ram, size, bss, sinit, sinit_end, file, flags; } RomOverlay;
extern RomOverlay *gOverlays;
extern int gOverlayCount;
const u8 *rom_file(u32 file_id, u32 *size);
const u8 *overlay_image(int id, u32 *size);   /* decompressed */

/* time and interrupts (io.c) */
void io_init(void);
void time_advance_line(void);        /* one scanline */
void time_wait_irq(void);            /* until an interrupt is delivered */
void irq_check(void);                /* deliver a pending, enabled interrupt */
void irq_raise(u32 bits);
extern u32 gFrame;

/* ARM7 (arm7.c) */
void arm7_init(void);
void arm7_ipc_sync_write(u32 v);
void arm7_fifo_send(u32 v);
u32 arm7_fifo_recv(void);
int arm7_fifo_empty(void);
void arm7_step(void);                /* once per scanline */
void arm7_on_wait(void);             /* the ARM9 spins (SVC_WaitByLoop) */

/* card (card.c) */
void card_write_cmd(void);
u32 card_read_data(void);

/* cpu modes (cpu.c) */
void cpu_init(void);
void cpu_save(Cpu *c);
void cpu_restore(const Cpu *c);

/* Black 2 address -> the compiled game's, and back (dispatch.c) */
u32 pb2_addr(u32 b2);
u32 pb2_b2addr(u32 a);

/* threads (thread.c) */
void thread_init(void);
void thread_switch_context(u32 save_ctx, u32 load_ctx);
void thread_load_context(u32 ctx);

#endif
