# Tests par touches scriptées

Chaque fichier `.keys` est une valeur de `PB2_KEYS` (`frame:touches,...`,
voir `src/video/screen.c`). Avec `PB2_RTC` fixé, une exécution donne toujours
les mêmes images.

- `new_game_then_save.keys` : intro, titre, nouvelle partie (nom « AAAAAAA »,
  rival « HughAAA »), ville, chambre, dialogue avec la mère, puis menu X →
  SAVE → Oui. Vers la frame 22 400 : « AAAAAAA saved the game. »

```
PB2_RTC=2026-11-25-14:00 PB2_HEADLESS=1 PB2_SAVE=test.sav PB2_KEYS="$(cat tests/new_game_then_save.keys)" PB2_SHOT=22800:fin.bmp build/pc/pokeblack2.exe <ROM>
```

Rechargement : même `PB2_SAVE`, puis
`PB2_KEYS="3700:S,3706:,3900:A,3906:,4100:A,4106:"` : CONTINUE, retour dans
la maison à Pavonnay.
