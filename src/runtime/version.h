/* version.h - what differs between the builds: Black 2 / White 2, English /
 * French (CMake PB2_VERSION=black2|white2 and PB2_LANG=en|fr define
 * PB2_WHITE2 and PB2_FRENCH; the recompiled code itself comes from
 * tools/recomp/recomp.py --version, with the address map of that ROM). */
#ifndef PB2_VERSION_H
#define PB2_VERSION_H

#if defined(PB2_WHITE2) && defined(PB2_FRENCH)
#define PB2_GAME_NAME  "Pokemon Version Blanche 2"
#define PB2_GAME_TITLE "POKEMON W2"      /* ROM header, offset 0 */
#define PB2_GAME_CODE  "IRDF"            /* ROM header, offset 0x0C: French */
#define PB2_GAME_LANG  "French"
#define PB2_FILE_BASE  "pokeblanche2"    /* pokeblanche2.exe / .sav / .log */
#elif defined(PB2_FRENCH)
#define PB2_GAME_NAME  "Pokemon Version Noire 2"
#define PB2_GAME_TITLE "POKEMON B2"
#define PB2_GAME_CODE  "IREF"
#define PB2_GAME_LANG  "French"
#define PB2_FILE_BASE  "pokenoire2"
#elif defined(PB2_WHITE2)
#define PB2_GAME_NAME  "Pokemon White 2"
#define PB2_GAME_TITLE "POKEMON W2"
#define PB2_GAME_CODE  "IRDO"            /* English, USA/Europe */
#define PB2_GAME_LANG  "English, USA/Europe"
#define PB2_FILE_BASE  "pokewhite2"
#else
#define PB2_GAME_NAME  "Pokemon Black 2"
#define PB2_GAME_TITLE "POKEMON B2"
#define PB2_GAME_CODE  "IREO"
#define PB2_GAME_LANG  "English, USA/Europe"
#define PB2_FILE_BASE  "pokeblack2"
#endif

#endif
