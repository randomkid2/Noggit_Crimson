# Dev bridge — driving the editor from a script

A loopback command socket for checking rendering without a human at the window.

It exists because the interesting half of this fork could not be tested any other way. The logic
layer has 223 test cases, but *"do the spawns appear in the right place, at the right size, with the
right texture"* is a question about pixels in an OpenGL window. Every rendering defect found in this
branch — the 300-yard cull, the terminate on the second load, the black skins — cost a round trip
through a person to discover.

> [!warning] This is a remote control surface, and it is gated twice on purpose
>
> 1. **Compiled only** when CMake is configured with `-DNOGGIT_DEV_BRIDGE=ON`, which defaults to
>    `OFF`. `DevBridge.cpp` is excluded from the source list entirely in a default build — the code
>    is not present, not merely unreachable.
> 2. **Listens only** when the environment variable `NOGGIT_BRIDGE_PORT` names a port, and it binds
>    `QHostAddress::LocalHost`. Never a routable interface.
>
> A build shipping an always-listening command socket would let anything running on the machine
> drive the editor, and what the editor can do includes writing files. Do not release a build with
> `NOGGIT_DEV_BRIDGE=ON`; CMake prints a warning when it is set, for that reason.
>
> An environment variable rather than a setting is also deliberate: a `QSettings` key persists, so a
> build enabled once would keep listening on every later run with nothing on screen saying so.

## Building and starting it

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" -DCMAKE_PREFIX_PATH="<QtDir>/msvc2019_64" -DUSE_SQL=ON -DNOGGIT_DEV_BRIDGE=ON
```

```powershell
. tools/dev-bridge/noggit-bridge.ps1
Start-Noggit
Wait-NoggitBridge      # blocks until a map is open -- see the limitation below
Send-Noggit "status"
```

## Unattended startup

The socket is created by `MapView`, which does not exist until a project and map have been chosen.
Rather than require a human for that, a dev-bridge build accepts:

```
noggit.exe --project <folder> --map <id> [--goto <sx,sy,sz>]
```

`--project` takes the folder containing the `.noggitproj`, and replicates what
`NoggitProjectSelectionWindow` does when a project is opened from disk. `--goto` takes
**TrinityCore server** coordinates and converts them through `SpawnPlacement::positionFor`.

> [!warning] `--map` skips the UID fix
> Normal map entry goes through `check_uid_then_enter_map`, which can raise the UID fix window — a
> modal, and a modal is a hang when nobody is present. The automation path calls `enterMapAt`
> directly with `uid_fix_mode::none`.
>
> That makes it fine for looking at a tile and **wrong for editing one**: a session that skipped
> the UID check and then saved could reuse unique IDs. This is a second reason the whole feature is
> compiled only into a `-DNOGGIT_DEV_BRIDGE=ON` build.

Worked example, no human involved:

```powershell
$env:NOGGIT_BRIDGE_PORT = "27055"
Start-Process noggit.exe -ArgumentList '--project','H:\ProjectsNogRed','--map','0','--goto','-9500,70,58'
Start-Sleep 25
. tools/dev-bridge/noggit-bridge.ps1
Send-Noggit "loadspawns"          # OK 5 spawn(s) across 1 tile(s)
Send-Noggit "screenshot H:\tmp\shot.png"
```

## Known limitation: screenshot framing

`screenshot` renders and writes a real image, and `status` reports the camera correctly, but **the
framing does not reliably track camera moves**. Wildly different positions and orientations can
produce the same composition, while the file hash changes.

Two real causes were found and fixed — `paintGL` returns early unless `_needs_redraw` is set (which
produced a perfectly black PNG of a fully loaded scene), and an idle window never repaints, so
nothing recomputes the MVP between commands. A residual issue remains and is **not** diagnosed.
`draw_map` recomputes `_model_view` from `_camera` itself (`MapView.cpp:4061`) and
`MapView::model_view` uses `_camera.look_at_matrix()` in 3D mode, so the obvious suspects are ruled
out. Moving to a different *tile* does update the view, because tile streaming forces repaints.

So the bridge is dependable for state — loading spawns, reporting counts, driving the database
path — and not yet dependable for "point the camera here and show me". Anyone continuing this
should start by instrumenting what `draw_map` actually receives.

## Commands

One line in, one line out. Every reply begins `OK` or `ERR`.

| Command | Effect |
|---|---|
| `ping` | `OK pong`. What `Wait-NoggitBridge` polls. |
| `status` | Camera, yaw/pitch, current tile and whether it is loaded, map id, overlay state, spawn and tile counts. |
| `camera <x> <y> <z>` | Move the camera, in **Noggit** coordinates. Triggers tile streaming, which nothing else does when the camera moves programmatically. |
| `goto <sx> <sy> <sz>` | Move the camera, in **TrinityCore server** coordinates. Converts through `SpawnPlacement::positionFor`, the same tested seam a pasted `.go` string will use. |
| `look <yaw> <pitch>` | Degrees. Positive pitch looks **down** — `direction()` rotates `(0,0,1)` about X, so the sign is the opposite of what most people assume. |
| `loadspawns [all [force]]` | Default is the tile under the camera. `all` is every loaded tile; `force` overrides the 2000-spawn confirmation threshold, which a non-interactive caller is otherwise refused at rather than silently allowed through. |
| `dbspawns on\|off` | Toggle the spawn overlay. |
| `spawns` | Per-spawn detail: guid, kind, display id, resolved model, scale, load state, replaceable texture slot types, and the skins resolved for it. Written for the "why is this creature black" question, which has at least four distinct causes that look identical in the viewport. |
| `lookat <guid> [distance]` | Point the camera at a loaded spawn. Shares `MapView::focusOnSpawn` with the panel's Focus button, so the two cannot frame a spawn differently. |
| `movespawn <guid> <sx> <sy> <sz>` | Move a loaded spawn, in server coordinates. Marks it dirty; writes nothing. |
| `rotatespawn <guid> <degrees>` | Set a spawn's facing, 0 = north. For a gameobject this rewrites `rotation0..3` as well as `orientation`, because the core reads one and the client renders the other. |
| `savechanges [apply]` | Emit every moved or rotated spawn as a reviewable `.sql` into the project's `changesets/`. `apply` also runs it against the **dev schema** — never a live one, which the connection layer refuses to open write-capable. |
| `textures [all]` | Terrain textures actually painted in scope, with a layer count each. Same scan the ground effect panel uses, so the two cannot disagree. |
| `screenshot <path>` | `grabFramebuffer()` to a PNG. Renders into an FBO and reads it back, so it works even when the window is occluded — unlike a desktop grab. |
| `help` | The command list. |

## Worked example — verifying the M1 fixtures

```powershell
. tools/dev-bridge/noggit-bridge.ps1
Send-Noggit "goto -9500 70 58"        # the fixture creatures, in server coordinates
Start-Sleep -Seconds 3                # let the tiles stream in
Send-Noggit "status"                  # confirm tile_loaded=yes before loading
Send-Noggit "loadspawns"
Send-Noggit "screenshot H:\tmp\fixtures.png"
```

## Notes

- Everything runs on the GUI thread. `QTcpServer` delivers signals on the thread its object lives
  on, and `MapView` constructs the bridge, so handlers may touch the GL context directly. This is
  why the class is shaped as it is rather than as a worker thread.
- `loadspawns` never raises a dialog. `MapView::loadDatabaseSpawns` takes an `interactive` flag:
  the menu passes true and gets dialogs, the bridge passes false and gets the same text back as a
  return value. A script left waiting on a modal nobody will click is a hang, not an error.
- Commands cannot throw into the Qt event loop; `DevBridge::onReadyRead` catches everything as a
  backstop. An exception unwinding through a signal emission would take the editor down while a
  human was watching the window rather than the socket.
- One connection per command, by design. A dropped connection costs one command rather than
  wedging a session.
