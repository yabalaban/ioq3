# Persistent bot profiles

`botprofile` is an administrator/server-console command. It saves named bots and
their native controller assignments independently of transient client slots.
Compile `g_botprofiles.c` with the game module. Providers still register through
`BotController_RegisterAll`; adding a provider requires a rebuild and restart.

```text
botprofile set orbit Sarge circle 3 auto "Orbit One"
botprofile set observer Major idle 2 blue "Observer"
botprofile list
botprofile disable orbit
botprofile enable orbit
botprofile remove orbit
```

Syntax: `botprofile set ID CHARACTER CONTROLLER SKILL TEAM "DISPLAY NAME" ["CONFIG"]`.
`set` creates or replaces a profile and enables it. Replacing a profile respawns
its bot so character, name, skill, team and controller settings all take effect.

* ID: 1–31 letters, digits, underscores or hyphens; case-insensitive and unique.
* CHARACTER: a named installed bot, such as Sarge; `random` is deliberately excluded.
* CONTROLLER: registered provider name. Unavailable providers remain pending.
* SKILL: whole number 1–5. Controllers decide how to use the character's skill.
* TEAM: `auto`, `red` or `blue`. Explicit teams apply in TDM/CTF; free-for-all and
  tournament modes use their normal team/spectator rules. Normal team balance
  restrictions still apply.
* DISPLAY NAME: 1–35 printable ASCII characters; normal Quake name cleanup and
  colour handling apply. Names do not have to match controller names.
* CONFIG: optional provider-specific string, up to 255 printable ASCII characters.
  `circle` accepts only an empty string; other providers define their own format.
* Text must not contain double quotes, semicolons or backslashes.

The reconciler checks once per game second while bot AI is running. It finds only
bots tagged with that profile's `botprofile` userinfo key. It never adopts a human
or an ordinary bot merely because the display name or slot matches. Missing bots
are spawned; missing attachments are restored after map changes, `map_restart`,
disconnects and process restarts. Controller callbacks receive a fresh context;
internal controller memory and scores are not persisted by this feature.

There can be at most 64 saved profiles, but active bots still require free player
slots. Full servers and missing characters/providers show a waiting reason in
`list`. A controller that rejects configuration is retried every ten game
seconds; the failed bot is removed so it cannot silently run stock AI. An enabled
profile protects its bot from `bot_minplayers` automatic removal. Kicking it or
manually detaching its controller is temporary: disable or remove its profile to
keep it out. `disable` removes the bot but preserves its saved settings; `remove`
deletes the profile and its bot. Other clients are unaffected.

Profiles are stored as versioned, checksummed data in `botprofiles-0.dat` and
`botprofiles-1.dat` under the game's writable home directory (`baseq3` or the
active mod directory). Each save alternates files, closes and reads back the new
snapshot, and keeps the previous snapshot. Startup chooses the newest valid
snapshot. An incomplete/corrupt newest file falls back to the previous save.
This is recovery from interrupted writes, not a substitute for volume backups.
Keep both files together; do not execute them or edit them as configuration scripts.

The `BotProfile: SLOT \\id\\ID\\controller\\PROVIDER` game-log event identifies each
successful attachment for dashboard telemetry. Display names continue through
standard `ClientUserinfoChanged` events. The profile attachment event does not
include controller configuration.
