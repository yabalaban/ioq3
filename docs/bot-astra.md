# Astra controller

Astra lives in `code/game/g_botastra.c` and registers as `astra`. It uses
controller observations/actions, `BotController_GetEntity`, shared item
definitions, and math helpers. It does not access game entities directly or
call engine/botlib traps.

After loading the rebuilt game module:

```text
addbot Sarge 5 "" 0 Astra
botcontroller list
botcontroller attach <Astra-slot> astra "match-seed"
```

The optional configuration string seeds a per-client random generator for
movement. It does not change aim accuracy. Reattach after map transitions, or
use the existing persistent bot-profile feature.

## Decisions

- Aim at center mass without artificial error; predict constant-velocity
  intercepts for projectiles. BFG/plasma use 2000 units/sec, rockets 900.
  Grenades receive approximate gravity/launch-boost compensation.
- Select usable weapons by range, including unlimited-ammo weapons, with
  hysteresis to avoid unnecessary switches. Close-range gauntlet movement
  commits to closing the gap.
- Turn and fire in the same command. Respect weapon range, wait for weapon
  switches, and suppress shots through nearby teammates.
- Prioritize needed health, armor, weapons, ammo and powerups; ignore hidden,
  unusable, and temporarily abandoned items. Aim and movement are independent.
- Vary strafing, react to damage, and use displacement over time to identify
  stalls. Hold an escape heading, jump when grounded, and temporarily abandon
  troublesome pickups.
- React to self-inflicted splash damage by selecting a safer weapon. After
  sustained firing without hit feedback, reposition and probe periodically.
- Use a medkit when critically hurt, alternate respawn inputs, and reset
  transient state when the API signals a lifecycle transition.

## Limits

Entity observations are privileged and include players behind walls. This
restricted observation/action subset has no collision traces or route queries.
Escape behavior and hit-feedback probes are heuristics, not line-of-sight
tests or pathfinding. Astra can still choose an obstructed target or get stuck.
It is a combat controller; it does not plan CTF objectives. Match results
depend on the map, opponents, spawn positions and game settings.

## Validation

The decision tests exercise the real registered callbacks against synthetic
observations, with real shared item definitions and math:

```sh
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -Wno-error=sign-compare -Wno-error=type-limits \
  -ffunction-sections -fdata-sections -fsanitize=address,undefined \
  tests/bot_astra_test.c code/game/g_botastra.c code/game/bg_misc.c \
  code/qcommon/q_math.c code/qcommon/q_shared.c -Wl,--gc-sections -lm \
  -o /tmp/bot-astra-test
ASAN_OPTIONS=detect_leaks=0 /tmp/bot-astra-test
```

Add `-DMISSIONPACK` to exercise the Team Arena variant. The warning exceptions
cover existing diagnostics in shared game code; leak detection is disabled
because LeakSanitizer cannot operate under the workspace process tracer.
AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.

```sh
cmake --build build --target qagame_baseq3 qagame_missionpack \
  qagameqvm_baseq3 qagameqvm_missionpack --parallel 2
```

### Development smoke matches

Three-minute FFA games against stock skill-5 Sarge, using a separate temporary
mod/home and `timescale 4`, produced the following results after the firing and
self-splash fixes, before the final pickup-quantity weighting adjustment:

| Map | Module | Astra score | Sarge score |
| --- | --- | ---: | ---: |
| q3dm1 | Native | 3 | 1 |
| q3tourney2 | QVM interpreter | -1 | 5 |

Only the first attached-controller round in each log is included; map restarts
restore stock AI. These are smoke tests, not a controlled win-rate benchmark.
The second map included a hazard death and exposed the excessive small-pickup
priority subsequently corrected in the decision tests.
