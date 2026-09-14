# Spectator-only web client

## Goal

Serve an interactive ioquake3 spectator in a browser: watch a live server,
follow players, switch targets, or move a free camera. Compile the existing C
client to WebAssembly with Emscripten and use the existing renderer through
WebGL. Start with one server and a small number of viewers.

This is an implementation plan. The live browser transport and enforced
spectator mode described below have not been implemented or verified.

## Existing support

- [cmake/platforms/emscripten.cmake](cmake/platforms/emscripten.cmake) configures
  the web build, enables WebGL 1/2, and disables the dedicated server, GL1
  renderer, and native game libraries for that build.
- [code/web/client.html.in](code/web/client.html.in) loads the engine and game
  assets into the browser filesystem. It currently sets `net_enabled 0`.
- [code/web/client-config.json](code/web/client-config.json) lists the assets
  and QVM game modules to load.
- [code/renderergl2](code/renderergl2) supplies the OpenGL ES-compatible renderer.
- [code/game/g_cmds.c](code/game/g_cmds.c) already implements spectator team
  changes, player following, and follow cycling.
- [code/game/g_session.c](code/game/g_session.c) handles initial team selection
  and session restoration; both matter for enforcing spectator access.

Spectators still need the renderer, maps, models, sounds, snapshot processing,
and client game module. Keep these working before attempting to reduce the
download size or remove unused engine code.

## Architecture

```text
HTTPS static hosting
  page + JavaScript + WebAssembly + game assets
                       |
                       v
Browser spectator <-- WSS --> gateway <-- UDP --> ioquake3 server
  WebAssembly/WebGL          one UDP socket       spectator enforcement
                             per viewer
```

Ordinary browser pages cannot directly use the server's UDP protocol. Add an
explicit browser transport and a WebSocket-to-UDP gateway. See the
[Emscripten networking documentation](https://emscripten.org/docs/porting/networking.html).
Serving the existing web build alone does not enable live multiplayer.

Use secure WebSockets (WSS) for the first version. TCP delivery can introduce
extra delay under packet loss; measure this before considering WebTransport
or WebRTC data channels and their additional gateway work.

## Implementation steps

### 1. Verify the existing browser build

Install and activate the Emscripten SDK as described in the
[repository build instructions](README.md). Use a separate build directory:

```sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CLIENT=ON -DBUILD_RENDERER_GL2=ON -DBUILD_GAME_QVMS=ON
cmake --build build-web --parallel
```

Copy the required PK3 files into `build-web/Release/baseq3/`. Check that the
generated QVM files and the files listed in
`build-web/Release/ioquake3-config.json` are present at their configured paths.
Use game data that is appropriate for the intended distribution and compatible
with the target server.

```sh
python3 -m http.server 8000 --directory build-web/Release
```

Open `http://localhost:8000/ioquake3.html`. Verify rendering, input, sound, and
local demo playback before adding live networking. These commands follow the
existing build configuration; this plan does not constitute a successful build
or browser test. Record the working SDK version once validated.

### 2. Add browser networking and the gateway

- Add an Emscripten-specific transport behind the engine's network interface;
  use [code/qcommon/net_ip.c](code/qcommon/net_ip.c) as the native reference.
- Define a binary WebSocket message as one complete Quake UDP packet. Preserve
  packet boundaries in both directions, including connectionless handshake
  traffic and engine-level fragments.
- Open the WebSocket asynchronously and queue received packets for the engine
  to consume during its normal frame loop. Start the Quake connection only
  after the transport is ready; handle closure and reconnect explicitly.
- Give each viewer a separate gateway UDP socket so the game server sees
  independent client sessions. Forward replies only to the owning viewer.
- Configure one allowed upstream server for the prototype. Bound packet sizes,
  queues, connection counts, and idle lifetimes; close the UDP socket when its
  viewer disconnects. The gateway must not be an arbitrary UDP relay.
- Update the web launcher to configure the gateway and enable the new
  transport. Simply removing `net_enabled 0` is insufficient.

### 3. Enforce spectator-only access

Add a dedicated web spectator build option and launch flow. Automatically
connect to the configured server and present follow-player, next/previous
player, free-camera, scoreboard, and disconnect controls. Remove play/team-join
actions from this interface.

Enforce the restriction on the server as well. Identify gateway connections
using a server-trusted mechanism, such as a dedicated gateway source address
controlled by the deployment. A client-supplied userinfo flag is not sufficient
to establish trusted connection identity.

Force these connections into spectator state on initial admission and session
restoration. Reject transitions into playing teams, including automatic
tournament promotion and team auto-join. Review `SetTeam`, session handling,
and other direct team assignments in the server game module. Preserve normal
player behavior for connections outside this spectator path.

This requires compatible server/game-module changes. An unmodified server can
support a cooperative spectator client, but does not provide this enforced
spectator-only guarantee. Hiding the console or sending `team spectator` once
does not enforce the restriction.

### 4. Package and serve

- Serve the generated HTML, JavaScript, WebAssembly, configuration, and assets
  over HTTPS, with the gateway exposed through WSS.
- Start with same-origin hosting for assets and the gateway. Serve `.wasm` as
  `application/wasm`; configure CORS if assets move to another origin.
- Match the target server's game/mod and asset set, including pure-server
  requirements. The browser build disables the native HTTP download feature,
  so provide required assets through the browser loader.
- Show loading progress and actionable errors for missing assets, an unavailable
  gateway, a full server, and a lost connection. Require a user interaction to
  start audio and capture mouse input where the browser requires it.
- Cache versioned assets. Measure initial download size and memory use before
  optimizing asset packaging or splitting downloads by map.

## Acceptance checks

- Load the page in the intended desktop browsers and render a known map.
- Connect through the gateway, complete the handshake, and receive live snapshots.
- Follow a player, cycle targets, and move the free camera with working audio.
- Verify spectator state from the first spawn, after reconnect, after a map
  change, and across tournament rounds.
- Attempt team changes through commands and a modified client; the server must
  keep gateway connections in spectator state.
- Confirm ordinary native clients can still join and play normally.
- Test two simultaneous browser viewers, disconnect cleanup, missing assets,
  server capacity errors, gateway failure, and delayed/lost packets.
- Measure frame rate, memory, bandwidth per viewer, and observed viewing delay.

## Scaling beyond the prototype

Each browser viewer initially consumes one server client slot and its own
snapshot stream. This is suitable for a small audience.

A larger broadcast requires a spectator relay designed to distribute game
state. Quake snapshots are specific to each client's state and visibility;
copying one spectator's UDP packets to many independent free-camera viewers
will not provide a correct shared broadcast. Treat relay snapshot generation,
camera visibility, and any intentional viewing delay as a separate phase.
