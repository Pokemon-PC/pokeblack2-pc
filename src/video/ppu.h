/*
 * ppu_pc.h - software renderer of the DS 2D engines (see ppu_pc.c).
 */
#ifndef PHG_PPU_PC_H
#define PHG_PPU_PC_H

/* Draws both screens from the current DS video state into the
 * pc_gfx_framebuffer() buffers, running H-Blank DMA and the H-Blank
 * interrupt between scanlines. */
void pc_ppu_render_frame(void);
/* The same, one scanline at a time (begin at line 0, line 0..191, end). */
void pc_ppu_begin_frame(void);
void pc_ppu_render_line(int line);
void pc_ppu_end_frame(void);

#endif
