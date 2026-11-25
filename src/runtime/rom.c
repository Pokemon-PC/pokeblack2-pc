/* rom.c - the player's ROM: file system table, overlays (BLZ decompression). */
#include "rt.h"

#include <stdlib.h>
#include <string.h>

const u8 *gRom;
size_t gRomSize;
RomOverlay *gOverlays;
int gOverlayCount;
static u8 **sOverlayImages;
static u32 *sOverlayImageSize;

static u32 r32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

u32 rom_r32(u32 off) { return off + 4 <= gRomSize ? r32(gRom + off) : 0; }

void rom_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        rt_fatal("cannot open the ROM %s", path);
    }
    fseek(f, 0, SEEK_END);
    gRomSize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(gRomSize);
    if (!buf || fread(buf, 1, gRomSize, f) != gRomSize) {
        rt_fatal("cannot read the ROM");
    }
    fclose(f);
    gRom = buf;
    if (memcmp(gRom + 0x0C, "IREO", 4) != 0) {
        rt_log("warning: game code %.4s (expected IREO, Pokemon Black 2 USA/Europe)", (const char *)gRom + 0x0C);
    }
    u32 ovt = r32(gRom + 0x50), ovsz = r32(gRom + 0x54);
    gOverlayCount = (int)(ovsz / 32);
    gOverlays = calloc((size_t)gOverlayCount, sizeof *gOverlays);
    sOverlayImages = calloc((size_t)gOverlayCount, sizeof *sOverlayImages);
    sOverlayImageSize = calloc((size_t)gOverlayCount, sizeof *sOverlayImageSize);
    for (int i = 0; i < gOverlayCount; i++) {
        memcpy(&gOverlays[i], gRom + ovt + 32 * i, 32);
    }
    rt_log("ROM %.12s %.4s, %u MiB, %d overlays", (const char *)gRom, (const char *)gRom + 0x0C,
           (unsigned)(gRomSize >> 20), gOverlayCount);
}

const u8 *rom_file(u32 id, u32 *size) {
    u32 fat = r32(gRom + 0x48);
    u32 start = r32(gRom + fat + 8 * id), end = r32(gRom + fat + 8 * id + 4);
    if (size) {
        *size = end - start;
    }
    return gRom + start;
}

/* Nintendo's backward LZ ("BLZ") used for the ARM9 binary and overlays. */
static void blz_decompress(u8 *buf, u32 len, u32 out_len) {
    u32 top = r32(buf + len - 8), inc = r32(buf + len - 4);
    u32 enc_len = top & 0xFFFFFF, hdr_len = top >> 24;
    (void)inc;
    u8 *src = buf + len - hdr_len;
    u8 *src_end = buf + len - enc_len;
    u8 *dst = buf + out_len;
    while (src > src_end) {
        u8 flags = *--src;
        for (int i = 0; i < 8 && src > src_end; i++, flags <<= 1) {
            if (flags & 0x80) {
                u8 hi = *--src, lo = *--src;
                u32 disp = (((u32)(hi & 0x0F) << 8) | lo) + 3, n = (hi >> 4) + 3;
                while (n--) {
                    dst--;
                    *dst = dst[disp];
                }
            } else {
                *--dst = *--src;
            }
        }
    }
}

const u8 *overlay_image(int id, u32 *size) {
    if (id < 0 || id >= gOverlayCount) {
        return NULL;
    }
    if (!sOverlayImages[id]) {
        RomOverlay *o = &gOverlays[id];
        u32 fsize;
        const u8 *f = rom_file(o->file, &fsize);
        u8 *img = calloc(1, o->size + 16);
        if (o->flags & 0x01000000) {
            u32 clen = o->flags & 0xFFFFFF;
            memcpy(img, f, clen);
            blz_decompress(img, clen, o->size);
        } else {
            memcpy(img, f, fsize < o->size ? fsize : o->size);
        }
        sOverlayImages[id] = img;
        sOverlayImageSize[id] = o->size;
    }
    if (size) {
        *size = sOverlayImageSize[id];
    }
    return sOverlayImages[id];
}
