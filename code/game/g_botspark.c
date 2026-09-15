/* muse-spark-1-3 duelist controller. GPL-2.0-or-later.
 * Self-contained provider for the native bot controller API; see
 * docs/bot-api.md. Registered from BotController_RegisterAll. */
#include "g_local.h"
#include "g_botapi.h"
#include "g_botspark.h"
#include "../botlib/aasfile.h"
#include "../botlib/be_aas.h"
#include "../botlib/be_ai_goal.h"
#include "../botlib/be_ai_move.h"

/*
 * muse-spark-1-3: pure controller-API duelist.
 *
 * Uses only the observation/action interface plus BotController_GetEntity.
 * No trap_EA_*, botlib handles, G_Alloc, or blocking calls, so it builds
 * for native and QVM and never disturbs stock AI state for other bots.
 *
 * Tactics: closest-enemy hunt with revenge bias and weak-target focus,
 * range-aware weapon choice with switch hysteresis, projectile lead,
 * tightened aim with small error, orbit/strafe dodge with damage
 * reaction, chase/retreat for ideal range plus wall-slide when stalled,
 * item stocking when targetless or outgunned at long range, splash
 * discipline up close and while stalled, self-splash shyness after
 * clipping nearby geometry, LOS-gated fire plus proactive wall
 * feelers (trap_Trace, server thread), alternating attack for respawn.
 *
 * Console name is sanitized from "muse spark 1.3": spaces/dots are not
 * allowed by BotController_Register, so attach with:
 *   botcontroller attach <slot> muse-spark-1-3 ["optional seed text"]
 */
typedef struct {
	unsigned int rng;
	int strafeDir;
	int nextStrafeTime;
	int nextJumpTime;
	int targetClient;
	float wanderYaw;
	int step;
	int lastHealth;
	float lockYaw;
	float lockPitch;
	qboolean hasLock;
	int splashShyUntil;
	int moveState;
} museSparkContext_t;

static museSparkContext_t museSparks[MAX_CLIENTS];

static unsigned int MuseSpark_HashConfig( const char *s ) {
	unsigned int h = 2166136261u;
	while (s && *s) {
		h ^= (unsigned int)(unsigned char)(*s++);
		h *= 16777619u;
	}
	return h;
}

static unsigned int MuseSpark_Rand( museSparkContext_t *c ) {
	unsigned int x = c->rng;
	if (!x) {
		x = 0x9E3779B9u;
	}
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	if (!x) {
		x = 0x85EBCA6Bu;
	}
	c->rng = x;
	return x;
}

static float MuseSpark_Rand01( museSparkContext_t *c ) {
	return (float)(MuseSpark_Rand(c) & 1023u) / 1023.0f;
}

static qboolean MuseSpark_IsTeamGame( int gametype ) {
	return gametype >= GT_TEAM;
}

static qboolean MuseSpark_Usable( const playerState_t *self, int weapon ) {
	if (weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS) {
		return qfalse;
	}
	if (!(self->stats[STAT_WEAPONS] & (1 << weapon))) {
		return qfalse;
	}
	if (weapon == WP_GAUNTLET) {
		return qtrue;
	}
	if (weapon < 0 || weapon >= MAX_WEAPONS) {
		return qfalse;
	}
	return self->ammo[weapon] != 0;
}

static qboolean MuseSpark_Strong( const playerState_t *self ) {
	if (MuseSpark_Usable(self, WP_ROCKET_LAUNCHER)) {
		return qtrue;
	}
	if (MuseSpark_Usable(self, WP_RAILGUN)) {
		return qtrue;
	}
	if (MuseSpark_Usable(self, WP_PLASMAGUN)) {
		return qtrue;
	}
	if (MuseSpark_Usable(self, WP_LIGHTNING)) {
		return qtrue;
	}
	if (MuseSpark_Usable(self, WP_BFG)) {
		return qtrue;
	}
#ifdef MISSIONPACK
	if (MuseSpark_Usable(self, WP_CHAINGUN)) {
		return qtrue;
	}
	if (MuseSpark_Usable(self, WP_NAILGUN)) {
		return qtrue;
	}
#endif
	return qfalse;
}

static float MuseSpark_WeaponScore( int weapon, float dist, qboolean usable ) {
	float score;
	if (!usable) {
		return -1000.0f;
	}
	switch (weapon) {
	case WP_GAUNTLET:
		return dist < 90.0f ? 70.0f : -1000.0f;
	case WP_MACHINEGUN:
		return dist < 800.0f ? 50.0f : 30.0f;
	case WP_SHOTGUN:
		score = 95.0f - dist * 0.08f;
		return dist < 850.0f ? score : -1000.0f;
	case WP_GRENADE_LAUNCHER:
		if (dist < 130.0f) {
			return 30.0f;
		}
		return dist < 450.0f ? 55.0f : 20.0f;
	case WP_ROCKET_LAUNCHER:
		if (dist < 130.0f) {
			return 30.0f;
		}
		if (dist < 900.0f) {
			return 90.0f;
		}
		return 75.0f;
	case WP_LIGHTNING:
		return dist <= 620.0f ? 100.0f : -1000.0f;
	case WP_RAILGUN:
		return dist > 350.0f ? 92.0f : 68.0f;
	case WP_PLASMAGUN:
		if (dist > 200.0f && dist < 800.0f) {
			return 89.0f;
		}
		return 84.0f;
	case WP_BFG:
		if (dist < 130.0f) {
			return 30.0f;
		}
		if (dist > 150.0f && dist < 850.0f) {
			return 93.0f;
		}
		return 60.0f;
#ifdef MISSIONPACK
	case WP_NAILGUN:
		return dist < 700.0f ? 75.0f : 40.0f;
	case WP_PROX_LAUNCHER:
		return dist < 450.0f ? 50.0f : 20.0f;
	case WP_CHAINGUN:
		return dist < 800.0f ? 70.0f : 45.0f;
#endif
	default:
		return -1000.0f;
	}
}

static int MuseSpark_BestWeapon( const playerState_t *self, float dist, int current, qboolean splashShy ) {
	int w;
	int best = WP_NONE;
	float bestScore = -10000.0f;
	float currentScore = -10000.0f;
	for (w = WP_GAUNTLET; w < WP_NUM_WEAPONS; w++) {
		float s;
		if (w == WP_GRAPPLING_HOOK) {
			continue;
		}
		s = MuseSpark_WeaponScore(w, dist, MuseSpark_Usable(self, w));
		if (splashShy && (w == WP_ROCKET_LAUNCHER || w == WP_GRENADE_LAUNCHER || w == WP_BFG) && s > 25.0f) {
			s = 25.0f;
		}
		if (w == current) {
			currentScore = s;
		}
		if (s > bestScore) {
			bestScore = s;
			best = w;
		}
	}
	/* Hysteresis: keep the current weapon when it is nearly as good. */
	if (current > WP_NONE && current < WP_NUM_WEAPONS && best != current) {
		if (currentScore + 12.0f >= bestScore && MuseSpark_Usable(self, current)) {
			return current;
		}
	}
	if (bestScore < -500.0f) {
		return WP_NONE;
	}
	return best;
}

static float MuseSpark_IdealRange( int weapon ) {
	switch (weapon) {
	case WP_GAUNTLET:
		return 60.0f;
	case WP_SHOTGUN:
		return 250.0f;
	case WP_GRENADE_LAUNCHER:
		return 300.0f;
	case WP_LIGHTNING:
		return 400.0f;
	case WP_ROCKET_LAUNCHER:
		return 450.0f;
	case WP_PLASMAGUN:
		return 500.0f;
	case WP_BFG:
		return 500.0f;
	case WP_RAILGUN:
		return 700.0f;
	default:
		return 500.0f;
	}
}

static float MuseSpark_ProjectileSpeed( int weapon ) {
	switch (weapon) {
	case WP_ROCKET_LAUNCHER:
		return 900.0f;
	case WP_PLASMAGUN:
		return 2000.0f;
	case WP_GRENADE_LAUNCHER:
		return 700.0f;
	case WP_BFG:
		return 550.0f;
	default:
		return 0.0f;
	}
}

static int MuseSpark_NearestItem( const botObservation_t *obs, const vec3_t from, vec3_t itemPos ) {
	int i;
	int best = -1;
	float bestDist = 0.0f;
	botObservedEntity_t item;
	vec3_t delta;
	float d;
	if (!obs || !from || !itemPos) {
		return -1;
	}
	for (i = 0; i < obs->numEntities; i++) {
		if (!BotController_GetEntity(i, &item)) {
			continue;
		}
		if (item.state.eType != ET_ITEM) {
			continue;
		}
		VectorSubtract(item.origin, from, delta);
		d = VectorLength(delta);
		if (best < 0 || d < bestDist) {
			best = i;
			bestDist = d;
			VectorCopy(item.origin, itemPos);
		}
	}
	return best;
}

/* Proactive wall probe at head height along the given yaw, so the bot
 * slides before grinding into geometry. Safe from Think (server thread)
 * in native and QVM builds alike. */
static qboolean MuseSpark_WallAhead( const botObservation_t *obs, const playerState_t *self, float yaw ) {
	vec3_t angles;
	vec3_t dir;
	vec3_t eye;
	vec3_t end;
	trace_t trace;
	if (!obs || !self) {
		return qfalse;
	}
	VectorCopy(self->origin, eye);
	eye[2] += self->viewheight > 0 ? (float)self->viewheight : 26.0f;
	angles[PITCH] = 0.0f;
	angles[YAW] = yaw;
	angles[ROLL] = 0.0f;
	AngleVectors(angles, dir, NULL, NULL);
	VectorMA(eye, 110.0f, dir, end);
	trap_Trace(&trace, eye, NULL, NULL, end, obs->client, MASK_PLAYERSOLID);
	return trace.fraction < 0.75f;
}

/* AAS travel toward a goal position. On success the action holds botlib
 * locomotion (caller overwrites view/weapon/buttons) and qtrue returns.
 * Any missing AAS, bad area, failure, or conversion problem falls back
 * to the heuristic legs, so this never strands the bot. */
static qboolean MuseSpark_Travel( museSparkContext_t *spark, const botObservation_t *obs,
	const playerState_t *self, botAction_t *action, const vec3_t goalPos, int goalEnt ) {
	bot_initmove_t initmove;
	bot_goal_t goal;
	bot_moveresult_t moveresult;
	vec3_t selfOrigin;
	float thinktime;
	int selfArea;
	if (!spark || !obs || !self || !action || spark->moveState <= 0) {
		return qfalse;
	}
	if (!trap_AAS_Initialized()) {
		return qfalse;
	}
	VectorCopy(self->origin, selfOrigin);
	selfArea = trap_AAS_PointAreaNum(selfOrigin);
	if (!selfArea) {
		return qfalse;
	}
	memset(&goal, 0, sizeof(goal));
	VectorCopy(goalPos, goal.origin);
	goal.areanum = trap_AAS_PointAreaNum(goal.origin);
	if (!goal.areanum) {
		return qfalse;
	}
	VectorSet(goal.mins, -8, -8, -8);
	VectorSet(goal.maxs, 8, 8, 8);
	goal.entitynum = goalEnt;
	memset(&initmove, 0, sizeof(initmove));
	VectorCopy(self->origin, initmove.origin);
	VectorCopy(self->velocity, initmove.velocity);
	initmove.viewoffset[2] = (float)self->viewheight;
	initmove.entitynum = obs->client;
	initmove.client = obs->client;
	thinktime = obs->elapsed > 0 ? (float)obs->elapsed / 1000.0f : 0.05f;
	if (thinktime > 0.5f) {
		thinktime = 0.5f;
	}
	initmove.thinktime = thinktime;
	if (self->pm_flags & PMF_DUCKED) {
		initmove.presencetype = PRESENCE_CROUCH;
	} else {
		initmove.presencetype = PRESENCE_NORMAL;
	}
	VectorCopy(self->viewangles, initmove.viewangles);
	if (self->groundEntityNum != ENTITYNUM_NONE) {
		initmove.or_moveflags |= MFL_ONGROUND;
	}
	if ((self->pm_flags & PMF_TIME_KNOCKBACK) && self->pm_time > 0) {
		initmove.or_moveflags |= MFL_TELEPORTED;
	}
	if ((self->pm_flags & PMF_TIME_WATERJUMP) && self->pm_time > 0) {
		initmove.or_moveflags |= MFL_WATERJUMP;
	}
	trap_BotInitMoveState(spark->moveState, &initmove);
	memset(&moveresult, 0, sizeof(moveresult));
	trap_BotMoveToGoal(&moveresult, spark->moveState, &goal, TFL_DEFAULT);
	if (moveresult.failure) {
		return qfalse;
	}
	return BotController_ReadBotlibAction(obs->client, action);
}

static qboolean MuseSpark_Create( int client, const char *config, void **context ) {
	museSparkContext_t *spark;
	unsigned int seed;
	if (client < 0 || client >= MAX_CLIENTS || !context) {
		return qfalse;
	}
	if (!config) {
		config = "";
	}
	spark = &museSparks[client];
	memset(spark, 0, sizeof(*spark));
	seed = MuseSpark_HashConfig(config);
	seed ^= (unsigned int)(client + 1) * 2654435761u;
	seed ^= 0xC0FFEEu;
	if (!seed) {
		seed = 0x1234567u;
	}
	spark->rng = seed;
	spark->strafeDir = (MuseSpark_Rand(spark) & 1u) ? 1 : -1;
	spark->targetClient = -1;
	spark->lastHealth = 100;
	spark->hasLock = qfalse;
	spark->wanderYaw = 0.0f;
	/* Botlib steering is a bonus: without a move state the heuristic
	 * legs below still fight. Attach must not fail over this. */
	spark->moveState = trap_BotAllocMoveState();
	*context = spark;
	return qtrue;
}

static void MuseSpark_Destroy( int client, void *context ) {
	museSparkContext_t *spark = (museSparkContext_t *)context;
	(void)client;
	if (spark && spark->moveState > 0) {
		trap_BotFreeMoveState(spark->moveState);
		spark->moveState = 0;
	}
}

static qboolean MuseSpark_Think( const botObservation_t *obs, botAction_t *action, void *context ) {
	museSparkContext_t *spark = (museSparkContext_t *)context;
	const playerState_t *self;
	int health;
	int i;
	int bestEnt = -1;
	int itemEnt = -1;
	float bestDist = 0.0f;
	float bestScoreDist = 0.0f;
	botObservedEntity_t ent;
	int selfTeam;
	int attacker;
	vec3_t eye;
	vec3_t aim;
	vec3_t dir;
	vec3_t angles;
	float dist;
	float yawDiff;
	float pitchDiff;
	float errScale;
	int weapon;
	float ideal;
	float rangeError;
	float speed2d;
	vec3_t velFlat;
	qboolean strong;
	trace_t losTrace;
	qboolean hasLOS;
	vec3_t itemRunPos;
	vec3_t toItemRun;
	vec3_t itemRunAngles;

	if (!obs || !action || !spark) {
		return qfalse;
	}
	self = &obs->self;
	if (self->pm_type == PM_DEAD) {
		spark->targetClient = -1;
		spark->hasLock = qfalse;
		if (spark->step++ & 1) {
			action->buttons = BUTTON_ATTACK;
		}
		return qtrue;
	}
	if (self->pm_type != PM_NORMAL) {
		return qfalse;
	}
	health = self->stats[STAT_HEALTH];
	if (obs->elapsed == 0) {
		/* Fresh spawn or team/mode change: re-anchor aim, keep RNG/streak. */
		spark->lockYaw = self->viewangles[YAW];
		spark->lockPitch = self->viewangles[PITCH];
		spark->hasLock = qtrue;
		spark->targetClient = -1;
		spark->lastHealth = health;
		spark->wanderYaw = self->viewangles[YAW];
		if (spark->moveState > 0) {
			trap_BotResetMoveState(spark->moveState);
			trap_BotResetAvoidReach(spark->moveState);
		}
	}
	/* Damage reaction: flip strafe and hop on the next think. */
	if (health < spark->lastHealth) {
		spark->strafeDir = -spark->strafeDir;
		spark->nextJumpTime = obs->time;
		/* Self-splash (rocket/grenade/BFG clipped nearby geometry): go
		 * splash-shy for a while instead of repeating the mistake. */
		if (self->persistant[PERS_ATTACKER] == obs->client) {
			spark->splashShyUntil = obs->time + 6000;
		}
	}
	spark->lastHealth = health;

	selfTeam = self->persistant[PERS_TEAM];
	attacker = self->persistant[PERS_ATTACKER];

	/* Hunt the most attractive player entity. Closest wins, attacker favored. */
	for (i = 0; i < obs->numEntities; i++) {
		vec3_t delta;
		float d;
		int entTeam;
		if (i == obs->client) {
			continue;
		}
		if (!BotController_GetEntity(i, &ent)) {
			continue;
		}
		if (ent.state.eType != ET_PLAYER) {
			continue;
		}
		if (ent.health <= 0) {
			continue;
		}
		if (ent.state.clientNum == obs->client) {
			continue;
		}
		entTeam = ent.team;
		if (entTeam == TEAM_SPECTATOR) {
			continue;
		}
		if (MuseSpark_IsTeamGame(obs->gametype) && entTeam != -1 && entTeam == selfTeam) {
			continue;
		}
		VectorSubtract(ent.origin, self->origin, delta);
		d = VectorLength(delta);
		/* Revenge bias: prefer whoever hit us last when both are visible. */
		if (ent.state.clientNum == attacker) {
			d *= 0.7f;
		}
		/* Finish weak enemies: a low-health target is a cheap frag. */
		d -= (float)(100 - ent.health) * 1.5f;
		if (bestEnt < 0 || d < bestScoreDist) {
			bestEnt = i;
			bestScoreDist = d;
		}
	}

	if (bestEnt < 0) {
		/* No target: grab the nearest item to stock weapons, else wander. */
		float spd;
		vec3_t itemPos;
		vec3_t toItem;
		vec3_t itemAngles;
		spark->targetClient = -1;
		spark->hasLock = qfalse;
		if ((itemEnt = MuseSpark_NearestItem(obs, self->origin, itemPos)) >= 0 &&
			MuseSpark_Travel(spark, obs, self, action, itemPos, itemEnt)) {
			weapon = MuseSpark_BestWeapon(self, 500.0f, self->weapon, obs->time < spark->splashShyUntil);
			action->weapon = MuseSpark_Usable(self, weapon) ? weapon : 0;
			action->buttons = 0;
			return qtrue;
		}
		if (itemEnt >= 0) {
			VectorSubtract(itemPos, self->origin, toItem);
			if (VectorLength(toItem) > 24.0f) {
				vectoangles(toItem, itemAngles);
				spark->wanderYaw = itemAngles[YAW];
			}
			action->viewangles[YAW] = spark->wanderYaw;
			action->viewangles[PITCH] = 0.0f;
			action->viewangles[ROLL] = 0.0f;
			action->forward = 127;
			action->right = 0;
			action->up = 0;
			action->buttons = 0;
		} else {
			spark->wanderYaw = AngleNormalize180(spark->wanderYaw + 3.0f + MuseSpark_Rand01(spark) * 4.0f);
			action->viewangles[YAW] = spark->wanderYaw;
			action->viewangles[PITCH] = 0.0f;
			action->viewangles[ROLL] = 0.0f;
			action->forward = 90;
			action->right = spark->strafeDir * 30;
			action->up = 0;
			action->buttons = 0;
		}
		if (obs->time >= spark->nextStrafeTime) {
			if ((MuseSpark_Rand(spark) & 3u) == 0) {
				spark->strafeDir = -spark->strafeDir;
			}
			spark->nextStrafeTime = obs->time + 900 + (int)(MuseSpark_Rand(spark) % 700u);
		}
		spd = VectorLength(self->velocity);
		if (spd < 40.0f && obs->time >= spark->nextJumpTime) {
			action->up = 127;
			action->forward = 127;
			spark->nextJumpTime = obs->time + 700;
		}
		if (MuseSpark_WallAhead(obs, self, spark->wanderYaw)) {
			action->forward = 0;
			action->right = spark->strafeDir * 127;
			action->up = 127;
		}
		weapon = MuseSpark_BestWeapon(self, 500.0f, self->weapon, obs->time < spark->splashShyUntil);
		action->weapon = MuseSpark_Usable(self, weapon) ? weapon : 0;
		return qtrue;
	}

	/* We have a target: fetch it once more for the aim point. */
	if (!BotController_GetEntity(bestEnt, &ent)) {
		return qtrue;
	}
	spark->targetClient = ent.state.clientNum;
	VectorSubtract(ent.origin, self->origin, dir);
	bestDist = VectorLength(dir);
	dist = bestDist;
	if (dist < 1.0f) {
		dist = 1.0f;
	}

	/* Outgunned at long range: stock up instead of charging a peashooter
	 * across the map. Healthy and armed bots always press the fight. */
	strong = MuseSpark_Strong(self);
	if (bestDist > 550.0f && (!strong || health < 40)) {
		if ((itemEnt = MuseSpark_NearestItem(obs, self->origin, itemRunPos)) >= 0 &&
			MuseSpark_Travel(spark, obs, self, action, itemRunPos, itemEnt)) {
			spark->hasLock = qfalse;
			weapon = MuseSpark_BestWeapon(self, 500.0f, self->weapon, obs->time < spark->splashShyUntil);
			action->weapon = MuseSpark_Usable(self, weapon) ? weapon : 0;
			action->buttons = 0;
			return qtrue;
		}
		if (itemEnt >= 0) {
			float runSpd;
			spark->hasLock = qfalse;
			VectorSubtract(itemRunPos, self->origin, toItemRun);
			if (VectorLength(toItemRun) > 24.0f) {
				vectoangles(toItemRun, itemRunAngles);
				spark->wanderYaw = itemRunAngles[YAW];
			}
			action->viewangles[YAW] = spark->wanderYaw;
			action->viewangles[PITCH] = 0.0f;
			action->viewangles[ROLL] = 0.0f;
			action->forward = 127;
			action->right = 0;
			action->up = 0;
			action->buttons = 0;
			runSpd = VectorLength(self->velocity);
			if (runSpd < 40.0f && obs->time >= spark->nextJumpTime) {
				action->up = 127;
				spark->nextJumpTime = obs->time + 700;
			}
			if (MuseSpark_WallAhead(obs, self, spark->wanderYaw)) {
				action->forward = 0;
				action->right = spark->strafeDir * 127;
				action->up = 127;
			}
			weapon = MuseSpark_BestWeapon(self, 500.0f, self->weapon, obs->time < spark->splashShyUntil);
			action->weapon = MuseSpark_Usable(self, weapon) ? weapon : 0;
			return qtrue;
		}
	}

	weapon = MuseSpark_BestWeapon(self, bestDist, self->weapon, obs->time < spark->splashShyUntil);
	if (!MuseSpark_Usable(self, weapon)) {
		weapon = self->weapon;
		if (!MuseSpark_Usable(self, weapon)) {
			weapon = WP_NONE;
		}
	}

	/* Eye and chest-level aim with projectile lead. */
	VectorCopy(self->origin, eye);
	if (self->viewheight > 0) {
		eye[2] += (float)self->viewheight;
	} else {
		eye[2] += 26.0f;
	}
	VectorCopy(ent.origin, aim);
	aim[2] += 24.0f;
	{
		float shellSpeed = MuseSpark_ProjectileSpeed(weapon);
		if (shellSpeed > 0.0f && dist > 120.0f) {
			float leadTime = dist / shellSpeed;
			if (leadTime > 0.8f) {
				leadTime = 0.8f;
			}
			leadTime *= 0.85f;
			aim[0] += ent.state.pos.trDelta[0] * leadTime;
			aim[1] += ent.state.pos.trDelta[1] * leadTime;
			aim[2] += ent.state.pos.trDelta[2] * leadTime * 0.5f;
		}
	}
	/* Only shoot what the muzzle can actually see: saves ammo and ends
	 * wall-blind splash suicides. Hitting the target itself also counts. */
	trap_Trace(&losTrace, eye, NULL, NULL, aim, obs->client, MASK_SHOT);
	hasLOS = losTrace.fraction > 0.9f || losTrace.entityNum == bestEnt;
	VectorSubtract(aim, eye, dir);
	if (VectorLength(dir) < 0.001f) {
		VectorCopy(self->viewangles, angles);
	} else {
		vectoangles(dir, angles);
	}
	/* Small human-like error that grows with range and target speed. */
	{
		float targetSpeed = VectorLength(ent.state.pos.trDelta);
		errScale = 0.6f + dist / 1200.0f + targetSpeed / 900.0f;
		if (errScale > 2.0f) {
			errScale = 2.0f;
		}
		angles[YAW] += (MuseSpark_Rand01(spark) - 0.5f) * 2.0f * errScale;
		angles[PITCH] += (MuseSpark_Rand01(spark) - 0.5f) * 2.0f * errScale;
	}
	/* vectoangles wraps pitch into (-360, 0]; normalize so the fire gate
	 * below compares true angular distance, not wrap distance. */
	angles[YAW] = AngleNormalize180(angles[YAW]);
	angles[PITCH] = AngleNormalize180(angles[PITCH]);
	angles[ROLL] = 0.0f;
	action->viewangles[YAW] = angles[YAW];
	action->viewangles[PITCH] = angles[PITCH];
	action->viewangles[ROLL] = 0.0f;
	spark->lockYaw = angles[YAW];
	spark->lockPitch = angles[PITCH];
	spark->hasLock = qtrue;

	/* Unseen enemy: let AAS walk us into the fight; the direct legs and
	 * the fire gate below stay as the fallback and the discipline. */
	if (!hasLOS) {
		if (MuseSpark_Travel(spark, obs, self, action, ent.origin, bestEnt)) {
			action->buttons = 0;
			action->weapon = weapon > WP_NONE ? weapon : 0;
			return qtrue;
		}
	}

	/* Movement: chase/retreat around the ideal range plus orbit strafe. */
	ideal = MuseSpark_IdealRange(weapon);
	rangeError = bestDist - ideal;
	if (rangeError > 150.0f) {
		action->forward = 127;
	} else if (rangeError < -150.0f) {
		action->forward = -80;
	} else {
		action->forward = 40;
	}
	if (obs->time >= spark->nextStrafeTime) {
		spark->strafeDir = -spark->strafeDir;
		spark->nextStrafeTime = obs->time + 700 + (int)(MuseSpark_Rand(spark) % 600u);
	}
	action->right = spark->strafeDir * 100;
	action->up = 0;

	velFlat[0] = self->velocity[0];
	velFlat[1] = self->velocity[1];
	velFlat[2] = 0.0f;
	speed2d = VectorLength(velFlat);
	if ((speed2d < 45.0f && rangeError > 150.0f) ||
		(rangeError > 150.0f && MuseSpark_WallAhead(obs, self, action->viewangles[YAW]))) {
		/* Chasing into a wall, felt or already grinding: slide along it. */
		action->forward = 0;
		action->right = spark->strafeDir * 127;
		action->up = 127;
	}
	if (speed2d < 45.0f && obs->time >= spark->nextJumpTime) {
		/* Likely snagged: hop and keep strafing. */
		action->up = 127;
		spark->nextJumpTime = obs->time + 800;
	} else if (bestDist < 300.0f && obs->time >= spark->nextJumpTime) {
		if ((MuseSpark_Rand(spark) & 3u) == 0) {
			action->up = 127;
			spark->nextJumpTime = obs->time + 600 + (int)(MuseSpark_Rand(spark) % 600u);
		}
	}

	/* Fire only when roughly on target and inside the weapon envelope. */
	yawDiff = AngleNormalize180(angles[YAW] - self->viewangles[YAW]);
	if (yawDiff < 0.0f) {
		yawDiff = -yawDiff;
	}
	pitchDiff = angles[PITCH] - self->viewangles[PITCH];
	if (pitchDiff < 0.0f) {
		pitchDiff = -pitchDiff;
	}
	action->buttons = 0;
	action->weapon = weapon > WP_NONE ? weapon : 0;
	if (yawDiff < 12.0f && pitchDiff < 10.0f && weapon > WP_NONE && hasLOS) {
		qboolean inEnvelope = qtrue;
		qboolean splash;
		if (weapon == WP_GAUNTLET && bestDist > 90.0f) {
			inEnvelope = qfalse;
		} else if (weapon == WP_LIGHTNING && bestDist > 720.0f) {
			inEnvelope = qfalse;
		} else if (weapon == WP_SHOTGUN && bestDist > 850.0f) {
			inEnvelope = qfalse;
		}
		splash = (weapon == WP_ROCKET_LAUNCHER || weapon == WP_GRENADE_LAUNCHER || weapon == WP_BFG);
		if (splash && speed2d < 45.0f) {
			/* Probably grinding a wall: splash would come straight back. */
			inEnvelope = qfalse;
		}
		if (inEnvelope) {
			action->buttons = BUTTON_ATTACK;
		}
	}
	return qtrue;
}

static const botController_t museSparkController = {
	BOT_CONTROLLER_API_VERSION, "muse-spark-1-3", 50, MuseSpark_Create, MuseSpark_Think, MuseSpark_Destroy
};

qboolean BotSpark_Register( void ) {
	return BotController_Register(&museSparkController);
}
