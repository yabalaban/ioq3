/* Standalone contract tests; compile command is in docs/bot-api.md. */
#include "../code/game/g_local.h"
#include "../code/game/g_botapi.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>

gentity_t g_entities[MAX_GENTITIES];
level_locals_t level;
vmCvar_t g_gametype;
static gclient_t gameClients[MAX_CLIENTS];
static int created, destroyed, changed, calls[MAX_CLIENTS], elapsed;
static qboolean produce = qtrue, failCreate;
static int contexts[MAX_CLIENTS];
static botAction_t output;

/* Engine boundary stubs. Tests run the actual controller implementation. */
void QDECL G_Printf( const char *fmt, ... ) {}
int Q_stricmp( const char *a, const char *b ) { return strcasecmp(a, b); }
float Com_Clamp( float min, float max, float value ) { return value < min ? min : value > max ? max : value; }
float AngleNormalize180( float angle ) {
	while (angle > 180) angle -= 360;
	while (angle < -180) angle += 360;
	return angle;
}
void trap_EA_ResetInput( int client ) {}
void trap_EA_View( int client, vec3_t angles ) {}
void trap_EA_SelectWeapon( int client, int weapon ) {}
int trap_BotGetServerCommand( int client, char *message, int size ) { return 0; }
int trap_Argc( void ) { return 0; }
void trap_Argv( int n, char *buffer, int size ) { buffer[0] = 0; }
void BotAIControllerChanged( int client ) { changed++; }

static qboolean Create( int client, const char *config, void **context ) {
	if (failCreate) return qfalse;
	created++;
	contexts[client] = client;
	*context = &contexts[client];
	return qtrue;
}

static qboolean Think( const botObservation_t *observation, botAction_t *action, void *context ) {
	assert(context == &contexts[observation->client]);
	assert(observation->self.stats[STAT_HEALTH] == 100);
	calls[observation->client]++;
	elapsed = observation->elapsed;
	*action = output;
	/* Lifecycle changes from callbacks must not invalidate this call. */
	BotController_Detach(observation->client);
	assert(BotController_IsAttached(observation->client));
	return produce;
}

static void Destroy( int client, void *context ) {
	assert(context == &contexts[client]);
	destroyed++;
}

static const botController_t provider = {
	BOT_CONTROLLER_API_VERSION, "test", 100, Create, Think, Destroy
};

void BotController_RegisterAll( void ) { assert(BotController_Register(&provider)); }

static void SetupClient( int client ) {
	gentity_t *ent = &g_entities[client];
	ent->inuse = qtrue;
	ent->r.linked = qtrue;
	ent->r.svFlags = SVF_BOT;
	ent->client = &gameClients[client];
	ent->client->pers.connected = CON_CONNECTED;
	ent->client->ps.stats[STAT_HEALTH] = 100;
	ent->client->ps.stats[STAT_WEAPONS] = 1 << WP_MACHINEGUN;
	ent->client->ps.weapon = WP_MACHINEGUN;
	ent->client->ps.viewangles[YAW] = 45;
	ent->client->ps.delta_angles[YAW] = 1234;
}

int main( void ) {
	botController_t invalid;
	botObservedEntity_t entity;
	usercmd_t command;
	int previous;
	level.maxclients = 3;
	level.num_entities = 3;
	SetupClient(0);
	SetupClient(1);
	SetupClient(2);
	g_entities[2].r.svFlags = 0; /* Human. */
	BotController_Init();
	assert(!BotController_Register(&provider)); /* Duplicate name. */
	invalid = provider;
	invalid.apiVersion++;
	assert(!BotController_Register(&invalid));
	invalid = provider;
	invalid.name = "bad name";
	assert(!BotController_Register(&invalid));
	assert(!BotController_Attach(-1, "test", ""));
	assert(!BotController_Attach(MAX_CLIENTS, "test", ""));
	assert(!BotController_Attach(2, "test", ""));
	assert(!BotController_Attach(0, "missing", ""));
	failCreate = qtrue;
	assert(!BotController_Attach(0, "test", ""));
	assert(!BotController_IsAttached(0));
	failCreate = qfalse;
	assert(BotController_Attach(0, "test", ""));
	assert(!BotController_Attach(0, "test", ""));
	assert(BotController_Attach(1, "test", ""));
	assert(created == 2);
	assert(!BotController_Input(2, 0, &command));
	assert(BotController_Input(0, 0, &command));
	assert(command.forwardmove == 0 && command.buttons == 0);
	assert(command.angles[YAW] == ANGLE2SHORT(45) - 1234);

	output.forward = 1000;
	output.right = -1000;
	output.up = 25;
	output.buttons = BUTTON_ATTACK | BUTTON_TALK | BUTTON_WALKING;
	output.viewangles[PITCH] = 100;
	output.viewangles[YAW] = 90;
	output.weapon = WP_ROCKET_LAUNCHER; /* Not owned. */
	BotController_Frame(1000);
	assert(calls[0] == 1 && calls[1] == 1 && elapsed == 0);
	assert(BotController_Input(0, 1000, &command));
	assert(command.serverTime == 1000);
	assert(command.forwardmove == 127 && command.rightmove == -127 && command.upmove == 25);
	assert(command.buttons == (BUTTON_ATTACK | BUTTON_WALKING));
	assert(command.weapon == WP_MACHINEGUN);
	assert(command.angles[PITCH] == ANGLE2SHORT(89));
	assert(command.angles[YAW] == ANGLE2SHORT(90) - 1234);
	BotController_Frame(1050);
	assert(calls[0] == 1);
	BotController_Frame(1100);
	assert(calls[0] == 2 && elapsed == 100);

	/* False releases previous movement/buttons, preserving the current view. */
	produce = qfalse;
	BotController_Frame(1200);
	BotController_Input(0, 1200, &command);
	assert(command.forwardmove == 0 && command.buttons == 0);
	assert(command.angles[YAW] == ANGLE2SHORT(45) - 1234);
	produce = qtrue;
	output.viewangles[YAW] = NAN;
	BotController_Frame(1300);
	BotController_Input(0, 1300, &command);
	assert(command.angles[YAW] == ANGLE2SHORT(45) - 1234);

	/* Life/team transitions invalidate held controls before the next think. */
	gameClients[0].ps.persistant[PERS_SPAWN_COUNT]++;
	BotController_Input(0, 1310, &command);
	assert(command.forwardmove == 0 && command.buttons == 0);
	previous = calls[0];
	BotController_Frame(1310);
	assert(calls[0] == previous + 1 && elapsed == 0);
	gameClients[0].ps.pm_type = PM_DEAD;
	BotController_Frame(1320);
	assert(calls[0] == previous + 2 && elapsed == 0);
	gameClients[0].ps.persistant[PERS_TEAM] = TEAM_SPECTATOR;
	BotController_Frame(1330);
	assert(calls[0] == previous + 3 && elapsed == 0);

	assert(BotController_GetEntity(1, &entity));
	assert(!BotController_GetEntity(-1, &entity));
	g_entities[1].r.svFlags |= SVF_NOCLIENT;
	assert(!BotController_GetEntity(1, &entity));
	gameClients[1].pers.connected = CON_DISCONNECTED;
	BotController_Frame(1340);
	assert(!BotController_IsAttached(1) && destroyed == 1);
	BotController_Detach(0);
	BotController_Detach(0);
	assert(destroyed == 2 && changed == 4);
	assert(!BotController_Input(0, 1400, &command));
	assert(BotController_Attach(0, "test", ""));
	BotController_Input(0, 1400, &command);
	assert(command.forwardmove == 0);
	BotController_Init(); /* Map lifecycle: destroy once and re-register. */
	assert(destroyed == 3 && !BotController_IsAttached(0));
	assert(BotController_Attach(0, "test", ""));
	BotController_Shutdown();
	assert(destroyed == 4);
	puts("bot controller contract tests passed");
	return 0;
}
