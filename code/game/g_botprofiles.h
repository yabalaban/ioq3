/* Persistent named bot/controller assignments. GPL-2.0-or-later. */
#ifndef G_BOTPROFILES_H
#define G_BOTPROFILES_H
void BotProfiles_Init( void );
void BotProfiles_Frame( int time );
qboolean BotProfiles_IsManaged( int client );
qboolean BotProfiles_ConsoleCommand( void );
int G_AddProfileBot( const char *character, float skill, const char *team,
                    const char *name, const char *profile );
#endif
