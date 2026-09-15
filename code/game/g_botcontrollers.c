/* Example native controllers and registration. GPL-2.0-or-later. */
#include "g_local.h"
#include "g_botapi.h"
#include "g_botastra.h"
#include "g_botspark.h"

/* State belongs to each bot, not the provider. Avoid per-attach G_Alloc leaks. */
typedef struct {
	int step;
	float yaw;
} circleContext_t;

static circleContext_t circles[MAX_CLIENTS];

static qboolean CircleCreate( int client, const char *config, void **context ) {
	circleContext_t *circle = &circles[client];
	/* This demonstration takes no configuration. */
	if (config[0]) return qfalse;
	memset(circle, 0, sizeof(*circle));
	circle->yaw = g_entities[client].client->ps.viewangles[YAW];
	*context = circle;
	return qtrue;
}

static qboolean CircleThink( const botObservation_t *observation, botAction_t *action, void *context ) {
	circleContext_t *circle = context;
	if (observation->self.pm_type == PM_DEAD) {
		/* Alternate attack/release to request respawn. */
		if (circle->step++ & 1) action->buttons = BUTTON_ATTACK;
		return qtrue;
	}
	if (observation->self.pm_type != PM_NORMAL) return qfalse;
	circle->yaw = AngleNormalize180(circle->yaw + 6);
	action->viewangles[YAW] = circle->yaw;
	action->viewangles[PITCH] = 0;
	action->forward = 80;
	return qtrue;
}

static qboolean IdleThink( const botObservation_t *observation, botAction_t *action, void *context ) {
	return qfalse;
}

static const botController_t circleController = {
	BOT_CONTROLLER_API_VERSION, "circle", 100, CircleCreate, CircleThink, NULL
};

static const botController_t idleController = {
	BOT_CONTROLLER_API_VERSION, "idle", 100, NULL, IdleThink, NULL
};

void BotController_RegisterAll( void ) {
	if (!BotController_Register(&circleController) || !BotController_Register(&idleController) ||
		!BotSpark_Register() || !BotAstra_Register()) {
		G_Error("Failed to register built-in bot controllers");
	}
	/* Register additional static controller descriptors here. */
}
