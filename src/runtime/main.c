/*
 * main.c - boots the recompiled Pokemon Black 2 like the DS boot loader:
 * the ARM9 binary (still compressed: the game's crt0 unpacks it) at its RAM
 * address, the cartridge header at 0x027FFE00, then the game's _start.
 *
 * Usage: pokeblack2 [rom.nds] [--rom-dir DIR]   (Pokemon Black 2 USA/Europe, IREO;
 *        without a path the ROM is looked for next to the exe, then in the
 *        rom_dir of config.ini or PB2_ROM_DIR: see config.c)
 * Env:   PB2_TRACE=1   log the first access to each I/O register
 */
#include "rt.h"
#include "rc_decls.h"

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    void config_load(void);
    const char *config_find_rom(int argc, char **argv);
    const char *config_rom_dir(void);
    config_load();
    const char *rom = config_find_rom(argc, argv);
    if (!rom) {
        void config_message(const char *msg);
        char msg[1400];
        snprintf(msg, sizeof msg,
                 "Pokemon Black 2 ROM not found (English, USA/Europe: your own copy, .nds).\n\n"
                 "Put it next to pokeblack2.exe, or write its folder in config.ini:\n"
                 "rom_dir = <folder>   (now: \"%s\").", config_rom_dir());
        config_message(msg);
        return 2;
    }
    gTrace = getenv("PB2_TRACE") != NULL;
    void watchdog_start(void);
    watchdog_start();
    mem_init();
    rom_load(rom);
    cpu_init();
    io_init();
    arm7_init();
    void snd_init(void);
    snd_init();

    u32 a9off = rom_r32(0x20), a9entry = rom_r32(0x24), a9ram = rom_r32(0x28), a9size = rom_r32(0x2C);
    memcpy((void *)(uintptr_t)a9ram, gRom + a9off, a9size);
    memcpy((void *)(uintptr_t)0x027FFE00, gRom, 0x170);          /* DS cartridge header (not more: system work area follows) */
    *(u32 *)(uintptr_t)0x027FF800 = 0x00003FC2;                  /* card chip ID */
    *(u32 *)(uintptr_t)0x027FFC00 = 0x00003FC2;
    *(u16 *)(uintptr_t)0x027FFC10 = 0x5835;                      /* boot indicator */
    rt_log("ARM9 binary: 0x%X bytes at 0x%08X, entry 0x%08X", a9size, a9ram, a9entry);

    R(13) = 0x02FE3F00;
    LR = 0;
    _start();
    rt_log("_start returned");
    return 0;
}
