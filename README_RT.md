# Pokémon Platinum Demake — Arena em Tempo Real

Port of the real-time arena battle mod (originally for Pokémon Emerald) onto the
[sinnoh-remakes/pokeemerald-platinum](https://github.com/sinnoh-remakes/pokeemerald-platinum)
demake (pokeemerald-expansion based). Battles run live on an arena built from the map around you:
move with the D-pad, A/B/L/R fire your four moves, dodge the enemy's attacks.

## What is covered
- Gen 1–4 species sprites (national dex 1–493) and all 467 moves (Gen 4 moves have their own
  patterns/visuals in `tools/rt/movegen4.py`). Gen 3 move data comes from the vanilla table, Gen 4 data
  is translated from the expansion's `gMovesInfo` (`src/rt_movedata.c`, `include/rt_compat.h`).
- Gen 4 abilities/held items handled by the expansion; type chart read from `gTypeEffectivenessTable`.
- Options menu (scrolling): difficulty, auto aim, EXP share/bonus, battle speed, LEVEL CAP (next Gym ace:
  14/22/26/32/37/41/44/50, then 62 until the Hall of Fame) and AUTO HEAL (party healed after every battle).
- Battle: dash stamina bar, minimal transparent HUD, input buffer, talents (Lv 20/40/60, picked on the SUMMARY
  SKILLS page with START), trainer AI styles, rare arena modifiers (fog on Mt. Coronet / Eterna Forest, wind at the
  Valley Windworks and on the water), oversized sprites (Onix, Steelix, Dialga, Palkia...) drawn whole.
- SUMMARY SKILLS: A cycles stats / IVs / EVs. Shiny odds 1/64, reusable TMs, no trading needed for trade
  evolutions (Lv 36, or level up holding the item), evolution items sold in the Jubilife and Eterna marts.
- HM moves usable straight from the bag (Surf, Flash, Cut...).
- Not ported: legendary/roaming scripting (stubbed in `src/rt_legends.c`).

## Build
`make -j8` produces `pokeemerald.gba` (see INSTALL.md for the toolchain). Generated assets are checked in
(`src/data/rt`, `graphics/rt`); regenerate with `tools/rt/*.py` (needs PMD SpriteCollab).

## Credits
Sprites: [PMD SpriteCollab](https://github.com/PMDCollab/SpriteCollab) (CC BY-NC 4.0, see the
contributors list there). Base: sinnoh-remakes/pokeemerald-platinum, pokeemerald-expansion, pret/pokeemerald.
