/* Native bot controller API. See docs/bot-api.md. GPL-2.0-or-later. */
#ifndef G_BOTAPI_H
#define G_BOTAPI_H

#include "../qcommon/q_shared.h"

#define BOT_CONTROLLER_API_VERSION 1
#define BOT_CONTROLLER_MAX_PROVIDERS 16

typedef struct {
	int client;
	int time;                 /* Server simulation time, milliseconds. */
	int elapsed;              /* Since previous think; zero on first call/life change. */
	int numEntities;          /* Upper bound for BotController_GetEntity. */
	int gametype;
	playerState_t self;       /* Copy; valid for this callback only. */
} botObservation_t;

typedef struct {
	int forward, right, up;   /* Relative movement, clamped to [-127, 127]. */
	vec3_t viewangles;        /* Absolute world angles in degrees: pitch/yaw/roll. */
	int buttons;             /* BUTTON_* from q_shared.h. */
	int weapon;              /* WP_*; zero keeps the current weapon. */
} botAction_t;

typedef struct {
	entityState_t state;
	vec3_t origin;            /* Current server position, not trajectory base. */
	int health;
	int team;                /* -1 for entities that are not clients. */
} botObservedEntity_t;

typedef struct {
	int apiVersion;
	const char *name;         /* Unique console-safe name. Static lifetime. */
	int thinkInterval;       /* 1..1000 milliseconds; at most once per frame. */
	/* Optional. On failure, create must release anything it allocated. */
	qboolean (*create)(int client, const char *config, void **context);
	/* Return false to release all controls for this think interval. */
	qboolean (*think)(const botObservation_t *observation, botAction_t *action, void *context);
	/* Optional. Called on detach, disconnect, and map shutdown/restart. */
	void (*destroy)(int client, void *context);
} botController_t;

/* Register static descriptors during BotController_RegisterAll, once per map. */
qboolean BotController_Register( const botController_t *controller );
void BotController_RegisterAll( void );
qboolean BotController_Attach( int client, const char *name, const char *config );
void BotController_Detach( int client );
qboolean BotController_IsAttached( int client );

/* Privileged server observations: no field-of-view or visibility filtering. */
qboolean BotController_GetEntity( int entity, botObservedEntity_t *observation );

/* Convert accumulated trap_EA_* / botlib movement output to a controller action. */
qboolean BotController_ReadBotlibAction( int client, botAction_t *action );

/* Game integration; controller callbacks must not call these functions. */
void BotController_Init( void );
void BotController_Shutdown( void );
void BotController_Frame( int time );
qboolean BotController_Input( int client, int time, usercmd_t *cmd );
qboolean BotController_ConsoleCommand( void );
void BotAIControllerChanged( int client );

#endif
