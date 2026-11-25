/*
 * video.h - what the 2D/3D renderers (ppu.c, vram.c, gpu3d.c, taken from the
 * HeartGold PC port) need from the runtime: DS types, and the few hooks the
 * HeartGold port provided through its NitroSDK layer. Here the game code is
 * recompiled, so the renderers read the DS state the game left at its native
 * addresses (I/O, palettes, VRAM, OAM) after each frame.
 */
#ifndef PB2_VIDEO_H
#define PB2_VIDEO_H

#include <stdint.h>

#include "cpu.h"

typedef int BOOL;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef volatile u32 REGType32v;
typedef u32 OSIrqMask;
#define OS_IE_H_BLANK (1u << 1)

/* H-Blank DMA and the H-Blank interrupt run per scanline in io.c, so the
 * renderer does not replay them (no channel, no dispatch). */
typedef struct PcDmaHBlank {
    u32  src, dest, ctrl;
    BOOL active;
} PcDmaHBlank;
static inline const PcDmaHBlank *pc_dma_hblank_channel(u32 dmaNo) { (void)dmaNo; return 0; }
static inline void pc_nitro_os_dispatch_irq(OSIrqMask irq) { (void)irq; }
static inline void pc_hw_set_vcount(u16 line) { (void)line; }

#define PC_SCREEN_W 256
#define PC_SCREEN_H 192
typedef enum { PC_SCREEN_TOP = 0, PC_SCREEN_BOTTOM = 1 } PcScreen;
uint32_t *pc_gfx_framebuffer(PcScreen screen);   /* 0xAABBGGRR, as HGSS */

void pc_gx_fifo_submit(const u32 *words, u32 count);

/* screen.c */
void video_init(void);
void video_frame(void);          /* V-Blank: render, show, input, pace */
u16 video_keys(void);            /* KEYINPUT (active low) */

#endif
