# Native C bot controllers

The controller API lets a C implementation replace an individual bot's decisions
while retaining ioquake3's spawning, physics, weapons, and networking. Controllers
compile into the server game module (`qagame`); the same interface builds as a
native library or QVM for base Quake III and Team Arena.

Start with [g_botapi.h](../code/game/g_botapi.h) and the working examples in
[g_botcontrollers.c](../code/game/g_botcontrollers.c). No external adapter is
required. This is a source-level extension interface, not a dynamically loaded
shared-library ABI or hot-reload facility.

## Try the examples

Build the server and game modules using the existing CMake build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ioq3ded qagame_baseq3 qagameqvm_baseq3 --parallel
```

Run a server with the newly built game module and your game data. For example,
from the repository root, using an isolated mod directory:

```sh
mkdir -p build/bot-controller-home/botcontrollers/vm
cp build/Release/baseq3/vm/qagame.qvm build/bot-controller-home/botcontrollers/vm/
./build/Release/ioq3ded \
  +set fs_basepath /absolute/path/to/quake3-data \
  +set fs_homepath "$PWD/build/bot-controller-home" \
  +set fs_game botcontrollers \
  +set vm_game 2 +set sv_pure 0 +set dedicated 1 \
  +set bot_enable 1 +set g_gametype 0 +map q3dm1
```

`fs_basepath` must contain `baseq3` and the required PK3s. The isolated mod
ensures the server loads the new `qagame.qvm` instead of the original game code
inside a PK3. `sv_pure 0` is for this local development setup. Native modules
use `vm_game 0` and the platform's built `qagame` library in the mod directory.

In the **server console**:

```text
addbot Sarge 3
addbot Grunt 3
botcontroller list
botcontroller attach 0 circle
botcontroller attach 1 idle
botcontroller detach 0
```

Use the bot slot numbers reported by `list`; players may already occupy lower
slots. `circle` moves forward while turning and requests respawn after death.
It demonstrates per-bot state and deterministic decisions, not navigation or
combat intelligence. `idle` releases all controls. Unattached bots use stock AI.
Detach before switching an attached bot to another provider.

You can also call `BotController_Attach(client, "name", "config")` and
`BotController_Detach(client)` from game code after the bot is connected.
Optional console configuration is one quoted string passed to `create`.

## Implement a controller

A provider supplies a static `botController_t` descriptor:

```c
#include "g_local.h"
#include "g_botapi.h"

static qboolean MyThink(const botObservation_t *obs,
                        botAction_t *action, void *context) {
    if (obs->self.pm_type != PM_NORMAL) return qfalse;
    action->forward = 80;
    action->viewangles[YAW] = 90;
    return qtrue;
}

const botController_t myController = {
    BOT_CONTROLLER_API_VERSION,
    "mybot",
    100,       /* Think interval in simulation milliseconds. */
    NULL,      /* Optional create(client, config, &context). */
    MyThink,
    NULL       /* Optional destroy(client, context). */
};
```

1. Add your source to `GAME_SOURCES` in [cmake/basegame.cmake](../cmake/basegame.cmake).
   Team Arena inherits this list.
2. Declare your descriptor in `g_botcontrollers.c` and register it inside
   `BotController_RegisterAll()` with `BotController_Register(&myController)`.
   Check the return value. Names must be unique, contain letters, digits,
   underscores or hyphens, and fit in `MAX_QPATH`.
3. Rebuild, restart the server, spawn a bot, and attach `mybot` to its slot.

The registry supports up to `BOT_CONTROLLER_MAX_PROVIDERS` providers. Each bot
has its own context. `create` can initialize a static per-client array, as the
example does, or allocate memory supported by your build. `G_Alloc` is an arena
allocator with no per-object free, so repeated attachments should not allocate
unbounded state from it. `destroy` must release provider-owned botlib handles or
native allocations. If `create` fails, it must clean up its partial allocations;
`destroy` is only called for successful attachments. Copy configuration text if
you need it after `create` returns.

## Observations and actions

`think` receives a copy of the bot's `playerState_t`, its client slot, simulation
time, elapsed time, game type, and the upper bound of entity indices. Player
state includes origin, velocity, view angles, health/armor statistics, ammo,
weapons, team, and spawn count. Observation/action pointers only live for the
callback; do not retain them.

Call `BotController_GetEntity(index, &entity)` to copy a linked entity's state,
current position, health, and team. Invalid, unlinked, or `SVF_NOCLIENT` entities
are excluded. These are **privileged server observations**: there is no camera,
field-of-view, or line-of-sight filter. Apply your own perception rules if needed.

Every `think` starts with neutral movement/buttons and the current view angle.
Return `qtrue` to use the resulting action, or `qfalse` to release controls and
retain the current view. Actions are held between think calls and submitted each
server frame through the same player-command path used by ordinary bots.

- `forward`, `right`, `up`: relative movement, clamped to `[-127, 127]`.
  Positive `up` jumps/swims upward; negative crouches/swims downward.
- `viewangles`: absolute world pitch, yaw, roll in degrees. Use `[-360, 360]`;
  invalid/non-finite components retain the current angle. Pitch is normalized
  and clamped to `[-89, 89]`. The API handles player delta-angle conversion.
- `buttons`: attack, use holdable, gesture, and walking are supported. Other
  bits are discarded. Controllers can request respawn with attack while dead.
- `weapon`: a `WP_*` value for an owned weapon, or zero to keep the current one.

## Reuse the internal bot library

The stock bot has three layers: decisions in `ai_*.c`, navigation/goals/weapons
in `code/botlib`, and elementary actions converted into `usercmd_t`. Controllers
replace the stock decision loop for attached clients and can call the existing
`trap_*` functions declared in `g_local.h`.

| Need | Existing interface |
| --- | --- |
| Locate a navigation area | `trap_AAS_PointAreaNum` |
| Plan movement to a goal | `trap_BotAllocMoveState`, `trap_BotInitMoveState`, `trap_BotMoveToGoal` |
| Choose item goals | `trap_BotAllocGoalState`, `trap_BotLoadItemWeights`, `trap_BotChooseLTGItem` |
| Choose a weapon | `trap_BotAllocWeaponState`, `trap_BotLoadWeaponWeights`, `trap_BotChooseBestFightWeapon` |
| Express movement/aim/fire | `trap_EA_Move`, `trap_EA_View`, `trap_EA_Attack` |

Allocate your own botlib handles in `create` and free them in `destroy`. Include
the relevant `code/botlib/be_*.h` types; initialize movement state from the current
observation before moving. Goal/weapon selection also needs the botlib inventory
representation and weight files; it does not consume raw ammo alone. The stock
`ai_dmq3.c` code demonstrates these requirements.

Elementary actions are reset and initialized with the current view/weapon before
each controller call. After using `trap_EA_*` or a botlib movement routine, call
`BotController_ReadBotlibAction(obs->client, action)` **inside `think`** to convert
that output to the action returned through this API. Without that conversion,
only the explicit `botAction_t` output controls the player.

## Timing and lifecycle

- Callbacks run synchronously on the server thread. Keep them bounded; blocking
  I/O or long inference would stall the game. Native controllers can poll results
  prepared elsewhere, but only the server thread may call game/botlib functions.
- Deterministic and stochastic controllers use the same interface. For
  reproducible decisions, keep a per-bot PRNG with an explicit seed in `config`,
  use simulation time, and avoid the shared game RNG. This API does not guarantee
  whole-engine deterministic replay across machines or configurations.
- Think intervals are 1..1000 ms, rounded by server frame timing. A delayed
  frame produces one think with actual elapsed time, not a burst of catch-up calls.
- First think and changes to spawn count, movement mode, or team report
  `elapsed == 0`; old actions are invalidated. Context survives these transitions,
  so reset any navigation/goal state that your controller needs to reset.
- `bot_pause` pauses custom controllers along with stock bots.
- Detach, disconnect, map change, map restart, and shutdown destroy controller
  state. Map transitions restore stock AI; reattach from game code or console.
- Attaching/detaching clears stock AI's held input and decision state. Human
  clients cannot be attached. Registration and lifecycle changes are rejected
  from inside callbacks; schedule them in surrounding game code instead.

The first version uses the existing `addbot` lifecycle, so it still requires
stock bot character data and an AAS navigation file even for an `idle` controller.
It does not create an asset-free bot client or expose a remote network endpoint.

## Validation

Build both native and QVM variants:

```sh
cmake --build build --target qagame_baseq3 qagame_missionpack \
  qagameqvm_baseq3 qagameqvm_missionpack --parallel
```

Run the standalone API contract tests (Clang/GCC; no game assets needed):

```sh
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -fsanitize=address,undefined tests/bot_controller_test.c \
  code/game/g_botapi.c -lm -o /tmp/ioq3-bot-controller-test
/tmp/ioq3-bot-controller-test
```

These exercise registration, lifecycle callbacks, independent bot contexts,
timing, neutral actions, movement/angle/weapon validation, life transitions,
entity filtering, and disconnect/reset cleanup. Also test live attach/detach,
multiple controllers, and map restart with actual game data.
