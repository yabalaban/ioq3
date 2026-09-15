/* Fable native bot controller. See docs/bot-api.md. GPL-2.0-or-later. */
#ifndef G_BOTFABLE_H
#define G_BOTFABLE_H

#include "g_botapi.h"

#define FABLE_INVENTORY_SIZE 256

typedef struct {
	float skill;              /* 0..1: aim precision, reaction, dodge rate. */
	float fov;                /* Perception field of view in degrees. */
	float turnRate;           /* Maximum view rotation in degrees per second. */
	int seed;                 /* Per-bot PRNG seed. */
	char weights[32];         /* Stock bot whose item/weapon weight files are borrowed. */
} fableConfig_t;

typedef struct {
	int entity;
	float dist;
	qboolean carriesFlag;
	qboolean isCurrent;
} fableCandidate_t;

typedef struct {
	vec3_t origin;
	int since;                /* Time the bot last made progress. */
} fableStuck_t;

extern const botController_t fableController;

/* Pure decision helpers, exposed for the standalone tests. */
qboolean Fable_ParseConfig( const char *config, int client, fableConfig_t *out );
unsigned Fable_RandomNext( unsigned *state );
float Fable_RandomFloat( unsigned *state );
float Fable_ProjectileSpeed( int weapon );
float Fable_SplashRadius( int weapon );
float Fable_IdealRange( int weapon );
float Fable_AimTolerance( int weapon );
int Fable_ChooseWeapon( const playerState_t *ps, float dist );
void Fable_LeadTarget( const vec3_t eye, const vec3_t target, const vec3_t velocity, float projectileSpeed, vec3_t out );
float Fable_PerceptionFov( float baseFov, qboolean tookDamage, qboolean enemyFiring, float dist );
qboolean Fable_InFov( const vec3_t viewangles, float fov, const vec3_t dir );
int Fable_ChooseTarget( const fableCandidate_t *candidates, int count );
qboolean Fable_ShouldFire( int weapon, const vec3_t viewDir, const vec3_t aimDir, int hitEntity, int targetEntity, float impactDist, float impactToTarget );
void Fable_BuildInventory( const playerState_t *ps, int *inventory );
qboolean Fable_MissileThreat( const vec3_t self, const vec3_t missile, const vec3_t velocity, vec3_t sideDir );
qboolean Fable_UpdateStuck( fableStuck_t *stuck, const vec3_t origin, int time, qboolean wantsToMove );
void Fable_TurnTowards( const vec3_t current, const vec3_t ideal, float maxDegrees, vec3_t out );

#endif
