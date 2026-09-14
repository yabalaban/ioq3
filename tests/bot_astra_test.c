/* Decision regressions using real Astra callbacks and shared game definitions.
 * cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
 *   -Wno-error=sign-compare -Wno-error=type-limits \
 *   -ffunction-sections -fdata-sections -fsanitize=address,undefined \
 *   tests/bot_astra_test.c code/game/g_botastra.c code/game/bg_misc.c \
 *   code/qcommon/q_math.c code/qcommon/q_shared.c -Wl,--gc-sections -lm \
 *   -o /tmp/bot-astra-test
 * ASAN_OPTIONS=detect_leaks=0 /tmp/bot-astra-test
 */
#include "../code/game/g_botapi.h"
#include "../code/game/bg_public.h"
#include "../code/game/g_botastra.h"
#include <assert.h>
#include <stdio.h>

static const botController_t *provider;
static botObservedEntity_t entities[MAX_GENTITIES];
static qboolean present[MAX_GENTITIES];
static botObservation_t obs;
static void *context;

qboolean BotController_Register( const botController_t *p ) {
	assert(!provider);
	provider = p;
	return qtrue;
}

qboolean BotController_GetEntity( int index, botObservedEntity_t *entity ) {
	assert(index >= 0 && index < MAX_GENTITIES);
	if (!present[index]) return qfalse;
	*entity = entities[index];
	return qtrue;
}

static void Setup( void ) {
	memset(&obs, 0, sizeof(obs));
	memset(entities, 0, sizeof(entities));
	memset(present, 0, sizeof(present));
	obs.time = 1000;
	obs.numEntities = MAX_CLIENTS + 3;
	obs.self.pm_type = PM_NORMAL;
	obs.self.stats[STAT_HEALTH] = obs.self.stats[STAT_MAX_HEALTH] = 100;
	obs.self.stats[STAT_WEAPONS] = (1 << WP_MACHINEGUN) | (1 << WP_GAUNTLET);
	obs.self.weapon = WP_MACHINEGUN;
	obs.self.ammo[WP_MACHINEGUN] = 100;
	obs.self.viewheight = 26;
	obs.self.gravity = 800;
	obs.self.groundEntityNum = ENTITYNUM_WORLD;
	assert(provider->create(0, "regression-seed", &context));
}

static void Enemy( int slot, float x, float y ) {
	present[slot] = qtrue;
	entities[slot].state.eType = ET_PLAYER;
	entities[slot].state.clientNum = slot;
	entities[slot].state.groundEntityNum = ENTITYNUM_WORLD;
	entities[slot].health = 100;
	VectorSet(entities[slot].origin, x, y, 0);
}

static int Item( int slot, itemType_t type, int tag, float x, float y ) {
	int i;
	for (i = 1; i < bg_numItems; i++) {
		if (bg_itemlist[i].giType != type || (tag >= 0 && bg_itemlist[i].giTag != tag)) continue;
		present[slot] = qtrue;
		entities[slot].state.eType = ET_ITEM;
		entities[slot].state.modelindex = i;
		VectorSet(entities[slot].origin, x, y, 0);
		return i;
	}
	assert(0);
	return 0;
}

static botAction_t Think( void ) {
	botAction_t action;
	int i;
	memset(&action, 0, sizeof(action));
	VectorCopy(obs.self.viewangles, action.viewangles);
	assert(provider->think(&obs, &action, context));
	for (i = 0; i < 3; i++) assert(isfinite(action.viewangles[i]));
	assert(action.viewangles[PITCH] >= -89 && action.viewangles[PITCH] <= 89);
	assert(action.viewangles[YAW] >= -360 && action.viewangles[YAW] <= 360);
	assert(abs(action.forward) <= 127 && abs(action.right) <= 127);
	obs.elapsed = 50;
	obs.time += 50;
	return action;
}

static void Combat( void ) {
	botAction_t action;
	Setup();
	Enemy(1, -500, 0);
	action = Think();
	assert(action.buttons & BUTTON_ATTACK); /* Turn and shoot in one command. */
	assert(fabs(fabs(action.viewangles[YAW]) - 180) < 0.1);
	assert(!action.up); /* Spawn at rest does not mean stuck. */
	obs.self.ammo[WP_MACHINEGUN] = -1;
	action = Think();
	assert(action.weapon == WP_MACHINEGUN && (action.buttons & BUTTON_ATTACK));
	obs.self.ammo[WP_MACHINEGUN] = 0;
	action = Think();
	assert(action.weapon == WP_GAUNTLET && action.forward > 100);
	assert(!(action.buttons & BUTTON_ATTACK));

	Setup();
	Enemy(1, 1000, 0);
	obs.self.stats[STAT_WEAPONS] = 1 << WP_LIGHTNING;
	obs.self.weapon = WP_LIGHTNING;
	obs.self.ammo[WP_LIGHTNING] = 100;
	action = Think();
	assert(!(action.buttons & BUTTON_ATTACK));

	Setup();
	Enemy(1, 100, 0);
	obs.self.stats[STAT_WEAPONS] = 1 << WP_ROCKET_LAUNCHER;
	obs.self.weapon = WP_ROCKET_LAUNCHER;
	obs.self.ammo[WP_ROCKET_LAUNCHER] = 10;
	action = Think();
	assert(!(action.buttons & BUTTON_ATTACK));

	Setup();
	Enemy(1, 600, 0);
	entities[1].state.pos.trDelta[1] = 300;
	obs.self.stats[STAT_WEAPONS] = 1 << WP_BFG;
	obs.self.weapon = WP_BFG;
	obs.self.ammo[WP_BFG] = 10;
	action = Think();
	/* 300 units/s lateral motion and 2000 units/s projectile: ~8.63 degrees. */
	assert(action.viewangles[YAW] > 8 && action.viewangles[YAW] < 10);
	assert(action.buttons & BUTTON_ATTACK); /* Stationary clear shot is usable. */

	Setup();
	Enemy(1, 1000, -0.1f);
	action = Think();
	assert(fabs(action.viewangles[YAW]) < 0.1); /* 360-degree wrap stays valid. */
}

static void TeamsAndLifecycle( void ) {
	botAction_t action;
	Setup();
	obs.gametype = GT_TEAM;
	obs.self.persistant[PERS_TEAM] = TEAM_RED;
	Enemy(1, 500, 0);
	entities[1].team = TEAM_RED;
	action = Think();
	assert(!(action.buttons & BUTTON_ATTACK));
	Enemy(2, 900, 0);
	entities[2].team = TEAM_BLUE;
	action = Think();
	assert(!(action.buttons & BUTTON_ATTACK)); /* Ally blocks the shot. */
	entities[1].origin[1] = 200;
	action = Think();
	assert(action.buttons & BUTTON_ATTACK);
	obs.self.pm_type = PM_DEAD;
	obs.elapsed = 0;
	action = Think();
	assert(action.buttons & BUTTON_ATTACK);
	action = Think();
	assert(!action.buttons);
	obs.self.pm_type = PM_NORMAL;
	obs.elapsed = 0;
	obs.time = 10; /* Restart simulation time; stale timers must not survive. */
	action = Think();
	assert(!action.up && (action.buttons & BUTTON_ATTACK));
}

static void SuppliesAndEscape( void ) {
	botAction_t action;
	int i, health;
	Setup();
	Item(MAX_CLIENTS, IT_AMMO, WP_MACHINEGUN, 0, 50); /* Already stocked. */
	Item(MAX_CLIENTS + 1, IT_WEAPON, WP_ROCKET_LAUNCHER, 300, 0);
	action = Think();
	assert(action.forward == 127 && abs(action.right) < 2);
	assert(fabs(action.viewangles[YAW]) < 0.1);
	/* A tiny nearby armor pickup must not outrank getting a real weapon. */
	Item(MAX_CLIENTS + 2, IT_ARMOR, -1, 0, 70);
	action = Think();
	assert(fabs(action.viewangles[YAW]) < 0.1);
	present[MAX_CLIENTS + 2] = qfalse;
	/* Hidden pickups must not attract the bot. */
	entities[MAX_CLIENTS + 1].state.eFlags = EF_NODRAW;
	action = Think();
	assert(action.viewangles[YAW] != 0);

	Setup();
	obs.self.stats[STAT_HEALTH] = 25;
	Enemy(1, 500, 0);
	health = Item(MAX_CLIENTS, IT_HEALTH, -1, 0, 120);
	action = Think();
	assert(action.buttons & BUTTON_ATTACK);
	assert(fabs(action.viewangles[YAW]) < 0.1);
	assert(action.right < -100); /* Move to health while shooting east. */
	(void)health;
	obs.self.stats[STAT_HOLDABLE_ITEM] = Item(MAX_CLIENTS + 1, IT_HOLDABLE, HI_MEDKIT, 0, 0);
	action = Think();
	assert(action.buttons & BUTTON_USE_HOLDABLE);

	Setup();
	Item(MAX_CLIENTS, IT_WEAPON, WP_ROCKET_LAUNCHER, 300, 0);
	action = Think();
	for (i = 0; i < 14; i++) action = Think();
	assert(action.up || action.forward < 0 || abs(action.right) > 50);
	/* Escape persists across thinks instead of immediately chasing the wall. */
	action = Think();
	assert(action.forward < 0 || abs(action.right) > 50);
}

static void Contexts( void ) {
	botAction_t first, second, replay, otherAction;
	void *other;
	int i;
	Setup();
	Enemy(1, 500, 0);
	first = Think();
	assert(provider->create(2, "other", &other));
	for (i = 0; i < 20; i++) {
		botObservation_t otherObs = obs;
		otherObs.client = 2;
		otherObs.time += i * 50;
		otherObs.elapsed = i ? 50 : 0;
		assert(provider->think(&otherObs, &otherAction, other));
	}
	second = Think();
	Setup();
	Enemy(1, 500, 0);
	replay = Think();
	assert(!memcmp(&first, &replay, sizeof(first)));
	replay = Think();
	assert(!memcmp(&second, &replay, sizeof(second)));
	assert(!provider->create(-1, "", &other));
	assert(!provider->create(MAX_CLIENTS, "", &other));
}

static void CombatFeedback( void ) {
	botAction_t action;
	int i;
	Setup();
	Enemy(1, 500, 0);
	obs.self.stats[STAT_WEAPONS] |= 1 << WP_ROCKET_LAUNCHER;
	obs.self.weapon = WP_ROCKET_LAUNCHER;
	obs.self.ammo[WP_ROCKET_LAUNCHER] = 10;
	action = Think();
	assert(action.buttons & BUTTON_ATTACK);
	obs.self.stats[STAT_HEALTH] = 80;
	obs.self.persistant[PERS_ATTACKER] = obs.client;
	action = Think();
	assert(action.weapon == WP_MACHINEGUN);
	assert(!(action.buttons & BUTTON_ATTACK)); /* Allow the switch to finish. */
	obs.self.weapon = WP_MACHINEGUN;
	action = Think();
	assert(action.buttons & BUTTON_ATTACK);

	Setup();
	Enemy(1, 500, 0);
	for (i = 0; i < 42; i++) {
		obs.self.origin[1] = (i & 1) ? 100 : 0;
		action = Think();
	}
	assert(!(action.buttons & BUTTON_ATTACK)); /* Throttle sustained misses. */
	obs.self.persistant[PERS_HITS]++;
	action = Think();
	assert(action.buttons & BUTTON_ATTACK); /* Hit feedback restores fire. */
}

int main( void ) {
	assert(BotAstra_Register());
	assert(!strcmp(provider->name, "astra"));
	Combat();
	TeamsAndLifecycle();
	SuppliesAndEscape();
	Contexts();
	CombatFeedback();
	puts("Astra decision regressions passed");
	return 0;
}
