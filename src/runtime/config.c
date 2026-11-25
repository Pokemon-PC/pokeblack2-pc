/*
 * config.c - config.ini next to the exe, and finding the player's ROM.
 *
 * config.ini (created with defaults when missing):
 *     rom_dir =            folder holding the player's ROMs (the launcher sets it)
 *
 * The ROM is searched, in this order:
 *     1. the path given on the command line (pokeblack2 <rom.nds>),
 *     2. the .nds files in the exe's folder,
 *     3. the .nds files in rom_dir (overridden by --rom-dir DIR or PB2_ROM_DIR).
 * A file is taken for its header (game code and title), never for its name:
 * the recompiled code is the English Pokemon Black 2 (IREO, "POKEMON B2").
 */
#include "rt.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char sExeDir[MAX_PATH];
static char sRomDir[MAX_PATH];

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = 0;
    }
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        memmove(s, s + 1, n - 1);
    }
}

static const char kDefaultIni[] =
    "; Pokemon Black 2 (PC) - settings\n"
    "\n"
    "; Folder holding your Pokemon Black 2 ROM (English, USA/Europe). Empty: the\n"
    "; ROM is looked for next to this exe. The file is recognised by its header,\n"
    "; whatever its name. Example: rom_dir = D:\\Games\\roms\n"
    "rom_dir =\n";

void config_load(void) {
    GetModuleFileNameA(NULL, sExeDir, sizeof sExeDir);
    char *slash = strrchr(sExeDir, '\\');
    if (slash) {
        *slash = 0;
    }
    char ini[MAX_PATH + 16];
    snprintf(ini, sizeof ini, "%s\\config.ini", sExeDir);
    FILE *f = fopen(ini, "r");
    if (!f) {
        FILE *w = fopen(ini, "w");
        if (w) {
            fputs(kDefaultIni, w);
            fclose(w);
        }
        return;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key);
        trim(val);
        if (_stricmp(key, "rom_dir") == 0) {
            snprintf(sRomDir, sizeof sRomDir, "%s", val);
        }
    }
    fclose(f);
}

/* the header of a candidate: Pokemon Black 2, English */
static int rom_is_ours(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    u8 h[0x10];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    return n == sizeof h && memcmp(h, "POKEMON B2", 10) == 0 && memcmp(h + 0x0C, "IREO", 4) == 0;
}

static int search_dir(const char *dir, char *out, size_t outsz) {
    if (!dir || !dir[0]) {
        return 0;
    }
    char pat[MAX_PATH + 8];
    snprintf(pat, sizeof pat, "%s\\*.nds", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int found = 0;
    do {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (rom_is_ours(p)) {
            snprintf(out, outsz, "%s", p);
            found = 1;
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* argv: [rom.nds] [--rom-dir DIR]; returns the ROM path or NULL */
const char *config_find_rom(int argc, char **argv) {
    static char path[MAX_PATH * 2];
    const char *explicit_rom = NULL;
    const char *env = getenv("PB2_ROM_DIR");          /* overrides config.ini */
    if (env && env[0]) {
        snprintf(sRomDir, sizeof sRomDir, "%s", env);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom-dir") == 0 && i + 1 < argc) {   /* overrides both */
            snprintf(sRomDir, sizeof sRomDir, "%s", argv[++i]);
        } else if (argv[i][0] != '-') {
            explicit_rom = argv[i];
        }
    }
    if (explicit_rom) {
        if (!rom_is_ours(explicit_rom)) {
            rt_log("warning: %s is not Pokemon Black 2 (English, IREO)", explicit_rom);
        }
        return explicit_rom;
    }
    if (search_dir(sExeDir, path, sizeof path) || search_dir(sRomDir, path, sizeof path)) {
        return path;
    }
    return NULL;
}

const char *config_rom_dir(void) {
    return sRomDir;
}
