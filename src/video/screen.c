/*
 * screen.c - the window, keyboard and pacing.
 *
 * At each V-Blank (io.c, line 192) the two screens are drawn by the HeartGold
 * port's renderers from the DS state, shown stacked in an SDL window, the
 * keyboard is read for the next frame and the game is held to 60 frames per
 * second.
 *
 * Env: PB2_HEADLESS=1   no window, no pacing (tests)
 *      PB2_FAST=1       no pacing
 *      PB2_SHOT=N:file  write both screens to file (BMP) at frame N, then quit
 *      PB2_SHOTS=dir    write a BMP every 60 frames into dir (dir/fNNNNN.bmp)
 *      PB2_SHOTS_EVERY=n   period of PB2_SHOTS in frames (default 60)
 *      PB2_KEYS=f:keys,...  scripted input: from frame f hold keys
 *      PB2_CTRL=dir     step-by-step control through dir/cmd.txt (see ctrl_keys)
 *                       (A B L R S=start E=select U D <  > X Y), e.g. 600:S,620:
 * Keys: X = A, Z = B, S = X, A = Y, Q = L, W = R, Return = Start,
 *       Right Shift / Backspace = Select, arrows; mouse on the bottom screen = touch.
 */
#define SDL_MAIN_HANDLED
#include "video.h"
#include "ppu.h"
#include "../runtime/rt.h"
#include "../runtime/version.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t sFb[2][PC_SCREEN_W * PC_SCREEN_H];
static SDL_Window *sWin;
static SDL_Renderer *sRen;
static SDL_Texture *sTex;
static int sHeadless, sFast;
static u32 sShotFrame = 0xFFFFFFFFu;
static const char *sShotPath, *sShotsDir;
static u32 sShotsEvery = 60;
static u16 sKeys = 0x03FF;           /* KEYINPUT, active low */
static u8 sXY = 0x7F;                /* EXTKEYIN: X, Y, pen (active low) */
static u32 sFrames;
static u64 sNext;
extern u32 gTouchRaw;

uint32_t *pc_gfx_framebuffer(PcScreen screen) {
    return sFb[screen == PC_SCREEN_BOTTOM];
}

u16 video_keys(void) {
    return sKeys;
}

void video_init(void) {
    sHeadless = getenv("PB2_HEADLESS") != NULL;
    sFast = sHeadless || getenv("PB2_FAST") != NULL;
    const char *shot = getenv("PB2_SHOT");
    if (shot) {
        sShotFrame = (u32)strtoul(shot, NULL, 10);
        const char *c = strchr(shot, ':');
        sShotPath = c ? c + 1 : "pb2_shot.bmp";
    }
    sShotsDir = getenv("PB2_SHOTS");
    if (getenv("PB2_SHOTS_EVERY")) {
        sShotsEvery = (u32)strtoul(getenv("PB2_SHOTS_EVERY"), NULL, 10);
        if (!sShotsEvery) {
            sShotsEvery = 60;
        }
    }
    SDL_SetMainReady();
    if (sHeadless) {
        return;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        rt_fatal("SDL: %s", SDL_GetError());
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    sWin = SDL_CreateWindow(PB2_GAME_NAME " (PC)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            PC_SCREEN_W * 2, PC_SCREEN_H * 4, SDL_WINDOW_RESIZABLE);
    if (!sWin) {
        rt_fatal("SDL window: %s", SDL_GetError());
    }
    sRen = SDL_CreateRenderer(sWin, -1, SDL_RENDERER_ACCELERATED);
    if (!sRen) {
        sRen = SDL_CreateRenderer(sWin, -1, 0);
    }
    SDL_RenderSetLogicalSize(sRen, PC_SCREEN_W, PC_SCREEN_H * 2);
    sTex = SDL_CreateTexture(sRen, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, PC_SCREEN_W, PC_SCREEN_H * 2);
}

static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        rt_log("screenshot: cannot write %s", path);
        return;
    }
    const int w = PC_SCREEN_W, h = PC_SCREEN_H * 2;
    u32 size = 54 + w * h * 3;
    u8 hd[54] = { 'B', 'M' };
    *(u32 *)(hd + 2) = size;
    *(u32 *)(hd + 10) = 54;
    *(u32 *)(hd + 14) = 40;
    *(s32 *)(hd + 18) = w;
    *(s32 *)(hd + 22) = h;
    *(u16 *)(hd + 26) = 1;
    *(u16 *)(hd + 28) = 24;
    fwrite(hd, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *row = sFb[y / PC_SCREEN_H] + (y % PC_SCREEN_H) * w;
        for (int x = 0; x < w; x++) {
            u8 px[3] = { (u8)(row[x] >> 16), (u8)(row[x] >> 8), (u8)row[x] };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

/* key letters of PB2_KEYS and PB2_CTRL: KEYINPUT (active low) and X/Y */
static void parse_keys(const char *p, const char *end, u16 *keys, u8 *xy) {
    for (; *p && p != end && *p != ','; p++) {
        switch (*p) {
        case 'A': *keys &= ~0x001; break;
        case 'B': *keys &= ~0x002; break;
        case 'E': *keys &= ~0x004; break;
        case 'S': *keys &= ~0x008; break;
        case '>': *keys &= ~0x010; break;
        case '<': *keys &= ~0x020; break;
        case 'U': *keys &= ~0x040; break;
        case 'D': *keys &= ~0x080; break;
        case 'R': *keys &= ~0x100; break;
        case 'L': *keys &= ~0x200; break;
        case 'X': *xy &= ~0x01; break;
        case 'Y': *xy &= ~0x02; break;
        }
    }
}

static u32 touch_raw(int x, int y) {
    /* raw ADC values matching the firmware calibration (arm7.c) */
    int ax = 0x200 + (x - 32) * 16, ay = 0x200 + (y - 32) * 24;
    u32 rx = (u32)(ax < 0 ? 0 : ax > 0xFFF ? 0xFFF : ax), ry = (u32)(ay < 0 ? 0 : ay > 0xFFF ? 0xFFF : ay);
    return rx | ry << 12 | 1u << 24;
}

/* PB2_CTRL=dir: the game is driven step by step (headless tests that go far
 * without replaying from frame 0). When it has nothing left to do it writes
 * dir/status.txt ("frame N") and waits for dir/cmd.txt, one command per line:
 *   k KEYS N        hold KEYS (PB2_KEYS letters, '-' = none) for N frames
 *   m KEYS N P [HOLD]  tap KEYS (6 frames) every P frames, N frames in all,
 *                   holding HOLD the whole time (walk while pressing A)
 *   t X Y N         touch the bottom screen at X,Y for N frames
 *   s NAME          screenshot to dir/NAME.bmp
 *   r               log the 2D registers of both screens and their sprites
 *   q               quit
 * The file is deleted once read. */
typedef struct { char op; u16 keys, hold; u8 xy; int x, y; u32 n, period; char name[64]; } CtrlCmd;
static const char *sCtrlDir;
static CtrlCmd sCtrl[4096];
static u32 sCtrlHead, sCtrlCount, sCtrlDone;

static void ctrl_load(void) {
    char path[512], line[256];
    snprintf(path, sizeof path, "%s/status.txt", sCtrlDir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "frame %u\n", sFrames);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/cmd.txt", sCtrlDir);
    while (!(f = fopen(path, "r"))) {
        SDL_Delay(20);
    }
    sCtrlHead = sCtrlCount = 0;
    while (fgets(line, sizeof line, f) && sCtrlCount < sizeof sCtrl / sizeof sCtrl[0]) {
        CtrlCmd c = { .op = line[0], .keys = 0x03FF, .hold = 0x03FF, .xy = 0x7F };
        char arg[64] = "", hold[64] = "";
        if (c.op == 'k' || c.op == 'm') {
            if (sscanf(line + 1, "%63s %u %u %63s", arg, &c.n, &c.period, hold) < 2) {
                continue;
            }
            parse_keys(arg, NULL, &c.keys, &c.xy);
            parse_keys(hold, NULL, &c.hold, &c.xy);
        } else if (c.op == 't') {
            if (sscanf(line + 1, "%d %d %u", &c.x, &c.y, &c.n) < 3) {
                continue;
            }
        } else if (c.op == 's') {
            sscanf(line + 1, "%63s", c.name);
        } else if (c.op != 'q' && c.op != 'r') {
            continue;
        }
        sCtrl[sCtrlCount++] = c;
    }
    fclose(f);
    remove(path);
}

static void ctrl_keys(void) {
    static int init;
    if (!init) {
        init = 1;
        sCtrlDir = getenv("PB2_CTRL");
        if (sCtrlDir) {
            char path[512];
            snprintf(path, sizeof path, "%s/pid.txt", sCtrlDir);
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "%d\n", (int)getpid());
                fclose(f);
            }
        }
    }
    if (!sCtrlDir) {
        return;
    }
    for (;;) {
        if (sCtrlHead >= sCtrlCount) {
            ctrl_load();
            continue;
        }
        CtrlCmd *c = &sCtrl[sCtrlHead];
        if (c->op == 'q') {
            rt_log("PB2_CTRL: quit at frame %u", sFrames);
            exit(0);
        }
        if (c->op == 'r') {
            for (int e = 0; e < 2; e++) {
                u32 b = e ? 0x04001000 : 0x04000000;
                rt_log("%s: DISPCNT %08X BGCNT %04X %04X %04X %04X BLDCNT %04X ALPHA %04X Y %02X "
                       "WIN0H %04X WIN0V %04X WININ %04X WINOUT %04X MASTER %04X",
                       e ? "sub " : "main", rd32(b), rd16(b + 8), rd16(b + 0xA), rd16(b + 0xC), rd16(b + 0xE),
                       rd16(b + 0x50), rd16(b + 0x52), rd16(b + 0x54), rd16(b + 0x40), rd16(b + 0x44),
                       rd16(b + 0x48), rd16(b + 0x4A), rd16(b + 0x6C));
                rt_log("      BG ofs %04X,%04X %04X,%04X %04X,%04X %04X,%04X",
                       rd16(b + 0x10), rd16(b + 0x12), rd16(b + 0x14), rd16(b + 0x16),
                       rd16(b + 0x18), rd16(b + 0x1A), rd16(b + 0x1C), rd16(b + 0x1E));
                u32 oam = e ? 0x07000400 : 0x07000000, shown = 0;
                for (int i = 0; i < 128; i++) {
                    u16 a0 = (u16)rd16(oam + i * 8), a1 = (u16)rd16(oam + i * 8 + 2), a2 = (u16)rd16(oam + i * 8 + 4);
                    if ((a0 & 0x0300) == 0x0200) {
                        continue;                              /* hidden */
                    }
                    if (shown++ < 24) {
                        rt_log("      obj %3d: %04X %04X %04X", i, a0, a1, a2);
                    }
                }
                rt_log("      %u sprites shown", shown);
            }
            rt_log("VRAMCNT %08X %08X %04X", rd32(0x04000240), rd32(0x04000244), rd16(0x04000248));
            sCtrlHead++;
            continue;
        }
        if (c->op == 's') {
            char path[512];
            snprintf(path, sizeof path, "%s/%s.bmp", sCtrlDir, c->name);
            write_bmp(path);
            sCtrlHead++;
            continue;
        }
        if (sCtrlDone >= c->n) {
            sCtrlDone = 0;
            sCtrlHead++;
            continue;
        }
        if (c->op == 'm') {
            sKeys &= c->hold;
        }
        if (c->op == 'k' || (c->op == 'm' && sCtrlDone % (c->period ? c->period : 30) < 6)) {
            sKeys &= c->keys;
            sXY &= c->xy;
        } else if (c->op == 't') {
            gTouchRaw = touch_raw(c->x, c->y);
            sXY &= ~0x40;
        }
        sCtrlDone++;
        return;
    }
}

/* PB2_KEYS script: "frame:keys,frame:keys" */
static void scripted_keys(void) {
    static const char *script;
    static int init;
    if (!init) {
        init = 1;
        script = getenv("PB2_KEYS");
    }
    if (!script) {
        return;
    }
    u16 keys = 0x03FF;
    u8 xy = 0x7F;
    for (const char *p = script; *p;) {
        u32 f = (u32)strtoul(p, (char **)&p, 10);
        if (*p == ':') {
            p++;
        }
        u16 k = 0x03FF;
        u8 e = 0x7F;
        parse_keys(p, NULL, &k, &e);
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
        if (sFrames >= f) {
            keys = k;
            xy = e;
        }
    }
    sKeys &= keys;
    sXY &= xy;
}

static void read_input(void) {
    sKeys = 0x03FF;
    sXY = 0x7F;
    gTouchRaw = 0;
    if (!sHeadless) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                rt_log("window closed");
                exit(0);
            }
        }
        const u8 *k = SDL_GetKeyboardState(NULL);
        static const struct { SDL_Scancode sc; u16 bit; } map[] = {
            { SDL_SCANCODE_X, 0x001 }, { SDL_SCANCODE_Z, 0x002 }, { SDL_SCANCODE_RSHIFT, 0x004 },
            { SDL_SCANCODE_BACKSPACE, 0x004 }, { SDL_SCANCODE_RETURN, 0x008 }, { SDL_SCANCODE_RIGHT, 0x010 },
            { SDL_SCANCODE_LEFT, 0x020 }, { SDL_SCANCODE_UP, 0x040 }, { SDL_SCANCODE_DOWN, 0x080 },
            { SDL_SCANCODE_W, 0x100 }, { SDL_SCANCODE_Q, 0x200 },
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            if (k[map[i].sc]) {
                sKeys &= ~map[i].bit;
            }
        }
        if (k[SDL_SCANCODE_S]) {
            sXY &= ~0x01;
        }
        if (k[SDL_SCANCODE_A]) {
            sXY &= ~0x02;
        }
        /* touch: mouse button held over the bottom screen */
        int mx, my;
        if (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) {
            float lx, ly;
            SDL_RenderWindowToLogical(sRen, mx, my, &lx, &ly);
            int x = (int)lx, y = (int)ly - PC_SCREEN_H;
            if (x >= 0 && x < PC_SCREEN_W && y >= 0 && y < PC_SCREEN_H) {
                gTouchRaw = touch_raw(x, y);
                sXY &= ~0x40;
            }
        }
    }
    scripted_keys();
    ctrl_keys();
    *(volatile u16 *)(uintptr_t)0x04000130 = sKeys;
    /* HW_BUTTON_XY_BUF as the ARM7 writes it: X bit 10, Y bit 11, debug bit 13,
     * lid closed bit 15 (active low like KEYINPUT, ORed with it by PAD_Read) */
    u16 xy = (u16)(((sXY & 0x01) ? 0x0400 : 0) | ((sXY & 0x02) ? 0x0800 : 0) | 0x2000);
    *(volatile u16 *)(uintptr_t)0x027FFFA8 = xy;
}

static void pace(void) {
    if (sFast) {
        return;
    }
    u64 freq = SDL_GetPerformanceFrequency();
    u64 period = freq * 1001 / 60000;             /* ~59.94 Hz, close to the DS 59.83 */
    u64 now = SDL_GetPerformanceCounter();
    if (!sNext || now > sNext + period * 4) {
        sNext = now;
    }
    sNext += period;
    while ((now = SDL_GetPerformanceCounter()) < sNext) {
        u64 left = (sNext - now) * 1000 / freq;
        if (left > 1) {
            SDL_Delay((u32)(left - 1));
        }
    }
}

void video_frame(void) {
    void heapwatch_frame(u32 frame);
    sFrames++;
    heapwatch_frame(sFrames);
    /* PB2_PERF=1: time spent drawing vs the whole frame, every 600 frames */
    static int perf = -1;
    static u64 tRender, tStart, tPrev;
    if (perf < 0) {
        perf = getenv("PB2_PERF") != NULL;
    }
    /* PB2_BENCH=frame: draw that frame again and again, old and new 3D
     * rasteriser in turn (same process, same load), then quit */
    static long bench = -2;
    if (bench == -2) {
        bench = getenv("PB2_BENCH") ? atol(getenv("PB2_BENCH")) : -1;
    }
    if (bench >= 0 && sFrames == (u32)bench) {
        void pc_gpu3d_set_slow(int slow);
        double f = (double)SDL_GetPerformanceFrequency(), tot[2] = { 0, 0 };
        for (int round = 0; round < 10; round++) {
            for (int slow = 0; slow < 2; slow++) {
                pc_gpu3d_set_slow(slow);
                u64 a = SDL_GetPerformanceCounter();
                for (int k = 0; k < 20; k++) {
                    pc_ppu_render_frame();
                }
                tot[slow] += (SDL_GetPerformanceCounter() - a) * 1000.0 / f / 20;
            }
        }
        rt_log("bench frame %u: drawing %.2f ms (old 3D rasteriser %.2f ms)", sFrames, tot[0] / 10, tot[1] / 10);
        exit(0);
    }
    u64 t0 = SDL_GetPerformanceCounter();      /* the frame was drawn line by line (io.c) */
    if (perf) {
        u64 t1 = SDL_GetPerformanceCounter();
        tRender += t1 - t0;
        if (!tStart) {
            tStart = t0;
        }
        if (sFrames % 600 == 0) {
            double f = (double)SDL_GetPerformanceFrequency();
            rt_log("perf: %.2f ms/frame, drawing %.2f ms/frame", (t1 - tPrev) * 1000.0 / f / 600, tRender * 1000.0 / f / 600);
            tRender = 0;
            tPrev = t1;
        }
    }
    if (!sHeadless) {
        SDL_UpdateTexture(sTex, &(SDL_Rect){ 0, 0, PC_SCREEN_W, PC_SCREEN_H }, sFb[0], PC_SCREEN_W * 4);
        SDL_UpdateTexture(sTex, &(SDL_Rect){ 0, PC_SCREEN_H, PC_SCREEN_W, PC_SCREEN_H }, sFb[1], PC_SCREEN_W * 4);
        SDL_RenderClear(sRen);
        SDL_RenderCopy(sRen, sTex, NULL, NULL);
        SDL_RenderPresent(sRen);
    }
    if (sShotsDir && sFrames % sShotsEvery == 0) {
        char p[512];
        snprintf(p, sizeof p, "%s/f%05u.bmp", sShotsDir, sFrames);
        write_bmp(p);
    }
    if (sFrames == sShotFrame) {
        write_bmp(sShotPath);
        rt_log("screenshot at frame %u: %s", sFrames, sShotPath);
        exit(0);
    }
    void snd_vblank(void);
    snd_vblank();
    read_input();
    pace();
}
