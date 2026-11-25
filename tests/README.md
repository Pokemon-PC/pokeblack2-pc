# Tests par touches scriptées

La ROM est trouvée par `rom_dir` (`build/pc/config.ini`, ici
`D:\LUC\pokemonall\roms`) ou donnée en argument.

Chaque fichier `.keys` est une valeur de `PB2_KEYS` (`frame:touches,...`,
voir `src/video/screen.c`). Avec `PB2_RTC` fixé, une exécution donne toujours
les mêmes images.

- `new_game_then_save.keys` : intro, titre, nouvelle partie (nom « AAAAAAA »,
  rival « HughAAA »), ville, chambre, dialogue avec la mère, puis menu X →
  SAVE → Oui. Vers la frame 22 400 : « AAAAAAA saved the game. »

```
PB2_RTC=2026-11-25-14:00 PB2_HEADLESS=1 PB2_SAVE=test.sav PB2_KEYS="$(cat tests/new_game_then_save.keys)" PB2_SHOT=22800:fin.bmp build/pc/pokeblack2.exe
```

Rechargement : même `PB2_SAVE`, puis
`PB2_KEYS="3700:S,3706:,3900:A,3906:,4100:A,4106:"` : CONTINUE, retour dans
la maison à Pavonnay.

- `continue_to_first_battle.keys` : part de la sauvegarde faite par le test
  précédent (`PB2_RTC=2026-11-25-16:00`) : CONTINUE, sortie de la maison,
  Matis, École des Dresseurs, escalier du belvédère, Bianca, choix de
  Gruikui (surnom « AAAAAAA… »), Pokédex, combat contre Matis (~20 700 à
  24 500 : Charge, Mimi-Queue, victoire), puis Centre Pokémon (~29 000).
- `continue_to_route19.keys` : même départ, puis la suite de l'histoire :
  Centre Pokémon (PC, boutique, Poké Balls), la mère (Chaussures de course,
  Carte), porte de la Route 19 (Potion), leçon de capture de Bianca
  (Chacripan sauvage, sac, Poké Ball, « Gotcha! », ~41 500), joueur libre
  sur la Route 19 (~43 500).

## Blanche 2 (anglaise et française)

Les sauvegardes de Noire 2 et Blanche 2 ont le même format : Blanche 2
recharge une sauvegarde faite par Noire 2, ce qui évite de rejouer à
l'aveugle des déplacements dont le minutage diffère entre versions et
langues (textes plus longs en français).

- `b2_save_at_lookout.keys` (Noire 2, depuis la sauvegarde du premier test,
  `PB2_RTC=2026-11-25-16:00`) : comme `continue_to_first_battle.keys`
  jusqu'au starter et au Pokédex, puis X → SAUVEGARDER au belvédère.
- `w2_battle_from_lookout.keys` (Blanche 2, `pokewhite2.exe` ou
  `pokeblanche2.exe`, avec cette sauvegarde) : CONTINUE, descente qui fait
  venir Matis, fin de la scène avec B, Matis, puis le combat (~10 600 à
  15 500) : « VS. », Moustillon, Charge, Mimi-Queue.
