/* Native bot controller API. See docs/bot-api.md. GPL-2.0-or-later. */
#include "g_local.h"
#include "g_botapi.h"

#define CONTROLLER_BUTTONS (BUTTON_ATTACK | BUTTON_USE_HOLDABLE | BUTTON_GESTURE | BUTTON_WALKING)

typedef struct {
	const botController_t *provider;
	void *context;
	botAction_t action;
	int nextThink, lastThink, spawnCount, pmType, team;
	qboolean hasThought;
} controllerClient_t;

static const botController_t *providers[BOT_CONTROLLER_MAX_PROVIDERS];
static int numProviders;
static controllerClient_t clients[MAX_CLIENTS];
/* Lifecycle changes from callbacks would invalidate an active call. */
static qboolean inCallback;

static qboolean BotController_Client( int client ) {
	return client >= 0 && client < level.maxclients &&
		g_entities[client].inuse && g_entities[client].client &&
		(g_entities[client].r.svFlags & SVF_BOT) &&
		g_entities[client].client->pers.connected == CON_CONNECTED;
}

qboolean BotController_Register( const botController_t *controller ) {
	int i;
	const char *s;
	if (inCallback || !controller || controller->apiVersion != BOT_CONTROLLER_API_VERSION ||
		!controller->name || !controller->name[0] || !controller->think ||
		controller->thinkInterval < 1 || controller->thinkInterval > 1000 ||
		numProviders >= BOT_CONTROLLER_MAX_PROVIDERS) return qfalse;
	for (s = controller->name; *s; s++) {
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			(*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return qfalse;
	}
	if (s - controller->name >= MAX_QPATH) return qfalse;
	for (i = 0; i < numProviders; i++) {
		if (!Q_stricmp(controller->name, providers[i]->name)) return qfalse;
	}
	providers[numProviders++] = controller;
	return qtrue;
}

qboolean BotController_IsAttached( int client ) {
	return client >= 0 && client < MAX_CLIENTS && clients[client].provider != NULL;
}

void BotController_Detach( int client ) {
	controllerClient_t old;
	if (inCallback || !BotController_IsAttached(client)) return;
	old = clients[client];
	memset(&clients[client], 0, sizeof(clients[client]));
	if (old.provider->destroy) {
		inCallback = qtrue;
		old.provider->destroy(client, old.context);
		inCallback = qfalse;
	}
	BotAIControllerChanged(client);
}

qboolean BotController_Attach( int client, const char *name, const char *config ) {
	int i;
	controllerClient_t *c;
	void *context = NULL;
	qboolean created;
	if (inCallback || !name || !BotController_Client(client) || BotController_IsAttached(client)) return qfalse;
	for (i = 0; i < numProviders; i++) {
		if (!Q_stricmp(name, providers[i]->name)) break;
	}
	if (i == numProviders) return qfalse;
	if (providers[i]->create) {
		inCallback = qtrue;
		created = providers[i]->create(client, config ? config : "", &context);
		inCallback = qfalse;
		if (!created) return qfalse;
	}
	c = &clients[client];
	memset(c, 0, sizeof(*c));
	c->provider = providers[i];
	c->context = context;
	BotAIControllerChanged(client);
	return qtrue;
}

void BotController_Shutdown( void ) {
	int i;
	for (i = 0; i < MAX_CLIENTS; i++) BotController_Detach(i);
}

void BotController_Init( void ) {
	BotController_Shutdown();
	numProviders = 0;
	memset(providers, 0, sizeof(providers));
	BotController_RegisterAll();
}

qboolean BotController_GetEntity( int entity, botObservedEntity_t *observation ) {
	gentity_t *ent;
	if (!observation) return qfalse;
	memset(observation, 0, sizeof(*observation));
	if (entity < 0 || entity >= level.num_entities) return qfalse;
	ent = &g_entities[entity];
	if (!ent->inuse || !ent->r.linked || (ent->r.svFlags & SVF_NOCLIENT)) return qfalse;
	observation->state = ent->s;
	VectorCopy(ent->r.currentOrigin, observation->origin);
	observation->health = ent->health;
	observation->team = ent->client ? ent->client->sess.sessionTeam : -1;
	return qtrue;
}

static void BotController_Neutral( botAction_t *action, const playerState_t *ps ) {
	memset(action, 0, sizeof(*action));
	VectorCopy(ps->viewangles, action->viewangles);
}

void BotController_Frame( int time ) {
	int i;
	controllerClient_t *c;
	playerState_t *ps;
	botObservation_t observation;
	for (i = 0; i < level.maxclients; i++) {
		if (!BotController_IsAttached(i)) continue;
		if (!BotController_Client(i)) { BotController_Detach(i); continue; }
		c = &clients[i];
		ps = &g_entities[i].client->ps;
		if (c->spawnCount != ps->persistant[PERS_SPAWN_COUNT] || c->pmType != ps->pm_type ||
			c->team != ps->persistant[PERS_TEAM] || time < c->lastThink) {
			c->hasThought = qfalse;
		}
		if (c->hasThought && time < c->nextThink) continue;
		memset(&observation, 0, sizeof(observation));
		observation.client = i;
		observation.time = time;
		observation.elapsed = c->hasThought ? time - c->lastThink : 0;
		observation.numEntities = level.num_entities;
		observation.gametype = g_gametype.integer;
		observation.self = *ps;
		c->lastThink = time;
		c->nextThink = time + c->provider->thinkInterval;
		c->spawnCount = ps->persistant[PERS_SPAWN_COUNT];
		c->pmType = ps->pm_type;
		c->team = ps->persistant[PERS_TEAM];
		c->hasThought = qtrue;
		BotController_Neutral(&c->action, ps);
		trap_EA_ResetInput(i);
		trap_EA_View(i, ps->viewangles);
		trap_EA_SelectWeapon(i, ps->weapon);
		inCallback = qtrue;
		if (!c->provider->think(&observation, &c->action, c->context)) {
			BotController_Neutral(&c->action, ps);
		}
		inCallback = qfalse;
	}
}

static int BotController_Move( int value ) {
	if (value < -127) return -127;
	if (value > 127) return 127;
	return value;
}

qboolean BotController_Input( int client, int time, usercmd_t *cmd ) {
	controllerClient_t *c;
	playerState_t *ps;
	char message[MAX_STRING_CHARS];
	float angle;
	int i;
	if (!BotController_IsAttached(client)) return qfalse;
	c = &clients[client];
	ps = &g_entities[client].client->ps;
	/* Stock AI normally drains this reliable-command queue. */
	while (trap_BotGetServerCommand(client, message, sizeof(message))) {}
	memset(cmd, 0, sizeof(*cmd));
	cmd->serverTime = time;
	cmd->weapon = ps->weapon;
	if (c->hasThought && c->spawnCount == ps->persistant[PERS_SPAWN_COUNT] &&
		c->pmType == ps->pm_type && c->team == ps->persistant[PERS_TEAM]) {
		cmd->forwardmove = BotController_Move(c->action.forward);
		cmd->rightmove = BotController_Move(c->action.right);
		cmd->upmove = BotController_Move(c->action.up);
		cmd->buttons = c->action.buttons & CONTROLLER_BUTTONS;
		if (c->action.weapon > WP_NONE && c->action.weapon < WP_NUM_WEAPONS &&
			(ps->stats[STAT_WEAPONS] & (1 << c->action.weapon))) cmd->weapon = c->action.weapon;
		for (i = 0; i < 3; i++) {
			angle = c->action.viewangles[i];
			/* This also rejects NaN/infinity before float-to-integer conversion. */
			if (!(angle >= -360 && angle <= 360)) angle = ps->viewangles[i];
			if (i == PITCH) angle = Com_Clamp(-89, 89, AngleNormalize180(angle));
			cmd->angles[i] = ANGLE2SHORT(angle) - ps->delta_angles[i];
		}
	} else {
		for (i = 0; i < 3; i++) cmd->angles[i] = ANGLE2SHORT(ps->viewangles[i]) - ps->delta_angles[i];
	}
	return qtrue;
}

qboolean BotController_ConsoleCommand( void ) {
	char command[32], slot[16], name[MAX_QPATH], config[MAX_STRING_CHARS];
	int i, client = 0;
	trap_Argv(1, command, sizeof(command));
	if (!Q_stricmp(command, "list") && trap_Argc() == 2) {
		for (i = 0; i < numProviders; i++) G_Printf("controller: %s (%d ms)\n", providers[i]->name, providers[i]->thinkInterval);
		for (i = 0; i < level.maxclients; i++) {
			if (BotController_Client(i)) G_Printf("bot %d: %s\n", i, clients[i].provider ? clients[i].provider->name : "stock");
		}
		return qtrue;
	}
	trap_Argv(2, slot, sizeof(slot));
	for (i = 0; slot[i]; i++) {
		if (slot[i] < '0' || slot[i] > '9' || client >= MAX_CLIENTS) break;
		client = client * 10 + slot[i] - '0';
	}
	if (!slot[0] || slot[i] || !BotController_Client(client)) {
		G_Printf("botcontroller: expected a connected bot slot\n");
		return qtrue;
	}
	if (!Q_stricmp(command, "detach") && trap_Argc() == 3) {
		BotController_Detach(client);
		G_Printf("botcontroller: bot %d uses stock AI\n", client);
		return qtrue;
	}
	if (!Q_stricmp(command, "attach") && (trap_Argc() == 4 || trap_Argc() == 5)) {
		trap_Argv(3, name, sizeof(name));
		trap_Argv(4, config, sizeof(config));
		if (BotController_Attach(client, name, config)) G_Printf("botcontroller: attached %s to bot %d\n", name, client);
		else G_Printf("botcontroller: attach failed (unknown provider, already attached, or create failed)\n");
		return qtrue;
	}
	G_Printf("usage: botcontroller list | attach <slot> <provider> [\"config\"] | detach <slot>\n");
	return qtrue;
}
