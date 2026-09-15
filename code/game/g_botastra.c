/* Astra: a public bot-controller API contender. GPL-2.0-or-later. */
#include "g_botapi.h"
#include "bg_public.h"
#include "g_botastra.h"

/* Astra uses only callback observations, returned actions, and
 * BotController_GetEntity. It has no botlib handles, trap_* calls, or direct
 * server-entity access. */
typedef struct {
	unsigned int rng;
	int strafe;
	int nextStrafeTime;
	int nextJumpTime;
	int respawnStep;
	int targetClient;
	int lastHealth;
	float patrolYaw;
	vec3_t progressOrigin;
	int progressTime, escapeUntil, nextDamageDodge;
	float escapeYaw;
	int goal, goalSince;
	int avoidUntil[MAX_GENTITIES];
	int splashUnsafeUntil, lastHits, firingTime, nextProbeTime;
} astraContext_t;

static astraContext_t astras[MAX_CLIENTS];

static unsigned int Astra_Hash( const char *text ) {
	unsigned int value = 2166136261u;
	while (text && *text) {
		value ^= (unsigned int)(unsigned char)*text++;
		value *= 16777619u;
	}
	return value;
}

static unsigned int Astra_Random( astraContext_t *astra ) {
	unsigned int value = astra->rng;
	if (!value) value = 0x6D2B79F5u;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	if (!value) value = 0xA5A5A5A5u;
	astra->rng = value;
	return value;
}

static qboolean Astra_WeaponUsable( const playerState_t *self, int weapon ) {
	if (weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS) return qfalse;
	if (!(self->stats[STAT_WEAPONS] & (1 << weapon))) return qfalse;
	return weapon == WP_GAUNTLET || self->ammo[weapon] != 0;
}

static int Astra_WeaponScore( int weapon, float distance, qboolean usable ) {
	if (!usable) return -10000;
	switch (weapon) {
	case WP_GAUNTLET: return distance < 60.0f ? 85 : 1;
	case WP_MACHINEGUN: return distance < 900.0f ? 45 : 30;
	case WP_SHOTGUN: return distance < 260.0f ? 96 : distance < 650.0f ? 60 : -10000;
	case WP_GRENADE_LAUNCHER: return distance < 180.0f ? 0 : distance < 550.0f ? 35 : 0;
	case WP_ROCKET_LAUNCHER: return distance < 160.0f ? 0 : distance < 900.0f ? 92 : 65;
	case WP_LIGHTNING: return distance < LIGHTNING_RANGE - 24 ? 110 : 0;
	case WP_RAILGUN: return distance > 600.0f ? 105 : 80;
	case WP_PLASMAGUN: return distance > 180.0f && distance < 850.0f ? 90 : 65;
	case WP_BFG: return distance < 160.0f ? 0 : 140;
#ifdef MISSIONPACK
	case WP_NAILGUN: return distance < 700.0f ? 76 : 45;
	case WP_PROX_LAUNCHER: return distance > 180.0f && distance < 550.0f ? 30 : 0;
	case WP_CHAINGUN: return distance < 900.0f ? 75 : 45;
#endif
	default: return -10000;
	}
}

static qboolean Astra_SplashWeapon( int weapon ) {
	return weapon == WP_ROCKET_LAUNCHER || weapon == WP_BFG ||
		weapon == WP_GRENADE_LAUNCHER || weapon == WP_PLASMAGUN
#ifdef MISSIONPACK
		|| weapon == WP_PROX_LAUNCHER
#endif
		;
}

static int Astra_BestWeapon( const playerState_t *self, float distance, qboolean splashUnsafe ) {
	int weapon;
	int best = WP_NONE;
	int bestScore = -10000;
	int currentScore = -10000;
	for (weapon = WP_GAUNTLET; weapon < WP_NUM_WEAPONS; weapon++) {
		int score;
		if (weapon == WP_GRAPPLING_HOOK) continue;
		score = Astra_WeaponScore(weapon, distance, Astra_WeaponUsable(self, weapon));
		if (splashUnsafe && Astra_SplashWeapon(weapon) && score > 0) score = 0;
		if (weapon == self->weapon) currentScore = score;
		if (score > bestScore) {
			best = weapon;
			bestScore = score;
		}
	}
	if (self->weapon > WP_NONE && self->weapon < WP_NUM_WEAPONS &&
		currentScore > 0 && currentScore + 8 >= bestScore && Astra_WeaponUsable(self, self->weapon)) {
		return self->weapon;
	}
	return bestScore > -10000 ? best : WP_NONE;
}

static float Astra_IdealRange( int weapon ) {
	switch (weapon) {
	case WP_GAUNTLET: return 30.0f;
	case WP_SHOTGUN: return 220.0f;
	case WP_GRENADE_LAUNCHER: return 320.0f;
	case WP_LIGHTNING: return 420.0f;
	case WP_ROCKET_LAUNCHER: return 460.0f;
	case WP_PLASMAGUN: return 520.0f;
	case WP_RAILGUN: return 720.0f;
	default: return 500.0f;
	}
}

static float Astra_ProjectileSpeed( int weapon ) {
	switch (weapon) {
	case WP_GRENADE_LAUNCHER: return 700.0f;
	case WP_ROCKET_LAUNCHER: return 900.0f;
	case WP_PLASMAGUN: return 2000.0f;
	case WP_BFG: return 2000.0f;
	default: return 0.0f;
	}
}

/* Item model indices refer to the shared, fixed item definitions, not live
 * server entities. All changing state comes from the observation API. */
static float Astra_ItemValue( const playerState_t *self, int model ) {
	const gitem_t *item;
	int maximum = self->stats[STAT_MAX_HEALTH];
	int amount, limit;
	if (maximum <= 0) maximum = 100;
	if (model <= 0 || model >= bg_numItems) return 0;
	item = &bg_itemlist[model];
	switch (item->giType) {
	case IT_HEALTH:
		limit = (item->quantity == 5 || item->quantity == 100) ? maximum * 2 : maximum;
		amount = limit - self->stats[STAT_HEALTH];
		if (amount <= 0) return 0;
		if (amount > item->quantity) amount = item->quantity;
		return amount * (self->stats[STAT_HEALTH] < 50 ? 6.0f : 2.0f);
	case IT_ARMOR:
		amount = maximum * 2 - self->stats[STAT_ARMOR];
		if (amount <= 0) return 0;
		if (amount > item->quantity) amount = item->quantity;
		return amount * (self->stats[STAT_ARMOR] < 50 ? 3.0f : 1.5f);
	case IT_WEAPON:
		if (item->giTag <= WP_NONE || item->giTag >= WP_NUM_WEAPONS ||
			item->giTag == WP_GRAPPLING_HOOK) return 0;
		if (!Astra_WeaponUsable(self, item->giTag))
			return item->giTag == WP_GAUNTLET || item->giTag == WP_MACHINEGUN ? 30 : 230;
		return 0;
	case IT_AMMO:
		if (item->giTag <= WP_NONE || item->giTag >= WP_NUM_WEAPONS ||
			!(self->stats[STAT_WEAPONS] & (1 << item->giTag)) ||
			self->ammo[item->giTag] < 0 || self->ammo[item->giTag] >= 30) return 0;
		return self->ammo[item->giTag] < 5 ? 160 : 45;
	case IT_POWERUP:
		return item->giTag == PW_QUAD ? 300 : 150;
	case IT_HOLDABLE:
		return self->stats[STAT_HOLDABLE_ITEM] ? 0 : 50;
	default:
		return 0;
	}
}

static int Astra_FindItem( const botObservation_t *obs, astraContext_t *astra,
		vec3_t destination ) {
	int index, best = -1;
	float bestScore = 0;
	for (index = 0; index < obs->numEntities && index < MAX_GENTITIES; index++) {
		botObservedEntity_t entity;
		vec3_t offset;
		float value, score;
		if (astra->avoidUntil[index] > obs->time ||
			!BotController_GetEntity(index, &entity) || entity.state.eType != ET_ITEM ||
			(entity.state.eFlags & EF_NODRAW)) continue;
		value = Astra_ItemValue(&obs->self, entity.state.modelindex);
		if (value <= 0) continue;
		VectorSubtract(entity.origin, obs->self.origin, offset);
		/* Height changes are expensive without a navigation graph. */
		score = value / (160.0f + VectorLength(offset) + 3.0f * fabs(offset[2]));
		if (index == astra->goal) score *= 1.25f;
		if (score > bestScore) {
			bestScore = score;
			best = index;
			VectorCopy(entity.origin, destination);
		}
	}
	return best;
}

/* Exact constant-velocity interception for straight projectiles. A bounded
 * horizon avoids aiming far outside the arena at unreachable intercepts. */
static float Astra_Intercept( const vec3_t offset, const vec3_t velocity, float speed ) {
	float a = DotProduct(velocity, velocity) - speed * speed;
	float b = 2.0f * DotProduct(offset, velocity);
	float c = DotProduct(offset, offset);
	float t = -1, d, t1, t2;
	if (fabs(a) < 0.01f) {
		if (b < -0.01f) t = -c / b;
	} else {
		d = b * b - 4.0f * a * c;
		if (d >= 0) {
			t1 = (-b - sqrt(d)) / (2.0f * a);
			t2 = (-b + sqrt(d)) / (2.0f * a);
			if (t1 > 0) t = t1;
			if (t2 > 0 && (t < 0 || t2 < t)) t = t2;
		}
	}
	if (t < 0) t = sqrt(c) / speed;
	return t > 1.2f ? 1.2f : t;
}

static void Astra_Aim( const playerState_t *self, const botObservedEntity_t *target,
		int weapon, vec3_t angles ) {
	vec3_t eye, aim, offset, velocity;
	float speed = Astra_ProjectileSpeed(weapon);
	VectorCopy(self->origin, eye);
	eye[2] += self->viewheight;
	VectorCopy(target->origin, aim);
	/* Center mass also hits crouching players; headshots confer no bonus. */
	aim[2] += (target->state.legsAnim & ~ANIM_TOGGLEBIT) == LEGS_IDLECR ? 0 : 8;
	VectorCopy(target->state.pos.trDelta, velocity);
	if (target->state.groundEntityNum != ENTITYNUM_NONE) velocity[2] = 0;
	VectorSubtract(aim, eye, offset);
	if (speed > 0) {
		float t = Astra_Intercept(offset, velocity, speed);
		VectorMA(aim, t, velocity, aim);
		if (weapon == WP_GRENADE_LAUNCHER)
			aim[2] += 0.5f * self->gravity * t * t - 0.2f * speed * t;
	}
	VectorSubtract(aim, eye, offset);
	vectoangles(offset, angles);
	angles[PITCH] = Com_Clamp(-89, 89, AngleNormalize180(angles[PITCH]));
	angles[YAW] = AngleNormalize180(angles[YAW]);
	angles[ROLL] = 0;
}

/* Project a world-space movement direction onto the view used by this command,
 * so collecting an item does not require looking away from the enemy. */
static void Astra_Move( botAction_t *action, const vec3_t direction ) {
	vec3_t forward, right, angles;
	float f, r, scale;
	VectorSet(angles, 0, action->viewangles[YAW], 0);
	AngleVectors(angles, forward, right, NULL);
	f = DotProduct(direction, forward);
	r = DotProduct(direction, right);
	scale = fabs(f) > fabs(r) ? fabs(f) : fabs(r);
	if (scale > 0.001f) {
		action->forward = (int)(127 * f / scale);
		action->right = (int)(127 * r / scale);
	}
}

static qboolean Astra_FireRange( int weapon, float distance ) {
	switch (weapon) {
	case WP_GAUNTLET: return distance < 65;
	case WP_LIGHTNING: return distance < LIGHTNING_RANGE - 12;
	case WP_SHOTGUN: return distance < 900;
	case WP_ROCKET_LAUNCHER:
	case WP_BFG: return distance > 160;
	case WP_GRENADE_LAUNCHER: return distance > 180 && distance < 550;
#ifdef MISSIONPACK
	case WP_PROX_LAUNCHER: return distance > 180 && distance < 550;
#endif
	default: return weapon > WP_NONE && weapon != WP_GRAPPLING_HOOK;
	}
}

static qboolean Astra_Create( int client, const char *config, void **context ) {
	astraContext_t *astra;
	unsigned int seed;
	if (client < 0 || client >= MAX_CLIENTS || !context) return qfalse;
	astra = &astras[client];
	memset(astra, 0, sizeof(*astra));
	seed = Astra_Hash(config);
	seed ^= (unsigned int)(client + 1) * 2654435761u;
	if (!seed) seed = 0xA57A0DEu;
	astra->rng = seed;
	astra->strafe = Astra_Random(astra) & 1u ? 1 : -1;
	astra->targetClient = -1;
	astra->goal = -1;
	*context = astra;
	return qtrue;
}

static qboolean Astra_Think( const botObservation_t *obs, botAction_t *action, void *context ) {
	astraContext_t *astra = (astraContext_t *)context;
	const playerState_t *self;
	botObservedEntity_t target;
	vec3_t offset, destination, movement, toward, progress;
	float bestScore = 1e30f, distance = 0, itemDistance = 0;
	int index, targetClient = -1, weapon, item, elapsed, health, holdable;
	qboolean resupply = qfalse, escaping, damaged, unproductive;
	if (!obs || !action || !astra) return qfalse;
	self = &obs->self;
	memset(action, 0, sizeof(*action));
	VectorCopy(self->viewangles, action->viewangles);
	elapsed = obs->elapsed > 0 && obs->elapsed < 1000 ? obs->elapsed : 50;
	health = self->stats[STAT_HEALTH];
	if (obs->elapsed == 0) {
		astra->targetClient = astra->goal = -1;
		astra->nextStrafeTime = obs->time;
		astra->nextJumpTime = obs->time;
		astra->nextDamageDodge = obs->time;
		astra->escapeUntil = obs->time;
		astra->progressTime = astra->goalSince = obs->time;
		astra->lastHealth = health;
		astra->splashUnsafeUntil = obs->time;
		astra->lastHits = self->persistant[PERS_HITS];
		astra->firingTime = 0;
		astra->nextProbeTime = obs->time;
		astra->respawnStep = 0;
		astra->patrolYaw = self->viewangles[YAW];
		VectorCopy(self->origin, astra->progressOrigin);
		memset(astra->avoidUntil, 0, sizeof(astra->avoidUntil));
	}
	if (self->pm_type == PM_DEAD) {
		if ((astra->respawnStep++ & 1) == 0) action->buttons = BUTTON_ATTACK;
		return qtrue;
	}
	if (self->pm_type != PM_NORMAL) return qfalse;
	damaged = health < astra->lastHealth;
	if (damaged && self->persistant[PERS_ATTACKER] == obs->client)
		astra->splashUnsafeUntil = obs->time + 2000;
	astra->lastHealth = health;
	if ((damaged && obs->time >= astra->nextDamageDodge) || obs->time >= astra->nextStrafeTime) {
		astra->strafe = -astra->strafe;
		astra->nextStrafeTime = obs->time + 400 + (int)(Astra_Random(astra) % 650u);
		astra->nextDamageDodge = obs->time + 350;
	}

	for (index = 0; index < obs->numEntities && index < MAX_CLIENTS; index++) {
		botObservedEntity_t entity;
		float score;
		if (index == obs->client || !BotController_GetEntity(index, &entity) ||
			entity.state.eType != ET_PLAYER || entity.health <= 0 ||
			entity.team == TEAM_SPECTATOR || entity.state.clientNum == obs->client) continue;
		if (obs->gametype >= GT_TEAM && entity.team == self->persistant[PERS_TEAM]) continue;
		VectorSubtract(entity.origin, self->origin, offset);
		score = VectorLength(offset) + 2 * fabs(offset[2]);
		if (entity.state.clientNum == astra->targetClient) score *= 0.8f;
		if (entity.health <= 50) score *= 0.75f;
		if (damaged && entity.state.clientNum == self->persistant[PERS_ATTACKER]) score *= 0.8f;
		if (score < bestScore) {
			target = entity;
			targetClient = entity.state.clientNum;
			bestScore = score;
		}
	}
	if (targetClient != astra->targetClient || self->persistant[PERS_HITS] != astra->lastHits) {
		astra->firingTime = 0;
		astra->nextProbeTime = obs->time;
	}
	astra->lastHits = self->persistant[PERS_HITS];
	astra->targetClient = targetClient;
	unproductive = astra->firingTime >= 2000;
	if (targetClient >= 0) {
		VectorSubtract(target.origin, self->origin, offset);
		distance = VectorLength(offset);
	}
	weapon = Astra_BestWeapon(self, targetClient >= 0 ? distance : 450,
		obs->time < astra->splashUnsafeUntil);
	action->weapon = weapon;
	item = Astra_FindItem(obs, astra, destination);
	if (item >= 0) {
		VectorSubtract(destination, self->origin, offset);
		itemDistance = VectorLength(offset);
		resupply = targetClient < 0 || (unproductive && itemDistance < 900) ||
			(health < 45 && itemDistance < 650) ||
			((weapon == WP_MACHINEGUN || weapon == WP_GAUNTLET || weapon == WP_NONE) &&
			(distance > 250 || itemDistance < 150)) || itemDistance < 100;
	}
	if (!resupply) item = -1;
	if (item != astra->goal) {
		astra->goal = item;
		astra->goalSince = obs->time;
	} else if (item >= 0 && obs->time - astra->goalSince >= 5000) {
		/* A useful but unreachable pickup must not monopolize every decision. */
		astra->avoidUntil[item] = obs->time + 10000;
		astra->goal = -1;
	}

	VectorClear(movement);
	if (targetClient >= 0) {
		Astra_Aim(self, &target, weapon, action->viewangles);
		VectorSubtract(target.origin, self->origin, toward);
		toward[2] = 0;
		VectorNormalize(toward);
		if (resupply) {
			VectorSubtract(destination, self->origin, movement);
		} else {
			float range = Astra_IdealRange(weapon);
			float advance = distance > range + 100 ? 1.0f : distance < range - 100 ? -0.8f : 0.0f;
			float strafe = weapon == WP_GAUNTLET ? 0.12f : 0.7f;
			if (weapon == WP_GAUNTLET) advance = 1;
			if (unproductive) advance = 1;
			movement[0] = toward[0] * advance + toward[1] * astra->strafe * strafe;
			movement[1] = toward[1] * advance - toward[0] * astra->strafe * strafe;
		}
	} else if (resupply) {
		VectorSubtract(destination, self->origin, movement);
		vectoangles(movement, action->viewangles);
		action->viewangles[PITCH] = 0;
	} else {
		vec3_t angles;
		astra->patrolYaw = AngleNormalize180(astra->patrolYaw + elapsed * 0.015f);
		VectorSet(angles, 0, astra->patrolYaw, 0);
		AngleVectors(angles, movement, NULL, NULL);
		VectorCopy(angles, action->viewangles);
	}

	/* Measure displacement over time, not instantaneous speed at spawn. Hold
	 * the escape heading long enough to get around an obstacle. */
	VectorSubtract(self->origin, astra->progressOrigin, progress);
	if (obs->time - astra->progressTime >= 650) {
		if (VectorLength(progress) < 24 && self->groundEntityNum != ENTITYNUM_NONE &&
			obs->time >= astra->escapeUntil) {
			vec3_t angles;
			vectoangles(movement, angles);
			astra->escapeYaw = AngleNormalize180(angles[YAW] + astra->strafe *
				(100 + (int)(Astra_Random(astra) % 80u)));
			astra->escapeUntil = obs->time + 1100;
			if (astra->goal >= 0) {
				astra->avoidUntil[astra->goal] = obs->time + 7000;
				astra->goal = -1;
			}
		}
		astra->progressTime = obs->time;
		VectorCopy(self->origin, astra->progressOrigin);
	}
	escaping = obs->time < astra->escapeUntil;
	if (escaping) {
		vec3_t angles;
		VectorSet(angles, 0, astra->escapeYaw, 0);
		AngleVectors(angles, movement, NULL, NULL);
	}
	Astra_Move(action, movement);
	if (escaping && self->groundEntityNum != ENTITYNUM_NONE &&
		!(self->pm_flags & PMF_JUMP_HELD) && obs->time >= astra->nextJumpTime) {
		action->up = 127;
		astra->nextJumpTime = obs->time + 800;
	}
	if ((self->legsAnim & ~ANIM_TOGGLEBIT) == LEGS_SWIM) action->up = 127;
	holdable = self->stats[STAT_HOLDABLE_ITEM];
	if (health < 45 && holdable > 0 && holdable < bg_numItems &&
		bg_itemlist[holdable].giType == IT_HOLDABLE && bg_itemlist[holdable].giTag == HI_MEDKIT)
		action->buttons |= BUTTON_USE_HOLDABLE;

	/* View and attack are consumed in the same usercmd. Comparing to the OLD
	 * view delayed every first shot and often prevented firing while tracking. */
	if (targetClient >= 0 && weapon == self->weapon && Astra_WeaponUsable(self, weapon) &&
		Astra_FireRange(weapon, distance)) {
		qboolean safe = qtrue;
		qboolean splash = Astra_SplashWeapon(weapon);
		if (splash && (escaping || obs->time < astra->splashUnsafeUntil)) safe = qfalse;
		/* Hit feedback is not visibility, but two seconds without damage is a
		 * reason to reposition and probe rather than empty ammo into a wall. */
		if (unproductive && obs->time < astra->nextProbeTime) safe = qfalse;
		if (obs->gametype >= GT_TEAM) {
			vec3_t ray, eye;
			VectorCopy(self->origin, eye);
			eye[2] += self->viewheight;
			AngleVectors(action->viewangles, ray, NULL, NULL);
			for (index = 0; index < obs->numEntities && index < MAX_CLIENTS; index++) {
				botObservedEntity_t ally;
				float along;
				if (index == obs->client || !BotController_GetEntity(index, &ally) ||
					ally.state.eType != ET_PLAYER || ally.health <= 0 ||
					ally.team != self->persistant[PERS_TEAM]) continue;
				VectorSubtract(ally.origin, eye, offset);
				offset[2] += 8;
				along = DotProduct(offset, ray);
				VectorMA(offset, -along, ray, offset);
				if (along > 0 && along < distance + 32 && VectorLength(offset) < 40) safe = qfalse;
				VectorSubtract(ally.origin, target.origin, offset);
				if (splash && VectorLength(offset) < 160) safe = qfalse;
			}
		}
		if (safe) {
			action->buttons |= BUTTON_ATTACK;
			if (astra->firingTime < 2000) astra->firingTime += elapsed;
			if (unproductive) astra->nextProbeTime = obs->time + 700;
		}
	}
	return qtrue;
}

static const botController_t astraController = {
	BOT_CONTROLLER_API_VERSION, "astra", 50, Astra_Create, Astra_Think, NULL
};

qboolean BotAstra_Register( void ) {
	return BotController_Register(&astraController);
}
