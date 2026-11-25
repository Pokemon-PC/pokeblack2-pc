/*
 * mem.c - the DS memory map at its native addresses in the PC process.
 *
 * Main RAM (4 MiB) is mirrored like on the DS across 0x02000000-0x02FFFFFF:
 * one shared memory section mapped four times (code uses both 0x023FFxxx and
 * 0x027FFxxx for the same system area). DTCM (16 KiB at 0x02FE0000) hides the
 * last mirror there. Allocation granularity on Windows is 64 KiB.
 */
#include "rt.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void alloc_at(u32 addr, u32 size, const char *what) {
    void *p = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)addr) {
        rt_fatal("cannot reserve %s at 0x%08X (%lu bytes): %lu", what, addr, (unsigned long)size,
                 (unsigned long)GetLastError());
    }
}

static void map_view(HANDLE sec, u32 offset, u32 size, u32 addr) {
    void *p = MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, offset, size, (void *)(uintptr_t)addr);
    if (p != (void *)(uintptr_t)addr) {
        rt_fatal("cannot map main RAM mirror at 0x%08X: %lu", addr, (unsigned long)GetLastError());
    }
}

void mem_init(void) {
    alloc_at(0x01FF0000, 0x10000, "ITCM");
    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x400000, NULL);
    if (sec == NULL) {
        rt_fatal("main RAM section: %lu", (unsigned long)GetLastError());
    }
    map_view(sec, 0, 0x400000, 0x02000000);
    map_view(sec, 0, 0x400000, 0x02400000);
    map_view(sec, 0, 0x400000, 0x02800000);
    map_view(sec, 0, 0x3E0000, 0x02C00000);
    alloc_at(DTCM_BASE, 0x10000, "DTCM");
    map_view(sec, 0x3F0000, 0x10000, 0x02FF0000);
    alloc_at(0x03000000, 0x00100000, "shared/ARM7 WRAM");
    alloc_at(0x04000000, 0x00010000, "I/O registers");
    alloc_at(0x04100000, 0x00010000, "I/O registers (FIFO/card data)");
    alloc_at(0x05000000, 0x00010000, "palettes");
    alloc_at(0x06000000, 0x00A00000, "VRAM");
    alloc_at(0x07000000, 0x00010000, "OAM");
    rt_log("memory map reserved at the native DS addresses");
}
