# mod-ilvl-scaling

*Version française : [README_FR.md](README_FR.md)*

AzerothCore module (Playerbot fork) that **scales difficulty and loot up**
based on the **average item level of the real players in the group** — bots
from mod-playerbots are **excluded from the average**, but are affected by
the scaling like everyone else.

Built for the [liyunfan1223/azerothcore-wotlk](https://github.com/liyunfan1223/azerothcore-wotlk)
fork (Playerbot branch): bot detection relies on `WorldSession::IsBot()`,
provided by that fork. No compile-time dependency on mod-playerbots.

## How it works

1. The module computes (cached for 30 s) the **average equipped item level**
   of the real players of the group who are on the same map (shirt/tabard
   excluded; off-hand not expected when a two-hander is equipped). An
   ungrouped player counts as a group of one.
2. It derives a **delta** against the "expected" item level: `190` at level
   80 (configurable), `level × 2.0` below 80.
3. Each item level above the baseline raises three multipliers (clamped by
   configurable min/max):

   | Multiplier | Effect | Default |
   |---|---|---|
   | **Health** | damage dealt to creatures is **divided** (higher effective health, displayed health bar unchanged) | +2%/ilvl, max ×4 |
   | **Damage** | creature damage (melee, spells, DoTs) is **multiplied** | +1%/ilvl, max ×3 |
   | **Loot** | looted money, drop chance of percentage entries, number of rolls on loot references (boss loot, probabilistic rounding) | +1%/ilvl, max ×3 |

4. **Item upgrading** (`IlvlScaling.Loot.Upgrade.*`): every weapon/armor
   dropped on a creature corpse is replaced by a random item of the same
   slot, same armor/weapon type and same quality, picked between
   `group average ilvl − 15` and `group average ilvl`. With a 274 group, a
   heroic dungeon drops 259–274 gear instead of 200. Quest items,
   conditional drops, tokens and emblems are never touched; when no
   candidate of the right quality exists in the range (e.g. green items stop
   around ilvl 200), the best available items below the average are used.
   Chests are not affected (creature corpses only).

By default the scaling (difficulty **and** loot) only applies in dungeons
and raids (set `IlvlScaling.OnlyInInstances = 0` to extend it to the open
world), and grey mobs (9+ levels below the group average) always escape it.

## In game

- When entering a dungeon or raid, each real player gets a summary message.
- The **`.ilvlscaling`** command shows the current values at any time
  (recomputed immediately, bypassing the cache).

## Installation

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/Maddnes95/mod-ilvl-scaling.git
```

Re-run CMake and rebuild the worldserver. The configuration file
`mod_ilvl_scaling.conf.dist` is copied automatically; every option can be
reloaded in game with `.reload config`.

## Design notes

- Extra health is applied by **reducing damage taken** by creatures instead
  of raising their max health: it adapts instantly to group changes and never
  alters persisted stats.
- Player pets/totems/guardians are never scaled as targets, and their damage
  is reduced like their owner's.
- Quest items never get their drop chance modified.

## License

[GNU GPL v2](LICENSE) — same license as AzerothCore.
