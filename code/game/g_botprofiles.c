/* Persistent named bot/controller assignments. GPL-2.0-or-later. */
#include "g_local.h"
#include "g_botapi.h"
#include "g_botprofiles.h"

#define PROFILE_LIMIT MAX_CLIENTS
#define PROFILE_FILE_SIZE (PROFILE_LIMIT * MAX_INFO_STRING + 128)
typedef struct {
	char id[32], character[MAX_QPATH], controller[MAX_QPATH];
	char name[MAX_NETNAME], config[256], team[8];
	int skill, enabled;
} botProfile_t;
static botProfile_t profiles[PROFILE_LIMIT];
static int generation, nextCheck;
static int retryAt[PROFILE_LIMIT];
static char status[PROFILE_LIMIT][64];
static char fileBuffer[PROFILE_FILE_SIZE];

/* Keep persisted records and console arguments unambiguous. */
static qboolean SafeText( const char *s, int size, qboolean identifier ) {
	int i;
	for (i = 0; s[i]; i++) {
		unsigned char c = s[i];
		if (i >= size - 1 || c < 32 || c > 126 || c == '\\' || c == '"' || c == ';') return qfalse;
		if (identifier && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_' || c == '-')) return qfalse;
	}
	return qtrue;
}

static qboolean Valid( const botProfile_t *p ) {
	return p->id[0] && p->character[0] && p->controller[0] && p->name[0] &&
		SafeText(p->id, sizeof(p->id), qtrue) && SafeText(p->character, sizeof(p->character), qtrue) &&
		Q_stricmp(p->character, "random") && SafeText(p->controller, sizeof(p->controller), qtrue) &&
		SafeText(p->name, sizeof(p->name), qfalse) && SafeText(p->config, sizeof(p->config), qfalse) &&
		(!strcmp(p->team, "auto") || !strcmp(p->team, "red") || !strcmp(p->team, "blue")) &&
		p->skill >= 1 && p->skill <= 5 && (p->enabled == 0 || p->enabled == 1);
}

static unsigned int Checksum( const char *s ) {
	unsigned int hash = 5381;
	while (*s) hash = (hash * 33) ^ (unsigned char)*s++;
	return hash & 0x7fffffff;
}

static qboolean Field( const char *line, const char *key, char *dest, int size ) {
	const char *value = Info_ValueForKey(line, key);
	if (strlen(value) >= size) return qfalse;
	Q_strncpyz(dest, value, size);
	return qtrue;
}

/* Two alternating, checksummed snapshots retain the preceding valid save if
 * power is lost during a write. Files are data, never executed as commands. */
static int Load( int slot, botProfile_t *out ) {
	fileHandle_t f;
	char *data, *line, *end, *cursor;
	int len, gen, sum, n = 0, i;
	botProfile_t *p;
	len = trap_FS_FOpenFile(va("botprofiles-%d.dat", slot), &f, FS_READ);
	if (!f) return 0;
	if (len < 1 || len >= sizeof(fileBuffer)) { trap_FS_FCloseFile(f); return 0; }
	trap_FS_Read(fileBuffer, len, f);
	trap_FS_FCloseFile(f);
	fileBuffer[len] = 0;
	if (strlen(fileBuffer) != len) return 0;
	data = strchr(fileBuffer, '\n');
	if (!data) return 0;
	*data++ = 0;
	cursor = fileBuffer;
	if (strcmp(COM_Parse(&cursor), "Q3BOTPROFILES1")) return 0;
	gen = atoi(COM_Parse(&cursor));
	sum = atoi(COM_Parse(&cursor));
	if (gen <= 0 || sum != Checksum(data)) return 0;
	memset(out, 0, sizeof(profiles));
	for (line = data; *line; line = end + 1) {
		end = strchr(line, '\n');
		if (!end || n >= PROFILE_LIMIT || end - line >= MAX_INFO_STRING) return 0;
		*end = 0;
		p = &out[n];
		if (!Field(line, "id", p->id, sizeof(p->id)) ||
			!Field(line, "character", p->character, sizeof(p->character)) ||
			!Field(line, "controller", p->controller, sizeof(p->controller)) ||
			!Field(line, "name", p->name, sizeof(p->name)) ||
			!Field(line, "config", p->config, sizeof(p->config)) ||
			!Field(line, "team", p->team, sizeof(p->team))) return 0;
		p->skill = atoi(Info_ValueForKey(line, "skill"));
		p->enabled = atoi(Info_ValueForKey(line, "enabled"));
		if (!Valid(p)) return 0;
		for (i = 0; i < n; i++) if (!Q_stricmp(out[i].id, p->id)) return 0;
		n++;
	}
	return gen;
}

static qboolean Save( void ) {
	static botProfile_t verified[PROFILE_LIMIT];
	fileHandle_t f;
	char line[MAX_INFO_STRING], header[128];
	int i;
	fileBuffer[0] = 0;
	for (i = 0; i < PROFILE_LIMIT; i++) {
		botProfile_t *p = &profiles[i];
		if (!p->id[0]) continue;
		Com_sprintf(line, sizeof(line),
			"\\id\\%s\\character\\%s\\controller\\%s\\name\\%s\\config\\%s\\team\\%s\\skill\\%d\\enabled\\%d\n",
			p->id, p->character, p->controller, p->name, p->config, p->team, p->skill, p->enabled);
		Q_strcat(fileBuffer, sizeof(fileBuffer), line);
	}
	if (generation == 0x7fffffff) return qfalse;
	Com_sprintf(header, sizeof(header), "Q3BOTPROFILES1 %d %u\n", generation + 1, Checksum(fileBuffer));
	trap_FS_FOpenFile(va("botprofiles-%d.dat", (generation + 1) % 2), &f, FS_WRITE);
	if (!f) { G_Printf("botprofile: save failed; state directory must be writable\n"); return qfalse; }
	trap_FS_Write(header, strlen(header), f);
	trap_FS_Write(fileBuffer, strlen(fileBuffer), f);
	trap_FS_FCloseFile(f);
	if (Load((generation + 1) % 2, verified) != generation + 1) {
		G_Printf("botprofile: save verification failed\n");
		return qfalse;
	}
	generation++;
	return qtrue;
}

void BotProfiles_Init( void ) {
	static botProfile_t candidate[PROFILE_LIMIT];
	int slot, gen;
	memset(profiles, 0, sizeof(profiles));
	memset(status, 0, sizeof(status));
	memset(retryAt, 0, sizeof(retryAt));
	generation = nextCheck = 0;
	for (slot = 0; slot < 2; slot++) {
		gen = Load(slot, candidate);
		if (gen > generation) { memcpy(profiles, candidate, sizeof(profiles)); generation = gen; }
	}
}

static int Find( const char *id ) {
	int i;
	for (i = 0; i < PROFILE_LIMIT; i++) if (profiles[i].id[0] && !Q_stricmp(profiles[i].id, id)) return i;
	return -1;
}

static qboolean Tagged( int client, const char *id ) {
	char info[MAX_INFO_STRING];
	if (client < 0 || client >= level.maxclients || !(g_entities[client].r.svFlags & SVF_BOT) ||
		level.clients[client].pers.connected == CON_DISCONNECTED) return qfalse;
	trap_GetUserinfo(client, info, sizeof(info));
	return !Q_stricmp(Info_ValueForKey(info, "botprofile"), id);
}

qboolean BotProfiles_IsManaged( int client ) {
	int i;
	for (i = 0; i < PROFILE_LIMIT; i++)
		if (profiles[i].id[0] && profiles[i].enabled && Tagged(client, profiles[i].id)) return qtrue;
	return qfalse;
}

static void Status( int index, const char *message ) {
	if (!strcmp(status[index], message)) return;
	Q_strncpyz(status[index], message, sizeof(status[index]));
	G_Printf("botprofile %s: %s\n", profiles[index].id, message);
}

void BotProfiles_Frame( int time ) {
	int i, client, found, freeSlot;
	char info[MAX_INFO_STRING];
	if (time < nextCheck || level.intermissiontime) return;
	nextCheck = time + 1000;
	for (i = 0; i < PROFILE_LIMIT; i++) {
		botProfile_t *p = &profiles[i];
		if (!p->id[0] || !p->enabled || time < retryAt[i]) continue;
		if (!BotController_HasProvider(p->controller)) {
			for (client = 0; client < level.maxclients; client++)
				if (Tagged(client, p->id)) trap_DropClient(client, "Bot controller unavailable");
			Status(i, "waiting: controller unavailable");
			continue;
		}
		found = -1; freeSlot = -1;
		for (client = 0; client < level.maxclients; client++) {
			if (Tagged(client, p->id)) { found = client; break; }
			if (level.clients[client].pers.connected == CON_DISCONNECTED) freeSlot = client;
		}
		if (found < 0) {
			if (freeSlot < 0) { Status(i, "waiting: no free player slot"); continue; }
			if (!G_GetBotInfoByName(p->character)) { Status(i, "waiting: bot character unavailable"); continue; }
			found = G_AddProfileBot(p->character, p->skill,
				g_gametype.integer < GT_TEAM || !strcmp(p->team, "auto") ? "" : p->team, p->name, p->id);
			if (found < 0) { Status(i, "waiting: spawn failed"); continue; }
		}
		if (level.clients[found].pers.connected != CON_CONNECTED) { Status(i, "waiting: connecting"); continue; }
		if (g_gametype.integer >= GT_TEAM && strcmp(p->team, "auto") &&
			level.clients[found].sess.sessionTeam != (!strcmp(p->team, "red") ? TEAM_RED : TEAM_BLUE))
			SetTeam(&g_entities[found], p->team);
		trap_GetUserinfo(found, info, sizeof(info));
		if (strcmp(Info_ValueForKey(info, "name"), p->name)) {
			Info_SetValueForKey(info, "name", p->name);
			trap_SetUserinfo(found, info);
			ClientUserinfoChanged(found);
		}
		if (Q_stricmp(BotController_Name(found), p->controller)) {
			BotController_Detach(found);
			if (!BotController_Attach(found, p->controller, p->config)) {
				trap_DropClient(found, "Bot controller attach failed");
				retryAt[i] = time + 10000;
				Status(i, "waiting: controller attach failed");
				continue;
			}
			G_LogPrintf("BotProfile: %d \\id\\%s\\controller\\%s\n", found, p->id, p->controller);
		}
		Status(i, va("active: slot %d team=%d", found, level.clients[found].sess.sessionTeam));
	}
}

qboolean BotProfiles_ConsoleCommand( void ) {
	char op[16], id[32], arg[MAX_STRING_CHARS];
	botProfile_t p, old;
	int index, i, argc = trap_Argc();
	trap_Argv(1, op, sizeof(op));
	if (argc == 2 && !Q_stricmp(op, "list")) {
		G_Printf("botprofiles: saved generation %d\n", generation);
		for (i = 0; i < PROFILE_LIMIT; i++) {
			botProfile_t *q = &profiles[i];
			if (q->id[0]) G_Printf("profile %s: name=\"%s\" character=%s controller=%s skill=%d team=%s %s\n",
				q->id, q->name, q->character, q->controller, q->skill, q->team,
				q->enabled ? (status[i][0] ? status[i] : "pending") : "disabled");
		}
		return qtrue;
	}
	trap_Argv(2, arg, sizeof(arg));
	if (!arg[0] || !SafeText(arg, sizeof(id), qtrue)) goto usage;
	Q_strncpyz(id, arg, sizeof(id));
	index = Find(id);
	if (!Q_stricmp(op, "set") && (argc == 8 || argc == 9)) {
		memset(&p, 0, sizeof(p));
		Q_strncpyz(p.id, id, sizeof(p.id));
		/* Read into a large buffer first so oversized values are rejected. */
#define PROFILE_ARG(n, field, ident) \
		trap_Argv(n, arg, sizeof(arg)); \
		if (!SafeText(arg, sizeof(p.field), ident)) goto usage; \
		Q_strncpyz(p.field, arg, sizeof(p.field))
		PROFILE_ARG(3, character, qtrue);
		PROFILE_ARG(4, controller, qtrue);
		trap_Argv(5, arg, sizeof(arg));
		if (strlen(arg) != 1 || arg[0] < '1' || arg[0] > '5') goto usage;
		p.skill = arg[0] - '0';
		PROFILE_ARG(6, team, qtrue);
		PROFILE_ARG(7, name, qfalse);
		PROFILE_ARG(8, config, qfalse);
#undef PROFILE_ARG
		p.enabled = 1;
		if (!Valid(&p)) goto usage;
		if (index < 0) for (i = 0; i < PROFILE_LIMIT; i++) if (!profiles[i].id[0]) { index = i; break; }
		if (index < 0) { G_Printf("botprofile: profile limit reached\n"); return qtrue; }
	} else if (argc == 3 && index >= 0 &&
		(!Q_stricmp(op, "remove") || !Q_stricmp(op, "enable") || !Q_stricmp(op, "disable"))) {
		p = profiles[index];
		if (!Q_stricmp(op, "remove")) memset(&p, 0, sizeof(p));
		else p.enabled = !Q_stricmp(op, "enable");
	} else goto usage;
	old = profiles[index];
	profiles[index] = p;
	if (!Save()) { profiles[index] = old; return qtrue; }
	/* Respawn on edits so character, skill and team also take effect. Disable
 * removes only this profile's bots. Ordinary bots and humans are untouched. */
	for (i = 0; i < level.maxclients; i++) if (Tagged(i, id)) trap_DropClient(i, "Bot profile changed");
	status[index][0] = 0;
	retryAt[index] = 0;
	nextCheck = 0;
	G_Printf("botprofile: saved %s (%s)\n", id, op);
	return qtrue;
usage:
	G_Printf("Usage: botprofile set ID CHARACTER CONTROLLER SKILL TEAM \"DISPLAY NAME\" [\"CONFIG\"]\n"
		"       botprofile list | enable ID | disable ID | remove ID\n"
		"SKILL: 1..5; TEAM: auto/red/blue. Text: printable ASCII, no quotes, semicolons or backslashes.\n");
	return qtrue;
}
