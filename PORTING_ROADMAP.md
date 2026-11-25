# Pokémon Noire 2 / Blanche 2 — essai de port PC par recompilation statique

Journal de l'essai (en français). Objectif de l'essai : savoir si l'on peut
faire tourner Noire 2 nativement sur PC **sans décompilation C complète**, en
traduisant automatiquement le code machine du jeu en C, branché sur la couche
DS du port HeartGold (`../pokeheartgold-master`).

## Pourquoi cette méthode

Il n'existe pas de décompilation C de Noire 2 / Blanche 2 : le meilleur dépôt,
[nds197/pokeblack2](https://github.com/nds197/pokeblack2) (cloné dans
`../pokeblack2`), est un **désassemblage** (≈ 1,6 million d'instructions ARM/
Thumb, presque pas de C) qui reconstruit les ROM anglaises à l'identique. Les
ports de HeartGold et d'Émeraude compilaient du C ; ici, il faut d'abord
produire ce C.

**Recompilation statique** (comme N64Recomp) : chaque fonction du jeu devient
une fonction C qui reproduit ses instructions sur un état CPU (registres,
drapeaux) ; la mémoire est la carte DS à ses vraies adresses, comme dans le
port HeartGold. Le C n'est pas lisible (c'est du code machine traduit) : les
mods de données marchent, pas les modifications de code ; au fil de
l'avancée du désassemblage, des fonctions traduites pourront être remplacées
par du vrai C.

## Jalons

| Jalon | Contenu | État |
|---|---|---|
| R1 Traducteur | `tools/recomp/recomp.py` : asm du dépôt → C, tout le jeu | ✅ |
| R2 Compilation | tout le C produit compile (Clang) | ✅ |
| R3 Environnement d'exécution | carte mémoire DS, chargement de l'ARM9 et des overlays depuis la ROM, BIOS, registres I/O, interruptions, appels indirects, fils (threads) | ✅ le jeu tourne en continu (boucle de frames, overlays, 3D envoyée) |
| R4 ARM7 reproduit | dialogue PXI : carte/système de fichiers, sauvegarde, écran tactile, horloge, son (pilote du port HeartGold), alimentation ; sans-fil hors ligne | ✅ PM, sauvegarde (`pokeblack2.sav`), NVRAM (firmware synthétisé), RTC, tactile, son (séquences, flux, effets) ; sans-fil : à faire |
| R5 Écran titre | 2D/3D de HeartGold branchés : critère de réussite de l'essai | ✅ intro, écran titre (frame ~3600), menu, « New Game », présentation de la professeure, nom du joueur et du rival, première vue de la ville |
| R6 Jeu | parcours, bugs, français (ROM FR du joueur) | — |

## Résultats (2026-11-25)

- **R1** : 265 fichiers asm, **42 192 fonctions, 1 585 340 instructions,
  3 non traduites (99,9998 %)**. Les 3 restantes sont le changement de contexte
  des fils (`ldmia/stmib … ^`), à reproduire dans le système (fils de l'hôte).
- **R2** : 298 fichiers C (86 Mo), **0 erreur** de compilation (`clang
  -fsyntax-only`).
- Traduction : registres et drapeaux dans une structure, drapeaux calculés
  seulement s'ils sont lus ensuite ; `bl`/`blx` → appels C ; retours
  (`bx lr`, `pop {pc}`, `ldmia sp!, {…, pc}`) → `return` ; branches internes →
  `goto` ; sauts calculés (tables de `switch`) → aiguillage sur les étiquettes ;
  appels indirects → `call_addr()` (table adresse → fonction ; pour les
  overlays qui partagent des adresses, choix d'après les octets chargés) ;
  BIOS (`SVC_*`) → `swi_call()` ; anti-piratage (`DSProt_*`) → réponse
  « cartouche authentique » ; `GAME_VERSION` (Noire 2 = 23, Blanche 2 = 22,
  option `--version`).

## Journal R3/R4 (2026-11-25, ROM anglaise du joueur)

Le jeu démarre, passe l'initialisation du système, charge ses overlays,
tourne sa boucle principale et envoie des commandes 3D chaque frame (pas encore
d'image : R5). Corrections qui ont débloqué le démarrage, dans l'ordre :

- pile IRQ : le gestionnaire (`FUN_01FF8C88`) empile `lr` et saute au
  gestionnaire de l'IRQ avec `ldr lr, =FUN_01FF8CE0 ; bx r0` ; le traducteur
  traite maintenant ce motif (retour vers `lr`, puis exécution de la cible) ;
- sauvegarde PXI (tag 11) : numérotation TwlSDK des commandes (6 lecture,
  7 écriture, 8 programmation, 9 vérification, 10-12/15 effacement, 13 état) ;
- NVRAM (tag 4) : lecture 0x23, écriture de page 0x24/0x25, registre d'état
  0x22 ; firmware synthétisé (réglages utilisateur valides, CRC16) ;
- RTC (tag 5), écran tactile (tag 6), son (tag 7 : liste de commandes, zone
  partagée repérée par la commande 0x1D) ;
- sauts calculés au milieu d'une fonction (`add pc, pc, rN` : division,
  remplissage) : chaque instruction devient une cible ; seules les
  instructions qui positionnent réellement les drapeaux le font
  (auto-test `PB2_SELFTEST` des divisions) ;
- branches d'une fonction vers une étiquette d'une autre : points d'entrée
  secondaires générés (53) ;
- **blocs `#if defined(WHITE2)/#else/#endif`** de l'asm : ils étaient tous
  traduits ensemble (411 instructions de Blanche 2 en trop) ; le traducteur
  les résout maintenant d'après `--version` ;
- compteur de VBlank `0x027FFC3C` (tenu par l'ARM7) ; GXSTAT (FIFO vide,
  moteur libre) ; IRQ FIFO géométrique (bit 21, de niveau) ;
- **boucles** : chaque branche arrière appelle `LOOP_TICK()` ; toutes les 256
  itérations, une ligne de balayage passe et les IRQ sont délivrées (les
  boucles qui attendent un drapeau posé par une interruption se terminent).

Outils de débogage : `PB2_WATCH=s` (pile de l'hôte + fils du jeu),
`PB2_WATCHADDR=hex` (point d'arrêt matériel en écriture sur une adresse DS),
`PB2_FNTRACE=1` (256 dernières entrées de fonctions), assertions du jeu
journalisées (`FUN_0203CB94`), `tools/symbolize.py exe log`.

## Journal R5 (2026-11-25)

Rendu : `src/video/` (moteurs 2D/3D logiciels du port HeartGold, fenêtre SDL2,
clavier/souris, 60 Hz). Corrections :

- **touches** : le tampon X/Y de l'ARM7 (0x027FFFA8) est OU-é avec KEYINPUT
  par le jeu ; il est écrit au format du SDK (X bit 10, Y bit 11, debug bit 13,
  couvercle fermé bit 15) — avant, A/B/Start étaient toujours « relâchés » ;
- **`bl` Thumb utilisé comme saut long** : le compilateur saute (`bl`) vers une
  étiquette d'une fonction que le désassemblage a coupée en deux (ex.
  FUN_02019C0C / FUN_02019C70, boucle de rendu du menu) ; le traducteur en fait
  un saut (appel + `return`, ou `goto` vers le début d'une « continuation »)
  au lieu d'un appel ;
- **couche 3D en 1re cible de fusion** : elle se mélange toujours avec son
  alpha par pixel, sans EVA/EVB (BLDCNT 0x0941, EVA=EVB=0 donnait des
  silhouettes noires pour les personnages MCSS) ;
- **sauvegarde** : le fichier est écrit 30 frames après la dernière écriture
  (et à la sortie), via un `.tmp` renommé — avant, chaque page écrite
  réécrivait 512 Kio ;
- **vitesse** : rendus en -O2, ordre des couches précalculé, fonds texte par
  tuile : ~3,5 ms par image au lieu de 13.

Outils ajoutés : `PB2_SHOTS_EVERY`, `PB2_PERF=1` (temps de dessin),
`PB2_PROFILE=fichier` + `tools/profile.py` (profil par échantillonnage),
`PB2_3DDUMP=frame` (polygones, palettes, registres 2D), journal sans verrou
CRT (le chien de garde ne se bloque plus), crash : `TerminateProcess`.

- **fils réutilisés** : le jeu recrée un fil terminé dans la même structure
  OSThread (fil de chargement, frame ~12830) ; la fibre de l'hôte restait
  celle de l'ancien fil, arrêtée juste avant `OS_Terminate` (écran noir). Le
  runtime note le mot « pc » du contexte quand il quitte une fibre ; si le
  contexte a changé depuis, le fil repart sur une fibre neuve.
  `OS_Terminate` est journalisé (état du planificateur) avant l'arrêt.

Résultat : nouvelle partie jusqu'à la ville de départ en 3D, dialogues sur le
terrain (frame ~14000 avec les touches scriptées).

Reste (R6) : parcours complet, paquet (exe + README, données lues dans la ROM du
joueur), Linux. `PB2_WATCHFROM=frame` limite `PB2_WATCHADDR` aux écritures
après une frame.

## Son (2026-11-25)

`src/audio/snd.c` : le pilote son logiciel du port HeartGold (séquenceur SSEQ
à 192 Hz, instruments SBNK, ondes SWAR PCM8/PCM16/ADPCM, PSG, bruit,
enveloppes, LFO, canaux directs, alarmes), branché derrière les listes de
commandes SND que la bibliothèque son ARM9 du jeu (recompilée) envoie par PXI
(tag 7). Les numéros de commandes sont ceux du TwlSDK (0x00 START_SEQ …
0x1D SHARED_WORK … 0x21 READ_DRIVER_INFO) ; le paquetage de
SETUP_CHANNEL_PCM vient de `FUN_0207D524` (canal | timer<<16 ; longueur |
shift<<22 | volume<<24 ; début de boucle | pan<<16 | format<<24 | boucle<<26).
La zone partagée `SNDSharedWork` est tenue comme par l'ARM7 : commandes
terminées, joueurs/canaux actifs, variables de séquence (stockées
directement dedans), compteurs de ticks. Les alarmes (flux `strm` de la
musique du titre) repartent vers l'ARM9 sur le tag 7 (numéro | id<<8).
Sortie SDL2 32768 Hz ; sans périphérique (PB2_HEADLESS) le pilote tourne une
fois par image. `PB2_AUDIO_DUMP=fichier` (s16 stéréo brut), `PB2_MUTE=1`,
`PB2_SNDLOG=1`. Vérifié : intro, musique du titre (flux rechargé, pas de
boucle de tampon), nouvelle partie jusqu'à la ville, sans saturation.
Non fait : READ_DRIVER_INFO (structure laissée telle quelle), capture/
réverbération, PSG/bruit en canal direct.

## Vitesse du terrain 3D (2026-11-25)

Profil sur le terrain (`PB2_PROFILE_FROM=frame`) : rastérisation 3D 43 %,
composition 2D 22 %, lecture des texels 7 %. Corrections :

- rastérisation : sur chaque ligne d'un triangle, seul l'intervalle de x où
  les trois poids barycentriques peuvent être positifs (élargi d'un pixel)
  est parcouru, le test exact par pixel est inchangé ; vérifié sur 300 000
  triangles aléatoires (fins, presque horizontaux) : aucun pixel perdu ;
  `PB2_SLOWRASTER=1` garde l'ancien parcours comme référence ;
- texels : plus de divisions (modulo 0x18000) dans le cas courant ;
- **deux fils pour la 3D** : la moitié basse de l'écran est rastérisée sur un
  fil SDL pendant que le fil du jeu fait la moitié haute (chaque pixel voit
  les polygones dans le même ordre ; pochoir des ombres par bande) ;
  `PB2_3DTHREADS=0` revient à un seul fil.

Mesures (`PB2_BENCH=frame` : la même image redessinée 200 fois dans le même
processus, ancien/nouveau en alternance) sur le terrain, frame 13 400 :
11 ms de dessin avec un fil, 6,4 ms avec deux. L'intervalle par ligne seul
ne gagne que 3 à 15 % : le coût vient des pixels réellement couverts (le
terrain a beaucoup de surdessin). Image complète avec la machine peu
chargée : 2,5 à 3 ms (intro, titre), 4 à 5 ms (présentation), 9 à 12 ms
(terrain, avant les deux fils) : 60 images/s tenues partout.

**Tests reproductibles** : le jeu lit l'horloge réelle (éclairage, animations),
donc deux exécutions différaient. `PB2_RTC=AAAA-MM-JJ-hh:mm` fixe l'heure de
départ, qui avance avec les images : deux exécutions donnent alors les mêmes
images. Avec elle, ancien et nouveau rastériseur donnent 28 images identiques
au pixel près jusqu'au terrain (frame 13 600).

## Sauvegarde et rechargement (2026-11-25)

Vérifié avec `tests/new_game_then_save.keys` : nouvelle partie, menu X →
SAVE → Oui, « AAAAAAA saved the game. » (date de `PB2_RTC`, Pavonnay) ; au
redémarrage avec le même fichier, le menu propose la partie sauvegardée
(nom, lieu, badges, temps de jeu), l'écran de saison s'affiche et le joueur
reprend dans la maison, au même endroit. Le fichier `pokeblack2.sav` est
l'image brute de 512 Kio de la mémoire flash de la cartouche. Les écritures
pendant la nouvelle partie (76 blocs) ne suffisent pas à une partie :
seule la sauvegarde du menu fait apparaître « CONTINUE », comme sur DS.

## Premier combat (2026-11-25)

Avec `tests/continue_to_first_battle.keys` : depuis la sauvegarde, sortie de
la maison, Matis, belvédère, Bianca, choix de Gruikui, Pokédex, puis
**combat contre Matis** (écran « VS. », menus tactiles FIGHT/BAG/RUN/
POKÉMON, attaques, PV, messages, victoire) et retour sur le terrain jusqu'au
Centre Pokémon. Corrections :

- **veneers Thumb→ARM** : `bx pc ; nop` suivi d'un veneer ARM écrit en octets
  (`ldr ip, [pc] ; bx ip`, parfois sur deux lignes `.byte`) et de l'adresse
  cible ; le traducteur en faisait `call_addr(R(15))`, donc un appel vers 0.
  1 134 veneers deviennent un saut vers la cible, souvent du code chargé en
  VRAM (overlay 169 à 0x06898000, moteur de combat) ;
- `rt_fatal` affiche la pile de l'hôte et la trace des fonctions ; un crash
  indique le module et le fil en cause.

À surveiller : un crash isolé (lecture à 0x8, hors de l'exécutable) juste
après CONTINUE, non reproduit en 7 essais.

## Rendu ligne par ligne (2026-11-25)

Chaque ligne de balayage est dessinée quand le faisceau l'atteint
(`io.c` → `pc_ppu_render_line`), avant le DMA et l'interruption H-Blank de
cette ligne : ce que le jeu change entre deux lignes (gestionnaire H-Blank,
toujours actif dans ce jeu, DMA H-Blank) apparaît comme sur DS. La 3D est
rendue au début de l'image, la luminosité maîtresse est appliquée par ligne.
`PB2_FRAMERENDER=1` revient au rendu de toute l'image au V-Blank. Sur les 28
images de référence, 25 sont identiques ; 3 (survol de Pavonnay) ne
diffèrent que par quelques sprites (feuilles). Le premier combat s'affiche
de la même façon. Journal : première utilisation d'un DMA H-Blank et de
l'interruption H-Blank.

## Suite de l'histoire jusqu'à la Route 19 (2026-11-25)

`tests/continue_to_route19.keys` (depuis la sauvegarde du premier test) :
Centre Pokémon et boutique, Poké Balls, Chaussures de course et Carte,
porte de la Route 19, puis la leçon de capture de Bianca : combat sauvage,
menu du sac, lancer de Poké Ball, animation de capture, « Gotcha! Purrloin
was caught! » ; le joueur est ensuite libre sur la Route 19. Aucune
correction n'a été nécessaire pour ce tronçon.

## Ce qu'il faut pour R3

- La **ROM anglaise** visée par le désassemblage : Noire 2
  (`sha1: e51e6dfb8678a3d19dcd2a10691b96a569ca0abb`) ou Blanche 2
  (`sha1: b5d7490be7b415b8f1e672a53e978a9cc667e56a`). Le code traduit lit ses
  données (tables, pointeurs) aux adresses d'origine : il faut les octets exacts
  du programme ARM9 et des overlays de cette version. La ROM française
  (`IRDF`) a un code placé autrement.
- Les **fonctions du SDK ne sont pas nommées** dans le désassemblage (seules
  ~11 le sont) : on ne peut pas remplacer `SND_*`, `FS_*`… par nom comme pour
  HeartGold. D'où R4 : reproduire ce que fait l'ARM7 en réponse aux messages
  PXI du jeu (le pilote son de HeartGold joue les mêmes séquences SSEQ).

## Risques

- Fils d'exécution du système (changement de contexte en assembleur) : à
  faire avec des fils/coroutines de l'hôte.
- Code à adresses partagées (344 overlays) : choix de la bonne fonction à
  l'appel indirect.
- Mode DSi (le jeu est « DSi Enhanced ») : viser le mode DS.
- Taille : un projet plus gros que le port HeartGold ; l'essai s'arrête si
  l'écran titre n'est pas atteignable.

## ROM : `rom_dir` (2026-11-25)

Les ROM sont désormais toutes dans `D:\LUC\pokemonall\roms\`. Le jeu lit
`config.ini` (créé à côté de l'exe avec `rom_dir =` vide) et cherche la ROM :
argument, dossier de l'exe, puis `rom_dir` (`PB2_ROM_DIR`, `--rom-dir`) ; il
la reconnaît par l'en-tête (`POKEMON B2`, `IREO`), pas par le nom
(`src/runtime/config.c`, `README.md`). Localement :
`build/pc/config.ini` → `rom_dir = D:\LUC\pokemonall\roms`.

## Commandes

```
python tools/recomp/recomp.py ../pokeblack2 build/rc --extra lib/asm/crt0.s [--version black2|white2]
cmake --build build/pc
build/pc/pokeblack2.exe            # ROM trouvée via rom_dir de build/pc/config.ini
build/pc/pokeblack2.exe <rom.nds>  # ou un chemin explicite
ls build/rc/*.c | xargs -P 16 -I{} clang -fsyntax-only -w -Isrc/runtime -Ibuild/rc {}
```
