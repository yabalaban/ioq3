/* Standalone tests for the fable controller's decision helpers. */
#include "../code/game/g_local.h"
#include "../code/game/g_botapi.h"
#include "../code/game/g_botfable.h"
#include "../code/game/inv.h"
#include "../code/botlib/botlib.h"
#include "../code/botlib/be_aas.h"
#include "../code/botlib/be_ai_goal.h"
#include "../code/botlib/be_ai_move.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Engine boundary stubs. */
void QDECL G_Printf( const char *fmt, ... ) {}
void QDECL Com_Error( int level, const char *error, ... ) { assert(0); }
void QDECL Com_Printf( const char *msg, ... ) {}

static qboolean Near( float a, float b, float tolerance ) { return fabsf(a - b) <= tolerance; }

/* Botlib and engine stubs recording what the controller asked for. */
static int allocs, frees, itemWeightsError, moveResets, goalResets;
static char itemFile[MAX_QPATH];
static botObservedEntity_t stubEntities[MAX_GENTITIES];
static qboolean stubPresent[MAX_GENTITIES];
static vec3_t lastMoveDir, eaMoveDir;
static float lastMoveSpeed;
static int moveCalls, moveOk = 1, blockedResult, blockedFlags, popCalls, eaMoveCalls, eaJumpCalls;
static bot_goal_t stubGoal;
static qboolean stubHasGoal;

int trap_BotAllocGoalState( int state ) { allocs++; return 100 + allocs; }
void trap_BotFreeGoalState( int handle ) { assert(handle > 100); frees++; }
int trap_BotAllocMoveState( void ) { allocs++; return 100 + allocs; }
void trap_BotFreeMoveState( int handle ) { assert(handle > 100); frees++; }
int trap_BotLoadItemWeights( int goalstate, char *filename ) { Q_strncpyz(itemFile, filename, sizeof(itemFile)); return itemWeightsError; }
void trap_BotResetMoveState( int movestate ) { moveResets++; }
void trap_BotResetAvoidReach( int movestate ) {}
void trap_BotResetGoalState( int goalstate ) { goalResets++; }
void trap_BotResetAvoidGoals( int goalstate ) {}
void trap_BotInitMoveState( int handle, void *initmove ) {}
void trap_BotMoveToGoal( void *result, int movestate, void *goal, int travelflags ) {
	bot_moveresult_t *r = result;
	memset(r, 0, sizeof(*r));
	r->blocked = blockedResult;
	r->flags = blockedFlags;
}
int trap_BotMoveInDirection( int movestate, vec3_t dir, float speed, int type ) { VectorCopy(dir, lastMoveDir); lastMoveSpeed = speed; moveCalls++; return moveOk; }
int trap_BotMovementViewTarget( int movestate, void *goal, int travelflags, float lookahead, vec3_t target ) { return 0; }
int trap_BotChooseLTGItem( int goalstate, vec3_t origin, int *inventory, int travelflags ) { return stubHasGoal; }
int trap_BotChooseNBGItem( int goalstate, vec3_t origin, int *inventory, int travelflags, void *ltg, float maxtime ) { return 0; }
int trap_BotGetTopGoal( int goalstate, void *goal ) { if (stubHasGoal) memcpy(goal, &stubGoal, sizeof(stubGoal)); return stubHasGoal; }
void trap_BotPopGoal( int goalstate ) { popCalls++; }
int trap_BotTouchingGoal( vec3_t origin, void *goal ) { return 0; }
int trap_BotItemGoalInVisButNotVisible( int viewer, vec3_t eye, vec3_t viewangles, void *goal ) { return 0; }
void trap_BotSetAvoidGoalTime( int goalstate, int number, float avoidtime ) {}
int trap_AAS_PointAreaNum( vec3_t point ) { return 1; }
int trap_AAS_TraceAreas( vec3_t start, vec3_t end, int *areas, vec3_t *points, int maxareas ) { return 0; }
void trap_EA_Move( int client, vec3_t dir, float speed ) { VectorCopy(dir, eaMoveDir); eaMoveCalls++; }
void trap_EA_Jump( int client ) { eaJumpCalls++; }
void trap_EA_Crouch( int client ) {}
void trap_EA_Attack( int client ) {}
void trap_EA_View( int client, vec3_t viewangles ) {}
void trap_EA_SelectWeapon( int client, int weapon ) {}
int trap_Cvar_VariableIntegerValue( const char *var_name ) { return 0; }

qboolean BotController_GetEntity( int entity, botObservedEntity_t *observation ) {
	if (entity < 0 || entity >= MAX_GENTITIES || !stubPresent[entity]) return qfalse;
	*observation = stubEntities[entity];
	return qtrue;
}

qboolean BotController_ReadBotlibAction( int client, botAction_t *action ) {
	action->forward = lastMoveSpeed > 0 ? 127 : 0;
	return qtrue;
}

/* Clear world; a segment passing within 24 units of a present player hits it. */
void trap_Trace( trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentmask ) {
	vec3_t dir, rel, closest;
	float len, along;
	int i;
	memset(results, 0, sizeof(*results));
	results->fraction = 1;
	results->entityNum = ENTITYNUM_NONE;
	VectorCopy(end, results->endpos);
	VectorSubtract(end, start, dir);
	len = VectorNormalize(dir);
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (!stubPresent[i] || i == passEntityNum) continue;
		VectorSubtract(stubEntities[i].origin, start, rel);
		along = DotProduct(rel, dir);
		if (along < 0 || along > len + 24) continue;
		VectorMA(rel, -along, dir, closest);
		if (VectorLength(closest) > 24) continue;
		results->entityNum = i;
		results->fraction = along / len;
		VectorMA(start, along, dir, results->endpos);
		return;
	}
}

static void ResetStubs( void ) {
	allocs = frees = itemWeightsError = moveResets = goalResets = moveCalls = 0;
	moveOk = 1;
	blockedResult = blockedFlags = popCalls = eaMoveCalls = eaJumpCalls = 0;
	stubHasGoal = qfalse;
	memset(&stubGoal, 0, sizeof(stubGoal));
	VectorClear(eaMoveDir);
	memset(stubPresent, 0, sizeof(stubPresent));
	memset(stubEntities, 0, sizeof(stubEntities));
	VectorClear(lastMoveDir);
	lastMoveSpeed = 0;
}

static void SetupObservation( botObservation_t *obs, int client, int time, int elapsed ) {
	memset(obs, 0, sizeof(*obs));
	obs->client = client;
	obs->time = time;
	obs->elapsed = elapsed;
	obs->numEntities = MAX_CLIENTS + 8;
	obs->self.pm_type = PM_NORMAL;
	obs->self.viewheight = DEFAULT_VIEWHEIGHT;
	obs->self.stats[STAT_HEALTH] = 100;
	obs->self.stats[STAT_WEAPONS] = (1 << WP_MACHINEGUN) | (1 << WP_GAUNTLET);
	obs->self.ammo[WP_MACHINEGUN] = 100;
	obs->self.weapon = WP_MACHINEGUN;
	obs->self.viewangles[YAW] = 60;
	obs->self.persistant[PERS_TEAM] = TEAM_FREE;
	stubPresent[client] = qtrue;
	stubEntities[client].team = TEAM_FREE;
}

static void AddPlayer( int entity, float x, float y, float z, int team ) {
	stubPresent[entity] = qtrue;
	memset(&stubEntities[entity], 0, sizeof(stubEntities[entity]));
	VectorSet(stubEntities[entity].origin, x, y, z);
	stubEntities[entity].state.number = entity;
	stubEntities[entity].state.eType = ET_PLAYER;
	stubEntities[entity].state.groundEntityNum = ENTITYNUM_WORLD;
	stubEntities[entity].health = 100;
	stubEntities[entity].team = team;
}

static void RunThink( void *ctx, botObservation_t *obs, botAction_t *action ) {
	memset(action, 0, sizeof(*action));
	VectorCopy(obs->self.viewangles, action->viewangles);
	assert(fableController.think(obs, action, ctx));
	/* The server applies the requested view before the next think. */
	VectorCopy(action->viewangles, obs->self.viewangles);
	obs->elapsed = fableController.thinkInterval;
	obs->time += fableController.thinkInterval;
}

static void TestDescriptor( void ) {
	assert(fableController.apiVersion == BOT_CONTROLLER_API_VERSION);
	assert(!strcmp(fableController.name, "fable"));
	assert(fableController.thinkInterval == 50);
	assert(fableController.create && fableController.think && fableController.destroy);
}

static void TestCreateBorrowsWeightFiles( void ) {
	void *ctx = NULL;
	ResetStubs();
	assert(fableController.create(2, "weights=xaero", &ctx));
	assert(ctx != NULL);
	assert(!strcmp(itemFile, "bots/xaero_i.c"));
	assert(allocs == 2 && frees == 0); /* Goal and move states; weapons are chosen in-controller. */
	fableController.destroy(2, ctx);
	assert(frees == 2);
}

static void TestCreateRejectsBadConfig( void ) {
	void *ctx = NULL;
	ResetStubs();
	assert(!fableController.create(2, "nonsense", &ctx));
	assert(allocs == 0);
}

static void TestCreateReleasesHandlesWhenWeightsFail( void ) {
	void *ctx = NULL;
	ResetStubs();
	itemWeightsError = 1;
	assert(!fableController.create(2, "", &ctx));
	assert(allocs == frees && allocs == 1);
}

static void TestDeadBotRequestsRespawn( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	int i, presses = 0;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	obs.self.pm_type = PM_DEAD;
	for (i = 0; i < 4; i++) {
		memset(&action, 0, sizeof(action));
		assert(fableController.think(&obs, &action, ctx));
		if (action.buttons & BUTTON_ATTACK) presses++;
		assert(action.forward == 0);
	}
	assert(presses == 2);
	obs.self.pm_type = PM_SPECTATOR;
	assert(!fableController.think(&obs, &action, ctx));
	fableController.destroy(0, ctx);
}

static void TestFirstThinkResetsNavigation( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	RunThink(ctx, &obs, &action);
	assert(moveResets == 1 && goalResets == 1);
	RunThink(ctx, &obs, &action);
	assert(moveResets == 1);
	obs.elapsed = 0; /* Respawn. */
	RunThink(ctx, &obs, &action);
	assert(moveResets == 2);
	fableController.destroy(0, ctx);
}

static void TestAcquiresEnemyThenFires( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	AddPlayer(1, 500, 0, 0, TEAM_FREE);
	RunThink(ctx, &obs, &action);
	/* Turning is rate limited: 900 deg/s over 50 ms brings yaw from 60 to 15. */
	assert(Near(action.viewangles[YAW], 15, 0.5f));
	assert(!(action.buttons & BUTTON_ATTACK));
	RunThink(ctx, &obs, &action);
	assert(Near(action.viewangles[YAW], 0, 0.5f));
	assert(action.buttons & BUTTON_ATTACK);
	assert(action.weapon == WP_MACHINEGUN);
	assert(moveCalls > 0); /* Strafing while fighting. */
	fableController.destroy(0, ctx);
}

static void TestHoldsFireOnTeammates( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	int i;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	obs.gametype = GT_TEAM;
	obs.self.persistant[PERS_TEAM] = TEAM_RED;
	stubEntities[0].team = TEAM_RED;
	AddPlayer(1, 500, 0, 0, TEAM_RED);
	for (i = 0; i < 4; i++) {
		RunThink(ctx, &obs, &action);
		assert(!(action.buttons & BUTTON_ATTACK));
	}
	fableController.destroy(0, ctx);
}

static void TestIgnoresDeadEnemies( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	int i;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	AddPlayer(1, 500, 0, 0, TEAM_FREE);
	stubEntities[1].state.eFlags |= EF_DEAD;
	stubEntities[1].health = 0;
	for (i = 0; i < 4; i++) {
		RunThink(ctx, &obs, &action);
		assert(!(action.buttons & BUTTON_ATTACK));
	}
	fableController.destroy(0, ctx);
}


static void TestWalksOffObstacleTowardsGoal( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	stubHasGoal = qtrue;
	stubGoal.number = 7;
	stubGoal.areanum = 350;
	VectorSet(stubGoal.origin, 300, 0, -150);
	blockedResult = 1;
	blockedFlags = MOVERESULT_ONTOPOFOBSTACLE;
	moveOk = 0; /* The botlib refuses to walk off the drop. */
	RunThink(ctx, &obs, &action);
	assert(eaMoveCalls > 0 && eaJumpCalls > 0);
	assert(eaMoveDir[0] > 0.9f && Q_fabs(eaMoveDir[1]) < 0.1f);
	fableController.destroy(0, ctx);
}

static void TestDropsGoalWhenBlockedForLong( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	int i;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	stubHasGoal = qtrue;
	stubGoal.number = 7;
	stubGoal.areanum = 350;
	VectorSet(stubGoal.origin, 300, 0, 0);
	blockedResult = 1;
	for (i = 0; i < 6; i++) RunThink(ctx, &obs, &action); /* 300 ms blocked. */
	assert(popCalls == 0);
	for (i = 0; i < 4; i++) RunThink(ctx, &obs, &action); /* Past 400 ms. */
	assert(popCalls >= 1);
	fableController.destroy(0, ctx);
}

static void TestSidestepsIncomingRocket( void ) {
	void *ctx = NULL;
	botObservation_t obs;
	botAction_t action;
	ResetStubs();
	assert(fableController.create(0, "", &ctx));
	SetupObservation(&obs, 0, 1000, 0);
	stubPresent[70] = qtrue;
	stubEntities[70].state.eType = ET_MISSILE;
	stubEntities[70].state.weapon = WP_ROCKET_LAUNCHER;
	VectorSet(stubEntities[70].origin, -400, 10, 20);
	VectorSet(stubEntities[70].state.pos.trDelta, 900, 0, 0);
	RunThink(ctx, &obs, &action);
	assert(moveCalls > 0);
	assert(Q_fabs(lastMoveDir[0]) < 0.2f && lastMoveDir[1] < -0.9f);
	fableController.destroy(0, ctx);
}


static void TestConfigDefaults( void ) {
	fableConfig_t c;
	assert(Fable_ParseConfig("", 3, &c));
	assert(Near(c.skill, 1, 0.001f));
	assert(Near(c.fov, 150, 0.001f));
	assert(Near(c.turnRate, 900, 0.001f));
	assert(!strcmp(c.weights, "xaero"));
	assert(c.seed != 0);
}

static void TestConfigParsesKeys( void ) {
	fableConfig_t c;
	assert(Fable_ParseConfig("skill=0.5 fov=120 seed=7 turn=360 weights=xaero", 0, &c));
	assert(Near(c.skill, 0.5f, 0.001f));
	assert(Near(c.fov, 120, 0.001f));
	assert(Near(c.turnRate, 360, 0.001f));
	assert(c.seed == 7);
	assert(!strcmp(c.weights, "xaero"));
}

static void TestConfigRejectsBadInput( void ) {
	fableConfig_t c;
	assert(!Fable_ParseConfig("bogus=1", 0, &c));
	assert(!Fable_ParseConfig("skill=2", 0, &c));
	assert(!Fable_ParseConfig("skill", 0, &c));
	assert(!Fable_ParseConfig("weights=../evil", 0, &c));
	assert(!Fable_ParseConfig("fov=0", 0, &c));
}

static void TestRandomIsSeededAndBounded( void ) {
	unsigned a = 42, b = 42, zero = 0;
	int i;
	for (i = 0; i < 100; i++) {
		float f = Fable_RandomFloat(&a);
		assert(f >= 0 && f < 1);
		assert(Fable_RandomFloat(&b) == f);
	}
	assert(Fable_RandomNext(&zero) != 0); /* A zero state must not get stuck. */
}

static void TestWeaponTables( void ) {
	assert(Fable_ProjectileSpeed(WP_ROCKET_LAUNCHER) == 900);
	assert(Fable_ProjectileSpeed(WP_GRENADE_LAUNCHER) == 700);
	assert(Fable_ProjectileSpeed(WP_PLASMAGUN) == 2000);
	assert(Fable_ProjectileSpeed(WP_BFG) == 2000);
	assert(Fable_ProjectileSpeed(WP_RAILGUN) == 0);
	assert(Fable_ProjectileSpeed(WP_MACHINEGUN) == 0);
	assert(Fable_SplashRadius(WP_ROCKET_LAUNCHER) == 120);
	assert(Fable_SplashRadius(WP_GRENADE_LAUNCHER) == 150);
	assert(Fable_SplashRadius(WP_RAILGUN) == 0);
	assert(Fable_IdealRange(WP_GAUNTLET) < Fable_IdealRange(WP_SHOTGUN));
	assert(Fable_IdealRange(WP_SHOTGUN) < Fable_IdealRange(WP_ROCKET_LAUNCHER));
	assert(Fable_IdealRange(WP_ROCKET_LAUNCHER) < Fable_IdealRange(WP_RAILGUN));
	assert(Fable_AimTolerance(WP_RAILGUN) < Fable_AimTolerance(WP_ROCKET_LAUNCHER));
	assert(Fable_AimTolerance(WP_ROCKET_LAUNCHER) < Fable_AimTolerance(WP_GAUNTLET));
}


static void GiveWeapon( playerState_t *ps, int weapon, int ammo ) {
	ps->stats[STAT_WEAPONS] |= 1 << weapon;
	ps->ammo[weapon] = ammo;
}

static void TestChooseWeaponByDistance( void ) {
	playerState_t ps;
	memset(&ps, 0, sizeof(ps));
	GiveWeapon(&ps, WP_GAUNTLET, -1);
	GiveWeapon(&ps, WP_MACHINEGUN, 50);
	GiveWeapon(&ps, WP_SHOTGUN, 10);
	GiveWeapon(&ps, WP_ROCKET_LAUNCHER, 5);
	assert(Fable_ChooseWeapon(&ps, 60) == WP_SHOTGUN);
	assert(Fable_ChooseWeapon(&ps, 300) == WP_ROCKET_LAUNCHER);
	assert(Fable_ChooseWeapon(&ps, 1200) == WP_ROCKET_LAUNCHER);
	GiveWeapon(&ps, WP_RAILGUN, 3);
	assert(Fable_ChooseWeapon(&ps, 1200) == WP_RAILGUN);
	assert(Fable_ChooseWeapon(&ps, 300) == WP_ROCKET_LAUNCHER);
	GiveWeapon(&ps, WP_LIGHTNING, 100);
	assert(Fable_ChooseWeapon(&ps, 80) == WP_LIGHTNING);
	assert(Fable_ChooseWeapon(&ps, 1200) == WP_RAILGUN);
}

static void TestChooseWeaponSkipsEmptyWeapons( void ) {
	playerState_t ps;
	memset(&ps, 0, sizeof(ps));
	GiveWeapon(&ps, WP_GAUNTLET, -1);
	GiveWeapon(&ps, WP_MACHINEGUN, 50);
	GiveWeapon(&ps, WP_SHOTGUN, 10);
	GiveWeapon(&ps, WP_ROCKET_LAUNCHER, 0);
	assert(Fable_ChooseWeapon(&ps, 300) == WP_SHOTGUN);
	ps.ammo[WP_MACHINEGUN] = 0;
	ps.ammo[WP_SHOTGUN] = 0;
	assert(Fable_ChooseWeapon(&ps, 300) == WP_GAUNTLET);
	assert(Fable_ChooseWeapon(&ps, 1200) == WP_GAUNTLET); /* Last resort. */
}

static void TestLeadTargetStationary( void ) {
	vec3_t eye = {0, 0, 0}, target = {900, 0, 0}, velocity = {0, 0, 0}, out;
	Fable_LeadTarget(eye, target, velocity, 900, out);
	assert(Near(out[0], 900, 0.01f) && Near(out[1], 0, 0.01f) && Near(out[2], 0, 0.01f));
}

static void TestLeadTargetHitscanIgnoresVelocity( void ) {
	vec3_t eye = {0, 0, 0}, target = {900, 0, 0}, velocity = {0, 320, 0}, out;
	Fable_LeadTarget(eye, target, velocity, 0, out);
	assert(Near(out[0], 900, 0.01f) && Near(out[1], 0, 0.01f));
}

static void TestLeadTargetMovingConverges( void ) {
	vec3_t eye = {0, 0, 0}, target = {900, 0, 0}, velocity = {0, 320, 0}, out, offset;
	float time, dist;
	Fable_LeadTarget(eye, target, velocity, 900, out);
	/* The rocket and the target must reach the aim point at the same time. */
	VectorSubtract(out, target, offset);
	time = offset[1] / velocity[1];
	VectorSubtract(out, eye, offset);
	dist = VectorLength(offset);
	assert(time > 1.0f && Near(dist / 900, time, 0.02f));
}

static void TestPerceptionFovWidensWhenAlerted( void ) {
	assert(Fable_PerceptionFov(150, qfalse, qfalse, 1000) == 150);
	assert(Fable_PerceptionFov(150, qtrue, qfalse, 1000) == 360);
	assert(Fable_PerceptionFov(150, qfalse, qtrue, 1000) == 360);
	assert(Fable_PerceptionFov(150, qfalse, qfalse, 200) == 360);
}

static void TestInFov( void ) {
	vec3_t view = {0, 90, 0}, ahead = {0, 1, 0}, behind = {0, -1, 0}, side = {1, 0.2f, 0};
	assert(Fable_InFov(view, 150, ahead));
	assert(!Fable_InFov(view, 150, behind));
	assert(!Fable_InFov(view, 150, side));
	assert(Fable_InFov(view, 360, behind));
}

static void TestChooseTargetPrefersClosestWithStickiness( void ) {
	fableCandidate_t c[3];
	memset(c, 0, sizeof(c));
	c[0].entity = 1; c[0].dist = 500;
	c[1].entity = 2; c[1].dist = 400;
	c[2].entity = 3; c[2].dist = 450;
	assert(Fable_ChooseTarget(c, 3) == 1);
	c[0].isCurrent = qtrue; /* Slightly further current target is kept. */
	assert(Fable_ChooseTarget(c, 3) == 0);
	c[0].dist = 1500; /* Much further current target is dropped. */
	assert(Fable_ChooseTarget(c, 3) == 1);
	c[2].carriesFlag = qtrue;
	assert(Fable_ChooseTarget(c, 3) == 2);
	assert(Fable_ChooseTarget(c, 0) == -1);
}

static void TestShouldFireDirectHit( void ) {
	vec3_t view = {1, 0, 0}, aim = {1, 0.01f, 0};
	VectorNormalize(aim);
	assert(Fable_ShouldFire(WP_MACHINEGUN, view, aim, 5, 5, 600, 0));
	assert(!Fable_ShouldFire(WP_MACHINEGUN, view, aim, 7, 5, 600, 30)); /* Hits a wall. */
}

static void TestShouldFireRequiresAlignment( void ) {
	vec3_t view = {1, 0, 0}, aim = {1, 0.2f, 0};
	VectorNormalize(aim); /* About 11 degrees off. */
	assert(!Fable_ShouldFire(WP_RAILGUN, view, aim, 5, 5, 600, 0));
	assert(!Fable_ShouldFire(WP_ROCKET_LAUNCHER, view, aim, 5, 5, 600, 0));
}

static void TestShouldFireSplash( void ) {
	vec3_t view = {1, 0, 0};
	/* Rocket landing near the target's feet counts even without a direct hit. */
	assert(Fable_ShouldFire(WP_ROCKET_LAUNCHER, view, view, ENTITYNUM_WORLD, 5, 600, 60));
	assert(!Fable_ShouldFire(WP_ROCKET_LAUNCHER, view, view, ENTITYNUM_WORLD, 5, 600, 200));
	/* Never fire a rocket into a wall within splash range of ourselves. */
	assert(!Fable_ShouldFire(WP_ROCKET_LAUNCHER, view, view, ENTITYNUM_WORLD, 5, 90, 10));
	assert(!Fable_ShouldFire(WP_ROCKET_LAUNCHER, view, view, 5, 5, 90, 0));
}

static void TestShouldFireRangedMelee( void ) {
	vec3_t view = {1, 0, 0};
	assert(Fable_ShouldFire(WP_GAUNTLET, view, view, 5, 5, 40, 0));
	assert(!Fable_ShouldFire(WP_GAUNTLET, view, view, 5, 5, 200, 0));
	assert(Fable_ShouldFire(WP_LIGHTNING, view, view, 5, 5, 500, 0));
	assert(!Fable_ShouldFire(WP_LIGHTNING, view, view, 5, 5, 900, 0));
}

static void TestBuildInventory( void ) {
	playerState_t ps;
	int inventory[FABLE_INVENTORY_SIZE];
	memset(&ps, 0, sizeof(ps));
	ps.stats[STAT_WEAPONS] = (1 << WP_MACHINEGUN) | (1 << WP_ROCKET_LAUNCHER);
	ps.ammo[WP_ROCKET_LAUNCHER] = 7;
	ps.ammo[WP_MACHINEGUN] = 50;
	ps.stats[STAT_HEALTH] = 80;
	ps.stats[STAT_ARMOR] = 25;
	ps.powerups[PW_QUAD] = 1;
	Fable_BuildInventory(&ps, inventory);
	assert(inventory[INVENTORY_ROCKETLAUNCHER] == 1);
	assert(inventory[INVENTORY_MACHINEGUN] == 1);
	assert(inventory[INVENTORY_RAILGUN] == 0);
	assert(inventory[INVENTORY_ROCKETS] == 7);
	assert(inventory[INVENTORY_BULLETS] == 50);
	assert(inventory[INVENTORY_HEALTH] == 80);
	assert(inventory[INVENTORY_ARMOR] == 25);
	assert(inventory[INVENTORY_QUAD] == 1);
}

static void TestMissileThreat( void ) {
	vec3_t self = {0, 0, 0}, missile = {-500, 20, 0}, towards = {900, 0, 0}, away = {-900, 0, 0}, side;
	vec3_t wide = {-500, 400, 0};
	assert(Fable_MissileThreat(self, missile, towards, side));
	assert(Near(VectorLength(side), 1, 0.01f) && Near(side[0], 0, 0.01f) && side[1] < 0); /* The path passes at +y, so step to -y. */
	assert(!Fable_MissileThreat(self, missile, away, side));
	assert(!Fable_MissileThreat(self, wide, towards, side));
}

static void TestStuckDetection( void ) {
	fableStuck_t s;
	vec3_t a = {0, 0, 0}, b = {10, 0, 0}, far = {100, 0, 0};
	memset(&s, 0, sizeof(s));
	assert(!Fable_UpdateStuck(&s, a, 1000, qtrue));
	assert(!Fable_UpdateStuck(&s, b, 1500, qtrue));
	assert(!Fable_UpdateStuck(&s, a, 2000, qtrue));
	assert(Fable_UpdateStuck(&s, b, 2600, qtrue));
	assert(!Fable_UpdateStuck(&s, far, 2700, qtrue)); /* Progress clears it. */
	assert(!Fable_UpdateStuck(&s, far, 5000, qfalse)); /* Standing still on purpose. */
}

static void TestTurnTowardsIsRateLimited( void ) {
	vec3_t current = {0, 170, 0}, ideal = {10, -170, 0}, out;
	Fable_TurnTowards(current, ideal, 5, out);
	assert(Near(out[YAW], 175, 0.01f)); /* Shortest way around. */
	assert(Near(out[PITCH], 5, 0.01f));
	Fable_TurnTowards(current, ideal, 90, out);
	assert(Near(out[YAW], -170, 0.01f) && Near(out[PITCH], 10, 0.01f));
	assert(out[ROLL] == 0);
}

int main( void ) {
	TestConfigDefaults();
	TestConfigParsesKeys();
	TestConfigRejectsBadInput();
	TestRandomIsSeededAndBounded();
	TestWeaponTables();
	TestChooseWeaponByDistance();
	TestChooseWeaponSkipsEmptyWeapons();
	TestLeadTargetStationary();
	TestLeadTargetHitscanIgnoresVelocity();
	TestLeadTargetMovingConverges();
	TestPerceptionFovWidensWhenAlerted();
	TestInFov();
	TestChooseTargetPrefersClosestWithStickiness();
	TestShouldFireDirectHit();
	TestShouldFireRequiresAlignment();
	TestShouldFireSplash();
	TestShouldFireRangedMelee();
	TestBuildInventory();
	TestMissileThreat();
	TestStuckDetection();
	TestTurnTowardsIsRateLimited();
	TestDescriptor();
	TestCreateBorrowsWeightFiles();
	TestCreateRejectsBadConfig();
	TestCreateReleasesHandlesWhenWeightsFail();
	TestDeadBotRequestsRespawn();
	TestFirstThinkResetsNavigation();
	TestAcquiresEnemyThenFires();
	TestHoldsFireOnTeammates();
	TestIgnoresDeadEnemies();
	TestSidestepsIncomingRocket();
	TestWalksOffObstacleTowardsGoal();
	TestDropsGoalWhenBlockedForLong();
	puts("fable controller tests passed");
	return 0;
}
