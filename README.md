# Pokémon Noire 2 — port PC natif (recompilation statique)

Le code ARM9 du jeu (désassemblage [pokeblack2](https://github.com/nds197/pokeblack2))
est traduit en C, puis compilé avec un environnement qui reproduit la DS
(mémoire, ARM7, 2D/3D, son). Détails et journal : `PORTING_ROADMAP.md`.

Aucune donnée du jeu n'est fournie : il faut **sa propre ROM** de Pokémon
Noire 2 anglaise (USA/Europe, code `IREO`).

## La ROM : `config.ini` et `rom_dir`

Au premier lancement, `config.ini` est créé à côté de l'exe :

```ini
; Folder holding your Pokemon Black 2 ROM (English, USA/Europe). Empty: the
; ROM is looked for next to this exe. ...
rom_dir =
```

La ROM est cherchée dans cet ordre :

1. le chemin donné en argument : `pokeblack2.exe <rom.nds>` ;
2. les fichiers `.nds` du dossier de l'exe ;
3. les fichiers `.nds` du dossier `rom_dir` (remplacé par la variable
   d'environnement `PB2_ROM_DIR`, puis par l'option `--rom-dir DOSSIER`).

Le bon fichier est reconnu par son **en-tête** (titre `POKEMON B2`, code
`IREO`), jamais par son nom : d'autres ROM peuvent être dans le même dossier.
Le lanceur règle `rom_dir` ; sur la machine de développement :
`rom_dir = D:\LUC\pokemonall\roms`.

La sauvegarde est `pokeblack2.sav` (512 Kio, image brute de la mémoire flash),
à côté de l'exe (`PB2_SAVE=fichier` pour un autre).

## Compiler

```
python tools/recomp/recomp.py ../pokeblack2 build/rc --extra lib/asm/crt0.s
cmake -S . -B build/pc -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/pc
```

## Paquet pour les joueurs

```
cmake -S . -B build/release -G Ninja -DCMAKE_C_COMPILER=clang -DPB2_RELEASE=ON
cmake --build build/release
python tools/package.py        # dist/PokemonNoire2-PC-<date>.zip
```

Le zip contient l'exe (sans console, SDL2 inclus), `config.ini` et
`LISEZMOI.txt` ; jamais de ROM ni de sauvegarde (le script le vérifie).
La sauvegarde et `pokeblack2.log` sont écrits à côté de l'exe.

## Jouer

Clavier : X = A, Z = B, S = X, A = Y, Q = L, W = R, Entrée = Start,
Maj droite / Retour arrière = Select, flèches ; souris sur l'écran du bas =
tactile.

## Tests et débogage

Touches scriptées, captures, horloge fixe : voir `tests/README.md`.
Variables : `PB2_HEADLESS`, `PB2_SHOT`, `PB2_SHOTS`, `PB2_KEYS`, `PB2_RTC`,
`PB2_MUTE`, `PB2_AUDIO_DUMP`, `PB2_PERF`, `PB2_BENCH`, `PB2_PROFILE`,
`PB2_WATCH`, `PB2_FNTRACE`, `PB2_WATCHADDR`, `PB2_3DDUMP`, `PB2_FRAMERENDER`,
`PB2_SLOWRASTER`, `PB2_3DTHREADS` (décrites dans `PORTING_ROADMAP.md` et en
tête des fichiers source).
