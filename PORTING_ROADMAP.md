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
| R3 Environnement d'exécution | carte mémoire DS, chargement de l'ARM9 et des overlays depuis la ROM, BIOS, registres I/O, interruptions, appels indirects, fils (threads) | ⏳ — **ROM anglaise nécessaire** |
| R4 ARM7 reproduit | dialogue PXI : carte/système de fichiers, sauvegarde, écran tactile, horloge, son (pilote du port HeartGold), alimentation ; sans-fil hors ligne | — |
| R5 Écran titre | 2D/3D de HeartGold branchés : critère de réussite de l'essai | — |
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

## Commandes

```
python tools/recomp/recomp.py ../pokeblack2 build/rc [--version black2|white2]
ls build/rc/*.c | xargs -P 16 -I{} clang -fsyntax-only -w -Isrc/runtime -Ibuild/rc {}
```
