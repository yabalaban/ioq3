/*
 * Fable: a native bot controller built on the controller API.
 * See docs/bot-api.md. GPL-2.0-or-later.
 *
 * The controller keeps the stock engine untouched. It only reads observations
 * through the controller API, plans movement with the existing botlib traps,
 * and returns a botAction_t. Decision helpers are pure functions so that the
 * standalone tests in tests/bot_fable_test.c can exercise them without a map.
 */
#include "g_local.h"
#include "g_botapi.h"
#include "g_botfable.h"
#include "../botlib/botlib.h"
#include "../botlib/be_aas.h"
#include "../botlib/be_ea.h"
#include "../botlib/be_ai_goal.h"
#include "../botlib/be_ai_move.h"
#include "../botlib/be_ai_weap.h"
#include "inv.h"

#define FABLE_DEFAULT_SKILL 1.0f
#define FABLE_DEFAULT_FOV 150.0f
#define FABLE_DEFAULT_TURN 900.0f
#define FABLE_DEFAULT_WEIGHTS "xaero"

#define FABLE_HEARING_RANGE 300.0f
#define FABLE_LIGHTNING_RANGE 768.0f
#define FABLE_GAUNTLET_RANGE 64.0f
#define FABLE_SELF_SPLASH_MARGIN 40.0f
#define FABLE_MISSILE_MISS_RADIUS 120.0f
#define FABLE_MISSILE_TIME 1.5f
#define FABLE_STUCK_DISTANCE 32.0f
#define FABLE_STUCK_TIME 1500

/*
==================
Configuration
==================
*/
static qboolean Fable_KeyValue( const char *token, char *key, int keySize, char *value, int valueSize ) {
	const char *eq;
	int len;
	eq = strchr(token, '=');
	if (!eq || eq == token || !eq[1]) return qfalse;
	len = eq - token;
	if (len >= keySize) return qfalse;
	memcpy(key, token, len);
	key[len] = 0;
	len = strlen(eq + 1);
	if (len >= valueSize) return qfalse;
	strcpy(value, eq + 1);
	return qtrue;
}

static qboolean Fable_ParseFloat( const char *value, float min, float max, float *out ) {
	const char *s;
	for (s = value; *s; s++) {
		if (!((*s >= '0' && *s <= '9') || *s == '.' || *s == '-')) return qfalse;
	}
	*out = atof(value);
	return *out >= min && *out <= max;
}

qboolean Fable_ParseConfig( const char *config, int client, fableConfig_t *out ) {
	char token[64], key[32], value[32];
	const char *s;
	int len, i;
	float f;

	memset(out, 0, sizeof(*out));
	out->skill = FABLE_DEFAULT_SKILL;
	out->fov = FABLE_DEFAULT_FOV;
	out->turnRate = FABLE_DEFAULT_TURN;
	out->seed = 0x9E3779B9u ^ (unsigned) (client + 1) * 2654435761u;
	Q_strncpyz(out->weights, FABLE_DEFAULT_WEIGHTS, sizeof(out->weights));
	if (!config) return qtrue;

	s = config;
	for (;;) {
		while (*s == ' ' || *s == '\t') s++;
		if (!*s) return qtrue;
		len = 0;
		while (*s && *s != ' ' && *s != '\t') {
			if (len >= (int) sizeof(token) - 1) return qfalse;
			token[len++] = *s++;
		}
		token[len] = 0;
		if (!Fable_KeyValue(token, key, sizeof(key), value, sizeof(value))) return qfalse;
		if (!Q_stricmp(key, "skill")) {
			if (!Fable_ParseFloat(value, 0, 1, &f)) return qfalse;
			out->skill = f;
		} else if (!Q_stricmp(key, "fov")) {
			if (!Fable_ParseFloat(value, 10, 360, &f)) return qfalse;
			out->fov = f;
		} else if (!Q_stricmp(key, "turn")) {
			if (!Fable_ParseFloat(value, 30, 7200, &f)) return qfalse;
			out->turnRate = f;
		} else if (!Q_stricmp(key, "seed")) {
			if (!Fable_ParseFloat(value, -2147483648.0f, 2147483647.0f, &f)) return qfalse;
			out->seed = atoi(value);
		} else if (!Q_stricmp(key, "weights")) {
			for (i = 0; value[i]; i++) {
				if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z') ||
					(value[i] >= '0' && value[i] <= '9') || value[i] == '_')) return qfalse;
			}
			Q_strncpyz(out->weights, value, sizeof(out->weights));
		} else {
			return qfalse;
		}
	}
}

/*
==================
Deterministic per-bot random numbers (xorshift32)
==================
*/
unsigned Fable_RandomNext( unsigned *state ) {
	unsigned x = *state;
	if (!x) x = 0x2545F491u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

float Fable_RandomFloat( unsigned *state ) {
	return (Fable_RandomNext(state) >> 8) / 16777216.0f;
}

/*
==================
Weapon knowledge (matches g_missile.c and g_weapon.c)
==================
*/
float Fable_ProjectileSpeed( int weapon ) {
	switch (weapon) {
	case WP_ROCKET_LAUNCHER: return 900;
	case WP_GRENADE_LAUNCHER: return 700;
	case WP_PLASMAGUN: return 2000;
	case WP_BFG: return 2000;
	default: return 0;
	}
}

float Fable_SplashRadius( int weapon ) {
	switch (weapon) {
	case WP_ROCKET_LAUNCHER: return 120;
	case WP_GRENADE_LAUNCHER: return 150;
	case WP_BFG: return 120;
	default: return 0;
	}
}

float Fable_IdealRange( int weapon ) {
	switch (weapon) {
	case WP_GAUNTLET: return 32;
	case WP_SHOTGUN: return 160;
	case WP_LIGHTNING: return 320;
	case WP_MACHINEGUN: return 400;
	case WP_PLASMAGUN: return 320;
	case WP_GRENADE_LAUNCHER: return 320;
	case WP_ROCKET_LAUNCHER: return 420;
	case WP_BFG: return 500;
	case WP_RAILGUN: return 800;
	default: return 350;
	}
}

float Fable_AimTolerance( int weapon ) {
	switch (weapon) {
	case WP_RAILGUN: return 1.5f;
	case WP_MACHINEGUN: return 3;
	case WP_LIGHTNING: return 3;
	case WP_PLASMAGUN: return 4;
	case WP_ROCKET_LAUNCHER: return 5;
	case WP_BFG: return 5;
	case WP_SHOTGUN: return 6;
	case WP_GRENADE_LAUNCHER: return 8;
	case WP_GAUNTLET: return 15;
	default: return 5;
	}
}

/* Preference per weapon and distance band: under 100, 450, 900, and beyond. */
static const int fableWeaponScores[WP_NUM_WEAPONS][4] = {
	{0, 0, 0, 0},      /* WP_NONE */
	{3, 0, 0, 0},      /* WP_GAUNTLET */
	{5, 5, 5, 5},      /* WP_MACHINEGUN */
	{9, 6, 3, 1},      /* WP_SHOTGUN */
	{1, 4, 3, 1},      /* WP_GRENADE_LAUNCHER */
	{2, 9, 8, 6},      /* WP_ROCKET_LAUNCHER */
	{10, 9, 4, 0},     /* WP_LIGHTNING */
	{6, 7, 9, 10},     /* WP_RAILGUN */
	{7, 8, 6, 5},      /* WP_PLASMAGUN */
	{2, 9, 9, 8},      /* WP_BFG */
	{0, 0, 0, 0},      /* WP_GRAPPLING_HOOK */
#ifdef MISSIONPACK
	{6, 7, 5, 3},      /* WP_NAILGUN */
	{1, 3, 2, 1},      /* WP_PROX_LAUNCHER */
	{7, 8, 7, 5},      /* WP_CHAINGUN */
#endif
};

int Fable_ChooseWeapon( const playerState_t *ps, float dist ) {
	int weapon, band, best = WP_NONE, bestScore = -1, score;
	if (dist < 100) band = 0;
	else if (dist < 450) band = 1;
	else if (dist < 900) band = 2;
	else band = 3;
	for (weapon = WP_GAUNTLET; weapon < WP_NUM_WEAPONS; weapon++) {
		if (!(ps->stats[STAT_WEAPONS] & (1 << weapon))) continue;
		if (ps->ammo[weapon] == 0 || weapon == WP_GRAPPLING_HOOK) continue;
		score = fableWeaponScores[weapon][band];
		if (score > bestScore) {
			bestScore = score;
			best = weapon;
		}
	}
	return best;
}

/*
==================
Aiming
==================
*/
void Fable_LeadTarget( const vec3_t eye, const vec3_t target, const vec3_t velocity, float projectileSpeed, vec3_t out ) {
	vec3_t dir;
	float time;
	int i;
	VectorCopy(target, out);
	if (projectileSpeed <= 0) return;
	VectorSubtract(target, eye, dir);
	time = VectorLength(dir) / projectileSpeed;
	for (i = 0; i < 4; i++) {
		VectorMA(target, time, velocity, out);
		VectorSubtract(out, eye, dir);
		time = VectorLength(dir) / projectileSpeed;
	}
}

void Fable_TurnTowards( const vec3_t current, const vec3_t ideal, float maxDegrees, vec3_t out ) {
	int i;
	float delta;
	for (i = 0; i < 2; i++) {
		delta = AngleSubtract(ideal[i], current[i]);
		if (delta > maxDegrees) delta = maxDegrees;
		if (delta < -maxDegrees) delta = -maxDegrees;
		out[i] = AngleNormalize180(current[i] + delta);
	}
	out[ROLL] = 0;
}

/*
==================
Perception
==================
*/
float Fable_PerceptionFov( float baseFov, qboolean tookDamage, qboolean enemyFiring, float dist ) {
	if (tookDamage || enemyFiring || dist < FABLE_HEARING_RANGE) return 360;
	return baseFov;
}

qboolean Fable_InFov( const vec3_t viewangles, float fov, const vec3_t dir ) {
	vec3_t angles;
	int i;
	if (fov >= 360) return qtrue;
	vectoangles(dir, angles);
	for (i = 0; i < 2; i++) {
		if (Q_fabs(AngleSubtract(angles[i], viewangles[i])) > fov * 0.5f) return qfalse;
	}
	return qtrue;
}

int Fable_ChooseTarget( const fableCandidate_t *candidates, int count ) {
	int i, best = -1;
	float score, bestScore = 0;
	for (i = 0; i < count; i++) {
		score = candidates[i].dist;
		if (candidates[i].isCurrent) score *= 0.6f;
		if (candidates[i].carriesFlag) score *= 0.25f;
		if (best < 0 || score < bestScore) {
			best = i;
			bestScore = score;
		}
	}
	return best;
}

/*
==================
Fire control
==================
*/
qboolean Fable_ShouldFire( int weapon, const vec3_t viewDir, const vec3_t aimDir, int hitEntity, int targetEntity, float impactDist, float impactToTarget ) {
	float splash;
	/* Compare cosines: the QVM libc has no acos. */
	if (DotProduct(viewDir, aimDir) < cos(DEG2RAD(Fable_AimTolerance(weapon)))) return qfalse;
	if (weapon == WP_GAUNTLET && impactDist > FABLE_GAUNTLET_RANGE) return qfalse;
	if (weapon == WP_LIGHTNING && impactDist > FABLE_LIGHTNING_RANGE) return qfalse;
	splash = Fable_SplashRadius(weapon);
	if (splash > 0 && impactDist < splash + FABLE_SELF_SPLASH_MARGIN) return qfalse;
	if (hitEntity == targetEntity) return qtrue;
	if (splash > 0 && impactToTarget < splash * 0.75f) return qtrue;
	return qfalse;
}

/*
==================
Botlib inventory (mirrors the stock BotUpdateInventory)
==================
*/
void Fable_BuildInventory( const playerState_t *ps, int *inventory ) {
	memset(inventory, 0, sizeof(int) * FABLE_INVENTORY_SIZE);
	inventory[INVENTORY_ARMOR] = ps->stats[STAT_ARMOR];
	inventory[INVENTORY_GAUNTLET] = (ps->stats[STAT_WEAPONS] & (1 << WP_GAUNTLET)) != 0;
	inventory[INVENTORY_SHOTGUN] = (ps->stats[STAT_WEAPONS] & (1 << WP_SHOTGUN)) != 0;
	inventory[INVENTORY_MACHINEGUN] = (ps->stats[STAT_WEAPONS] & (1 << WP_MACHINEGUN)) != 0;
	inventory[INVENTORY_GRENADELAUNCHER] = (ps->stats[STAT_WEAPONS] & (1 << WP_GRENADE_LAUNCHER)) != 0;
	inventory[INVENTORY_ROCKETLAUNCHER] = (ps->stats[STAT_WEAPONS] & (1 << WP_ROCKET_LAUNCHER)) != 0;
	inventory[INVENTORY_LIGHTNING] = (ps->stats[STAT_WEAPONS] & (1 << WP_LIGHTNING)) != 0;
	inventory[INVENTORY_RAILGUN] = (ps->stats[STAT_WEAPONS] & (1 << WP_RAILGUN)) != 0;
	inventory[INVENTORY_PLASMAGUN] = (ps->stats[STAT_WEAPONS] & (1 << WP_PLASMAGUN)) != 0;
	inventory[INVENTORY_BFG10K] = (ps->stats[STAT_WEAPONS] & (1 << WP_BFG)) != 0;
	inventory[INVENTORY_GRAPPLINGHOOK] = (ps->stats[STAT_WEAPONS] & (1 << WP_GRAPPLING_HOOK)) != 0;
#ifdef MISSIONPACK
	inventory[INVENTORY_NAILGUN] = (ps->stats[STAT_WEAPONS] & (1 << WP_NAILGUN)) != 0;
	inventory[INVENTORY_PROXLAUNCHER] = (ps->stats[STAT_WEAPONS] & (1 << WP_PROX_LAUNCHER)) != 0;
	inventory[INVENTORY_CHAINGUN] = (ps->stats[STAT_WEAPONS] & (1 << WP_CHAINGUN)) != 0;
#endif
	inventory[INVENTORY_SHELLS] = ps->ammo[WP_SHOTGUN];
	inventory[INVENTORY_BULLETS] = ps->ammo[WP_MACHINEGUN];
	inventory[INVENTORY_GRENADES] = ps->ammo[WP_GRENADE_LAUNCHER];
	inventory[INVENTORY_CELLS] = ps->ammo[WP_PLASMAGUN];
	inventory[INVENTORY_LIGHTNINGAMMO] = ps->ammo[WP_LIGHTNING];
	inventory[INVENTORY_ROCKETS] = ps->ammo[WP_ROCKET_LAUNCHER];
	inventory[INVENTORY_SLUGS] = ps->ammo[WP_RAILGUN];
	inventory[INVENTORY_BFGAMMO] = ps->ammo[WP_BFG];
#ifdef MISSIONPACK
	inventory[INVENTORY_NAILS] = ps->ammo[WP_NAILGUN];
	inventory[INVENTORY_MINES] = ps->ammo[WP_PROX_LAUNCHER];
	inventory[INVENTORY_BELT] = ps->ammo[WP_CHAINGUN];
#endif
	inventory[INVENTORY_HEALTH] = ps->stats[STAT_HEALTH];
	inventory[INVENTORY_TELEPORTER] = ps->stats[STAT_HOLDABLE_ITEM] == MODELINDEX_TELEPORTER;
	inventory[INVENTORY_MEDKIT] = ps->stats[STAT_HOLDABLE_ITEM] == MODELINDEX_MEDKIT;
#ifdef MISSIONPACK
	inventory[INVENTORY_KAMIKAZE] = ps->stats[STAT_HOLDABLE_ITEM] == MODELINDEX_KAMIKAZE;
	inventory[INVENTORY_PORTAL] = ps->stats[STAT_HOLDABLE_ITEM] == MODELINDEX_PORTAL;
	inventory[INVENTORY_INVULNERABILITY] = ps->stats[STAT_HOLDABLE_ITEM] == MODELINDEX_INVULNERABILITY;
#endif
	inventory[INVENTORY_QUAD] = ps->powerups[PW_QUAD] != 0;
	inventory[INVENTORY_ENVIRONMENTSUIT] = ps->powerups[PW_BATTLESUIT] != 0;
	inventory[INVENTORY_HASTE] = ps->powerups[PW_HASTE] != 0;
	inventory[INVENTORY_INVISIBILITY] = ps->powerups[PW_INVIS] != 0;
	inventory[INVENTORY_REGEN] = ps->powerups[PW_REGEN] != 0;
	inventory[INVENTORY_FLIGHT] = ps->powerups[PW_FLIGHT] != 0;
#ifdef MISSIONPACK
	inventory[INVENTORY_SCOUT] = ps->stats[STAT_PERSISTANT_POWERUP] == MODELINDEX_SCOUT;
	inventory[INVENTORY_GUARD] = ps->stats[STAT_PERSISTANT_POWERUP] == MODELINDEX_GUARD;
	inventory[INVENTORY_DOUBLER] = ps->stats[STAT_PERSISTANT_POWERUP] == MODELINDEX_DOUBLER;
	inventory[INVENTORY_AMMOREGEN] = ps->stats[STAT_PERSISTANT_POWERUP] == MODELINDEX_AMMOREGEN;
#endif
	inventory[INVENTORY_REDFLAG] = ps->powerups[PW_REDFLAG] != 0;
	inventory[INVENTORY_BLUEFLAG] = ps->powerups[PW_BLUEFLAG] != 0;
#ifdef MISSIONPACK
	inventory[INVENTORY_NEUTRALFLAG] = ps->powerups[PW_NEUTRALFLAG] != 0;
#endif
}

/*
==================
Dodging and stuck detection
==================
*/
qboolean Fable_MissileThreat( const vec3_t self, const vec3_t missile, const vec3_t velocity, vec3_t sideDir ) {
	vec3_t dir, toSelf, closest, up = {0, 0, 1};
	float speed, along, miss;
	VectorCopy(velocity, dir);
	speed = VectorNormalize(dir);
	if (speed < 1) return qfalse;
	VectorSubtract(self, missile, toSelf);
	along = DotProduct(toSelf, dir);
	if (along < 0 || along / speed > FABLE_MISSILE_TIME) return qfalse;
	VectorMA(toSelf, -along, dir, closest);
	miss = VectorLength(closest);
	if (miss > FABLE_MISSILE_MISS_RADIUS) return qfalse;
	/* Step out of the path horizontally, away from where it passes. */
	closest[2] = 0;
	if (VectorNormalize(closest) < 0.01f) {
		CrossProduct(dir, up, closest);
		closest[2] = 0;
		VectorNormalize(closest);
	}
	VectorCopy(closest, sideDir);
	return qtrue;
}

qboolean Fable_UpdateStuck( fableStuck_t *stuck, const vec3_t origin, int time, qboolean wantsToMove ) {
	vec3_t delta;
	if (!wantsToMove || !stuck->since) {
		VectorCopy(origin, stuck->origin);
		stuck->since = time;
		return qfalse;
	}
	VectorSubtract(origin, stuck->origin, delta);
	if (VectorLength(delta) >= FABLE_STUCK_DISTANCE) {
		VectorCopy(origin, stuck->origin);
		stuck->since = time;
		return qfalse;
	}
	return time - stuck->since >= FABLE_STUCK_TIME;
}

/*
==================
Controller state
==================
*/
#define FABLE_THINK_INTERVAL 50
#define FABLE_CHASE_TIME 2000
#define FABLE_LTG_TIME 20000
#define FABLE_NBG_TIME 6000
#define FABLE_NBG_RANGE 200.0f
#define FABLE_NBG_CHECK_INTERVAL 500
#define FABLE_DODGE_TIME 400
#define FABLE_UNSTICK_TIME 700
#define FABLE_BLOCKED_TIME 400
#define FABLE_ROAM_TIME 1000
#define FABLE_TRACK_TIME 1000
#define FABLE_JITTER_INTERVAL 200
#define FABLE_TRAVEL_FLAGS (TFL_DEFAULT)
/* Presence types as defined by the AAS file format. */
#define FABLE_PRESENCE_NORMAL 2
#define FABLE_PRESENCE_CROUCH 4

typedef struct {
	fableConfig_t config;
	unsigned rng;
	int ms, gs;                   /* Botlib move and goal state handles. */
	int inventory[FABLE_INVENTORY_SIZE];
	/* Enemy tracking. */
	int enemy;                    /* Entity number or -1. */
	int enemySeenTime;
	int acquiredTime;
	vec3_t enemyOrigin;
	int enemyArea;
	int lastHealth;
	/* Combat movement. */
	int strafeDir;
	int strafeFlipTime;
	int jumpTime;
	int dodgeUntil;
	vec3_t dodgeDir;
	/* Navigation. */
	bot_goal_t ltg, nbg;
	qboolean hasLtg, hasNbg;
	int ltgUntil, nbgUntil, nbgCheckTime;
	fableStuck_t stuck;
	int unstickUntil;
	vec3_t unstickDir;
	int blockedSince;             /* Zero when the last move was not blocked. */
	int avoidDir;                 /* Sideways direction used to pass obstacles. */
	/* Aim noise for lower skills. */
	float jitterPitch, jitterYaw;
	int jitterTime;
	int attackToggle;
	/* Diagnostics, printed when the fable_debug cvar is set. */
	int debugTime;
	int thinks, fightThinks, chaseThinks, navThinks, roamThinks, dodgeThinks, unstickThinks;
	int stuckEvents, goalFailures, shots;
} fableContext_t;

/* State belongs to each bot, not the provider. Avoid per-attach G_Alloc leaks. */
static fableContext_t fableContexts[MAX_CLIENTS];

static qboolean FableCreate( int client, const char *config, void **context ) {
	fableContext_t *ctx;
	char filename[MAX_QPATH];

	if (client < 0 || client >= MAX_CLIENTS) return qfalse;
	ctx = &fableContexts[client];
	memset(ctx, 0, sizeof(*ctx));
	if (!Fable_ParseConfig(config, client, &ctx->config)) return qfalse;
	ctx->rng = (unsigned) ctx->config.seed;
	ctx->enemy = -1;
	ctx->strafeDir = 1;
	ctx->avoidDir = 1;

	ctx->gs = trap_BotAllocGoalState(client);
	if (!ctx->gs) return qfalse;
	Com_sprintf(filename, sizeof(filename), "bots/%s_i.c", ctx->config.weights);
	if (trap_BotLoadItemWeights(ctx->gs, filename) != BLERR_NOERROR) {
		trap_BotFreeGoalState(ctx->gs);
		return qfalse;
	}
	ctx->ms = trap_BotAllocMoveState();
	if (!ctx->ms) {
		trap_BotFreeGoalState(ctx->gs);
		return qfalse;
	}
	*context = ctx;
	return qtrue;
}

static void FableDestroy( int client, void *context ) {
	fableContext_t *ctx = context;
	if (!ctx) return;
	if (ctx->ms) trap_BotFreeMoveState(ctx->ms);
	if (ctx->gs) trap_BotFreeGoalState(ctx->gs);
	ctx->ms = ctx->gs = 0;
}

/* A new life, team, or movement mode invalidates navigation and tracking. */
static void FableReset( fableContext_t *ctx, const botObservation_t *obs ) {
	trap_BotResetMoveState(ctx->ms);
	trap_BotResetAvoidReach(ctx->ms);
	trap_BotResetGoalState(ctx->gs);
	trap_BotResetAvoidGoals(ctx->gs);
	ctx->enemy = -1;
	ctx->enemySeenTime = 0;
	ctx->lastHealth = obs->self.stats[STAT_HEALTH];
	ctx->hasLtg = ctx->hasNbg = qfalse;
	ctx->ltgUntil = ctx->nbgUntil = ctx->nbgCheckTime = 0;
	ctx->dodgeUntil = ctx->unstickUntil = ctx->blockedSince = 0;
	ctx->strafeDir = 1;
	ctx->avoidDir = 1;
	ctx->strafeFlipTime = 0;
	memset(&ctx->stuck, 0, sizeof(ctx->stuck));
}

/*
==================
Botlib helpers
==================
*/
static int FableAreaNum( const vec3_t origin ) {
	vec3_t start, end;
	int areas[10], areanum;
	VectorCopy(origin, start);
	areanum = trap_AAS_PointAreaNum(start);
	if (areanum) return areanum;
	VectorCopy(origin, end);
	end[2] += 10;
	if (trap_AAS_TraceAreas(start, end, areas, NULL, 10) > 0) return areas[0];
	return 0;
}

static void FableInitMove( fableContext_t *ctx, const botObservation_t *obs, int dt ) {
	const playerState_t *ps = &obs->self;
	bot_initmove_t initmove;
	memset(&initmove, 0, sizeof(initmove));
	VectorCopy(ps->origin, initmove.origin);
	VectorCopy(ps->velocity, initmove.velocity);
	initmove.viewoffset[2] = ps->viewheight;
	initmove.entitynum = obs->client;
	initmove.client = obs->client;
	initmove.thinktime = dt * 0.001f;
	if (ps->groundEntityNum != ENTITYNUM_NONE) initmove.or_moveflags |= MFL_ONGROUND;
	if ((ps->pm_flags & PMF_TIME_KNOCKBACK) && ps->pm_time > 0) initmove.or_moveflags |= MFL_TELEPORTED;
	if ((ps->pm_flags & PMF_TIME_WATERJUMP) && ps->pm_time > 0) initmove.or_moveflags |= MFL_WATERJUMP;
	initmove.presencetype = (ps->pm_flags & PMF_DUCKED) ? FABLE_PRESENCE_CROUCH : FABLE_PRESENCE_NORMAL;
	VectorCopy(ps->viewangles, initmove.viewangles);
	trap_BotInitMoveState(ctx->ms, &initmove);
}

static void FableRandomDirection( fableContext_t *ctx, vec3_t dir ) {
	float yaw = Fable_RandomFloat(&ctx->rng) * 360;
	dir[0] = cos(DEG2RAD(yaw));
	dir[1] = sin(DEG2RAD(yaw));
	dir[2] = 0;
}

/*
==================
Perception
==================
*/
static qboolean FableVisible( int client, const vec3_t eye, const botObservedEntity_t *ent ) {
	static const float heights[3] = {8, 24, -16};
	trace_t trace;
	vec3_t end;
	int i;
	for (i = 0; i < 3; i++) {
		VectorCopy(ent->origin, end);
		end[2] += heights[i];
		trap_Trace(&trace, eye, NULL, NULL, end, client, MASK_SOLID | CONTENTS_PLAYERCLIP);
		if (trace.fraction >= 1 || trace.entityNum == ent->state.number) return qtrue;
	}
	return qfalse;
}

static qboolean FablePerceive( fableContext_t *ctx, const botObservation_t *obs, const vec3_t eye, qboolean tookDamage, botObservedEntity_t *enemy ) {
	fableCandidate_t candidates[MAX_CLIENTS];
	botObservedEntity_t ent;
	vec3_t dir;
	float dist, fov;
	int i, count = 0, best, limit;

	limit = obs->numEntities < MAX_CLIENTS ? obs->numEntities : MAX_CLIENTS;
	for (i = 0; i < limit; i++) {
		if (i == obs->client || !BotController_GetEntity(i, &ent)) continue;
		if (ent.state.eType != ET_PLAYER) continue;
		if ((ent.state.eFlags & EF_DEAD) || ent.health <= 0) continue;
		if (obs->gametype >= GT_TEAM && ent.team == obs->self.persistant[PERS_TEAM]) continue;
		VectorSubtract(ent.origin, eye, dir);
		dist = VectorLength(dir);
		fov = Fable_PerceptionFov(ctx->config.fov, tookDamage, (ent.state.eFlags & EF_FIRING) != 0, dist);
		/* Keep tracking a target that just slipped out of view. */
		if (i == ctx->enemy && obs->time - ctx->enemySeenTime < FABLE_TRACK_TIME) fov = 360;
		if (!Fable_InFov(obs->self.viewangles, fov, dir)) continue;
		if (!FableVisible(obs->client, eye, &ent)) continue;
		candidates[count].entity = i;
		candidates[count].dist = dist;
		candidates[count].carriesFlag = (ent.state.powerups & ((1 << PW_REDFLAG) | (1 << PW_BLUEFLAG))) != 0;
		candidates[count].isCurrent = i == ctx->enemy;
		count++;
	}
	best = Fable_ChooseTarget(candidates, count);
	if (best < 0) return qfalse;
	i = candidates[best].entity;
	if (i != ctx->enemy) {
		ctx->enemy = i;
		ctx->acquiredTime = obs->time;
	}
	if (!BotController_GetEntity(i, enemy)) return qfalse;
	ctx->enemySeenTime = obs->time;
	VectorCopy(enemy->origin, ctx->enemyOrigin);
	ctx->enemyArea = FableAreaNum(enemy->origin);
	return qtrue;
}

/*
==================
Aiming and fire control
==================
*/
static void FableAim( fableContext_t *ctx, const botObservation_t *obs, const vec3_t eye, const botObservedEntity_t *enemy, int weapon, vec3_t aimPoint, vec3_t ideal ) {
	trace_t trace;
	vec3_t dir, feet;
	float amplitude;

	Fable_LeadTarget(eye, enemy->origin, enemy->state.pos.trDelta, Fable_ProjectileSpeed(weapon), aimPoint);
	if (Fable_SplashRadius(weapon) > 0 && enemy->state.groundEntityNum != ENTITYNUM_NONE && aimPoint[2] < eye[2] + 32) {
		/* Splash weapons: put the projectile at the feet so near misses still hurt. */
		VectorCopy(aimPoint, feet);
		feet[2] -= 22;
		trap_Trace(&trace, eye, NULL, NULL, feet, obs->client, MASK_SOLID);
		if (trace.fraction >= 0.95f) VectorCopy(feet, aimPoint);
		else aimPoint[2] += 8;
	} else {
		aimPoint[2] += 8;
	}
	VectorSubtract(aimPoint, eye, dir);
	vectoangles(dir, ideal);
	if (ctx->config.skill < 1) {
		if (obs->time >= ctx->jitterTime) {
			ctx->jitterTime = obs->time + FABLE_JITTER_INTERVAL;
			amplitude = (1 - ctx->config.skill) * 10;
			ctx->jitterPitch = amplitude * (Fable_RandomFloat(&ctx->rng) * 2 - 1);
			ctx->jitterYaw = amplitude * (Fable_RandomFloat(&ctx->rng) * 2 - 1);
		}
		ideal[PITCH] += ctx->jitterPitch;
		ideal[YAW] += ctx->jitterYaw;
	}
	ideal[ROLL] = 0;
}

static qboolean FableWantsToFire( fableContext_t *ctx, const botObservation_t *obs, const vec3_t eye, const botObservedEntity_t *enemy, int weapon, const vec3_t viewangles, const vec3_t aimPoint ) {
	trace_t trace;
	vec3_t viewDir, aimDir, end, delta;
	float aimDist, length, impactDist, impactToTarget;

	if (obs->time < ctx->acquiredTime + (int) ((1 - ctx->config.skill) * 500)) return qfalse;
	AngleVectors(viewangles, viewDir, NULL, NULL);
	VectorSubtract(aimPoint, eye, aimDir);
	aimDist = VectorNormalize(aimDir);
	length = aimDist + 64;
	VectorMA(eye, length, viewDir, end);
	trap_Trace(&trace, eye, NULL, NULL, end, obs->client, MASK_SHOT);
	impactDist = trace.fraction * length;
	if (trace.fraction >= 1) {
		impactToTarget = 1e6f; /* Nothing hit: only a direct hit could count. */
	} else {
		VectorSubtract(trace.endpos, enemy->origin, delta);
		impactToTarget = VectorLength(delta);
	}
	return Fable_ShouldFire(weapon, viewDir, aimDir, trace.entityNum, enemy->state.number, impactDist, impactToTarget);
}

/*
==================
Movement
==================
*/
static qboolean FableDodge( fableContext_t *ctx, const botObservation_t *obs ) {
	botObservedEntity_t ent;
	vec3_t side;
	int i;
	if (obs->time < ctx->dodgeUntil) return qtrue;
	if (obs->self.groundEntityNum == ENTITYNUM_NONE) return qfalse;
	for (i = MAX_CLIENTS; i < obs->numEntities; i++) {
		if (!BotController_GetEntity(i, &ent) || ent.state.eType != ET_MISSILE) continue;
		if (ent.state.weapon != WP_ROCKET_LAUNCHER && ent.state.weapon != WP_GRENADE_LAUNCHER &&
			ent.state.weapon != WP_BFG && ent.state.weapon != WP_PLASMAGUN) continue;
		if (!Fable_MissileThreat(obs->self.origin, ent.origin, ent.state.pos.trDelta, side)) continue;
		if (Fable_RandomFloat(&ctx->rng) > ctx->config.skill) continue;
		VectorCopy(side, ctx->dodgeDir);
		ctx->dodgeUntil = obs->time + FABLE_DODGE_TIME;
		return qtrue;
	}
	return qfalse;
}

static void FableAttackMove( fableContext_t *ctx, const botObservation_t *obs, const botObservedEntity_t *enemy, int weapon ) {
	vec3_t forward, side, move, up = {0, 0, 1};
	float dist, ideal;
	int movetype = MOVE_WALK, i;

	VectorSubtract(enemy->origin, obs->self.origin, forward);
	forward[2] = 0;
	dist = VectorNormalize(forward);
	ideal = Fable_IdealRange(weapon);
	if (obs->time >= ctx->strafeFlipTime) {
		ctx->strafeFlipTime = obs->time + 600 + (int) (Fable_RandomFloat(&ctx->rng) * 900);
		if (Fable_RandomFloat(&ctx->rng) < 0.6f) ctx->strafeDir = -ctx->strafeDir;
	}
	if (obs->time >= ctx->jumpTime + 900 && Fable_RandomFloat(&ctx->rng) < 0.03f * ctx->config.skill) {
		ctx->jumpTime = obs->time;
		movetype = MOVE_JUMP;
	}
	for (i = 0; i < 2; i++) {
		CrossProduct(forward, up, side);
		VectorScale(side, ctx->strafeDir, move);
		if (dist > ideal + 60) VectorAdd(move, forward, move);
		else if (dist < ideal - 60 && weapon != WP_GAUNTLET) VectorSubtract(move, forward, move);
		VectorNormalize(move);
		if (trap_BotMoveInDirection(ctx->ms, move, 400, movetype)) return;
		ctx->strafeDir = -ctx->strafeDir; /* Blocked: try the other way. */
	}
	VectorNegate(forward, move);
	trap_BotMoveInDirection(ctx->ms, move, 400, MOVE_WALK);
}

/* Moves even where the botlib refuses, such as walking off a ledge it cannot route. */
static void FableForceMove( const botObservation_t *obs, vec3_t dir ) {
	trap_EA_Move(obs->client, dir, 400);
	trap_EA_Jump(obs->client);
}

/* Mirrors the stock dynamic obstacle avoidance for blocked movement results. */
static void FableAvoidBlockage( fableContext_t *ctx, const botObservation_t *obs, const bot_moveresult_t *result, const bot_goal_t *goal, qboolean hasGoal ) {
	vec3_t hordir, sideward, up = {0, 0, 1};
	VectorCopy(result->movedir, hordir);
	hordir[2] = 0;
	if (VectorNormalize(hordir) < 0.1f) {
		if (hasGoal) {
			VectorSubtract(goal->origin, obs->self.origin, hordir);
			hordir[2] = 0;
		}
		if (!hasGoal || VectorNormalize(hordir) < 0.1f) FableRandomDirection(ctx, hordir);
	}
	if (result->flags & MOVERESULT_ONTOPOFOBSTACLE) {
		/* Standing where no route exists: get off towards the goal. */
		if (!trap_BotMoveInDirection(ctx->ms, hordir, 400, MOVE_JUMP) &&
			!trap_BotMoveInDirection(ctx->ms, hordir, 400, MOVE_WALK)) {
			FableForceMove(obs, hordir);
		}
		return;
	}
	CrossProduct(hordir, up, sideward);
	VectorScale(sideward, ctx->avoidDir, sideward);
	if (!trap_BotMoveInDirection(ctx->ms, sideward, 400, MOVE_WALK)) {
		ctx->avoidDir = -ctx->avoidDir;
		VectorMA(sideward, -1, hordir, sideward);
		trap_BotMoveInDirection(ctx->ms, sideward, 400, MOVE_WALK);
	}
}

/* Drops the current goal after the botlib reported it unreachable or reached. */
static void FableDropGoal( fableContext_t *ctx, qboolean reached ) {
	bot_goal_t *goal = ctx->hasNbg ? &ctx->nbg : &ctx->ltg;
	if (!ctx->hasNbg && !ctx->hasLtg) return;
	if (reached) trap_BotSetAvoidGoalTime(ctx->gs, goal->number, -1);
	else trap_BotSetAvoidGoalTime(ctx->gs, goal->number, 5);
	trap_BotPopGoal(ctx->gs);
	if (ctx->hasNbg) ctx->hasNbg = qfalse;
	else ctx->hasLtg = qfalse;
}

static qboolean FableGoalDone( fableContext_t *ctx, const botObservation_t *obs, const vec3_t eye, bot_goal_t *goal, int until, qboolean *reached ) {
	vec3_t origin, viewangles;
	VectorCopy(obs->self.origin, origin);
	VectorCopy(obs->self.viewangles, viewangles);
	*reached = trap_BotTouchingGoal(origin, goal) != 0;
	if (*reached) return qtrue;
	if (obs->time > until) return qtrue;
	return trap_BotItemGoalInVisButNotVisible(obs->client, (float *) eye, viewangles, goal) != 0;
}

/* Chooses and follows item goals; returns the goal being followed, if any. */
static qboolean FableNavigate( fableContext_t *ctx, const botObservation_t *obs, const vec3_t eye, qboolean chase, bot_moveresult_t *result, bot_goal_t *goalOut ) {
	bot_goal_t goal;
	vec3_t origin;
	qboolean reached;

	memset(result, 0, sizeof(*result));
	VectorCopy(obs->self.origin, origin);
	if (chase && ctx->enemyArea) {
		memset(&goal, 0, sizeof(goal));
		goal.entitynum = ctx->enemy;
		goal.areanum = ctx->enemyArea;
		VectorCopy(ctx->enemyOrigin, goal.origin);
		VectorSet(goal.mins, -8, -8, -8);
		VectorSet(goal.maxs, 8, 8, 8);
		trap_BotMoveToGoal(result, ctx->ms, &goal, FABLE_TRAVEL_FLAGS);
		if (result->failure) ctx->enemySeenTime = 0;
		*goalOut = goal;
		return qtrue;
	}
	if (ctx->hasNbg && FableGoalDone(ctx, obs, eye, &ctx->nbg, ctx->nbgUntil, &reached)) FableDropGoal(ctx, reached);
	if (!ctx->hasNbg) {
		if (ctx->hasLtg && FableGoalDone(ctx, obs, eye, &ctx->ltg, ctx->ltgUntil, &reached)) FableDropGoal(ctx, reached);
		if (!ctx->hasLtg) {
			if (trap_BotChooseLTGItem(ctx->gs, origin, ctx->inventory, FABLE_TRAVEL_FLAGS) && trap_BotGetTopGoal(ctx->gs, &ctx->ltg)) {
				ctx->hasLtg = qtrue;
				ctx->ltgUntil = obs->time + FABLE_LTG_TIME;
			} else {
				trap_BotResetAvoidGoals(ctx->gs);
				trap_BotResetAvoidReach(ctx->ms);
			}
		}
		if (ctx->hasLtg && obs->time >= ctx->nbgCheckTime) {
			ctx->nbgCheckTime = obs->time + FABLE_NBG_CHECK_INTERVAL;
			if (trap_BotChooseNBGItem(ctx->gs, origin, ctx->inventory, FABLE_TRAVEL_FLAGS, &ctx->ltg, FABLE_NBG_RANGE) &&
				trap_BotGetTopGoal(ctx->gs, &ctx->nbg)) {
				ctx->hasNbg = qtrue;
				ctx->nbgUntil = obs->time + FABLE_NBG_TIME;
			}
		}
	}
	if (!ctx->hasNbg && !ctx->hasLtg) {
		/* Nothing worth walking to: wander until the avoid timers expire. */
		if (obs->time >= ctx->unstickUntil) {
			FableRandomDirection(ctx, ctx->unstickDir);
			ctx->unstickUntil = obs->time + FABLE_ROAM_TIME;
		}
		trap_BotMoveInDirection(ctx->ms, ctx->unstickDir, 400, MOVE_WALK);
		return qfalse;
	}
	goal = ctx->hasNbg ? ctx->nbg : ctx->ltg;
	trap_BotMoveToGoal(result, ctx->ms, &goal, FABLE_TRAVEL_FLAGS);
	if (result->failure) {
		trap_BotResetAvoidReach(ctx->ms);
		FableDropGoal(ctx, qfalse);
	}
	*goalOut = goal;
	return qtrue;
}

/*
==================
Diagnostics
==================
*/
static void FableDebugReport( fableContext_t *ctx, const botObservation_t *obs ) {
	if (obs->time < ctx->debugTime) return;
	ctx->debugTime = obs->time + 30000;
	if (!trap_Cvar_VariableIntegerValue("fable_debug")) return;
	G_Printf("fable %d: thinks %d fight %d chase %d nav %d roam %d dodge %d unstick %d | stuck %d goalfail %d shots %d | hp %d armor %d\n",
		obs->client, ctx->thinks, ctx->fightThinks, ctx->chaseThinks, ctx->navThinks, ctx->roamThinks,
		ctx->dodgeThinks, ctx->unstickThinks, ctx->stuckEvents, ctx->goalFailures, ctx->shots,
		obs->self.stats[STAT_HEALTH], obs->self.stats[STAT_ARMOR]);
}

/*
==================
Think
==================
*/
static qboolean FableThink( const botObservation_t *obs, botAction_t *action, void *context ) {
	fableContext_t *ctx = context;
	const playerState_t *ps = &obs->self;
	botObservedEntity_t enemy;
	bot_moveresult_t result;
	bot_goal_t goal;
	vec3_t eye, ideal, aimPoint, target, current, view, dir;
	qboolean tookDamage, enemyVisible, chase, retreat, hasGoal, wantsToMove, fire = qfalse;
	int dt, weapon;

	if (ps->pm_type == PM_DEAD) {
		/* Alternate attack/release to request respawn. */
		if (ctx->attackToggle++ & 1) action->buttons = BUTTON_ATTACK;
		return qtrue;
	}
	if (ps->pm_type != PM_NORMAL) return qfalse;

	dt = obs->elapsed > 0 ? obs->elapsed : FABLE_THINK_INTERVAL;
	if (obs->elapsed == 0) FableReset(ctx, obs);
	VectorCopy(ps->origin, eye);
	eye[2] += ps->viewheight;
	Fable_BuildInventory(ps, ctx->inventory);
	tookDamage = ps->stats[STAT_HEALTH] < ctx->lastHealth;
	ctx->lastHealth = ps->stats[STAT_HEALTH];

	enemyVisible = FablePerceive(ctx, obs, eye, tookDamage, &enemy);
	chase = !enemyVisible && ctx->enemy >= 0 && ctx->enemySeenTime && obs->time - ctx->enemySeenTime < FABLE_CHASE_TIME;
	retreat = ps->stats[STAT_HEALTH] + ps->stats[STAT_ARMOR] < 60;

	if (enemyVisible) {
		VectorSubtract(enemy.origin, eye, dir);
		weapon = Fable_ChooseWeapon(ps, VectorLength(dir));
	} else {
		weapon = Fable_ChooseWeapon(ps, 500);
	}
	if (weapon <= WP_NONE) weapon = ps->weapon;

	/* Movement: dodge, then fight, else navigate; recover when stuck. */
	FableInitMove(ctx, obs, dt);
	memset(&result, 0, sizeof(result));
	hasGoal = qfalse;
	wantsToMove = qtrue;
	ctx->thinks++;
	if (obs->time < ctx->unstickUntil && ctx->stuck.since) {
		ctx->unstickThinks++;
		if (!trap_BotMoveInDirection(ctx->ms, ctx->unstickDir, 400, MOVE_JUMP)) FableForceMove(obs, ctx->unstickDir);
	} else if (FableDodge(ctx, obs)) {
		ctx->dodgeThinks++;
		trap_BotMoveInDirection(ctx->ms, ctx->dodgeDir, 400, MOVE_WALK);
	} else if (enemyVisible && !retreat) {
		ctx->fightThinks++;
		FableAttackMove(ctx, obs, &enemy, weapon);
	} else {
		hasGoal = FableNavigate(ctx, obs, eye, chase && !retreat, &result, &goal);
		if (chase && !retreat) ctx->chaseThinks++;
		else if (hasGoal) ctx->navThinks++;
		else ctx->roamThinks++;
		if (result.failure) ctx->goalFailures++;
		if (result.flags & MOVERESULT_WAITING) wantsToMove = qfalse;
		if (result.blocked) {
			FableAvoidBlockage(ctx, obs, &result, &goal, hasGoal);
			if (!ctx->blockedSince) ctx->blockedSince = obs->time;
			else if (obs->time - ctx->blockedSince > FABLE_BLOCKED_TIME) {
				/* Still blocked: give the route up and let the goal selector pick elsewhere. */
				trap_BotResetAvoidReach(ctx->ms);
				if (hasGoal && !chase) FableDropGoal(ctx, qfalse);
				else ctx->enemySeenTime = 0;
				ctx->blockedSince = 0;
			}
		} else {
			ctx->blockedSince = 0;
		}
	}
	if (Fable_UpdateStuck(&ctx->stuck, ps->origin, obs->time, wantsToMove) && obs->time >= ctx->unstickUntil) {
		ctx->stuckEvents++;
		if (trap_Cvar_VariableIntegerValue("fable_debug")) {
			G_Printf("fable %d stuck at %d: origin %.0f %.0f %.0f area %d goal %d (%.0f %.0f %.0f area %d) flags %d travel %d type %d blocked %d fail %d chase %d visible %d\n",
				obs->client, obs->time, ps->origin[0], ps->origin[1], ps->origin[2], FableAreaNum(ps->origin),
				hasGoal ? goal.number : -1, hasGoal ? goal.origin[0] : 0, hasGoal ? goal.origin[1] : 0, hasGoal ? goal.origin[2] : 0,
				hasGoal ? goal.areanum : 0, result.flags, result.traveltype, result.type, result.blocked, result.failure, chase, enemyVisible);
		}
		trap_BotResetAvoidReach(ctx->ms);
		if (hasGoal && !chase) FableDropGoal(ctx, qfalse);
		FableRandomDirection(ctx, ctx->unstickDir);
		ctx->unstickUntil = obs->time + FABLE_UNSTICK_TIME;
		ctx->stuck.since = obs->time;
	}

	/* View: aim at the enemy, else look where the route goes and glance around. */
	VectorCopy(ps->viewangles, current);
	if (enemyVisible) {
		FableAim(ctx, obs, eye, &enemy, weapon, aimPoint, ideal);
	} else if (result.flags & MOVERESULT_MOVEMENTVIEW) {
		VectorCopy(result.ideal_viewangles, ideal);
	} else if (chase) {
		VectorSubtract(ctx->enemyOrigin, eye, dir);
		vectoangles(dir, ideal);
	} else {
		if (hasGoal && trap_BotMovementViewTarget(ctx->ms, &goal, FABLE_TRAVEL_FLAGS, 300, target)) {
			VectorSubtract(target, eye, dir);
		} else if (hasGoal) {
			VectorSubtract(goal.origin, eye, dir);
		} else {
			AngleVectors(current, dir, NULL, NULL);
		}
		dir[2] *= 0.5f; /* Keep the view mostly level while travelling. */
		vectoangles(dir, ideal);
		ideal[YAW] += 40 * sin(obs->time * 0.0025f);
	}
	Fable_TurnTowards(current, ideal, ctx->config.turnRate * dt * 0.001f, view);
	if (result.flags & MOVERESULT_MOVEMENTVIEW) VectorCopy(ideal, view);
	if (enemyVisible) fire = FableWantsToFire(ctx, obs, eye, &enemy, weapon, view, aimPoint);

	/* Convert the botlib movement relative to the final view angles. */
	trap_EA_View(obs->client, view);
	trap_EA_SelectWeapon(obs->client, weapon);
	BotController_ReadBotlibAction(obs->client, action);
	VectorCopy(view, action->viewangles);
	action->weapon = weapon;
	if (fire) {
		action->buttons |= BUTTON_ATTACK;
		ctx->shots++;
	}
	FableDebugReport(ctx, obs);
	return qtrue;
}

const botController_t fableController = {
	BOT_CONTROLLER_API_VERSION, "fable", FABLE_THINK_INTERVAL, FableCreate, FableThink, FableDestroy
};
