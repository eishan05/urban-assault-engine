# RL bridge

Lockstep external-control interface for using the engine as an RL
environment. Disabled unless the `UA_RL_SOCKET` environment variable is
set, in which case the engine listens on that path (Unix stream socket)
and keeps running normally until a client connects. POSIX only; on
Windows the module compiles to a no-op.

While a client is connected the engine is fully client-paced: each frame
blocks until one *frame-running* command arrives, executes exactly one
sim step with the supplied inputs and a fixed timestep, and answers with
one JSON state line. With `gfx.maxfps` forced to 0 on connect, stepping
speed is bounded only by sim cost.

## Commands (client → engine, one per line)

| Command | Frame? | Effect |
|---------|--------|--------|
| `INFO` | no | reply with a state line |
| `CFG k=v ...` | no | `dt=<ms>` fixed timestep (default 20), `headless=0/1` (`setYW_dontRender`), `units=0/1` and `sectors=0/1` (state verbosity), `seed=<n>` (`srand`) |
| `START <levelID>` | yes | menu mode only (rejected elsewhere — `ABORT` a running level first); queues the launch, applied at the next menu frame via `envAction = ACTION_PLAY` (after `ProcessGameShell`, which clears `envAction` each frame). Keep sending `STEP` until the reply shows `mode=2`, `level.state=0`, and the requested `level.id` |
| `STEP <s0> <s1> <s2> <btn> <key>` | yes | run one frame: `Sliders[0..2]` = turn/pitch/throttle, `btn` = `Buttons` bitmask (bit 0/1 missile, 2 minigun-held, 3 handbrake), `key` = engine keycode for `KbdLastHit/Down` (0 = none) |
| `RESET` / `ABORT` / `SAVE` / `LOAD` | yes | set `TLevelInfo::State` to RESTART / ABORTED / SAVE / LOAD before the frame (game mode only) |
| `PROTOS` | no | list vehicle prototypes (`vid`, model class, name, energy, force, mass, maxrot); needs a loaded level |
| `SPAWN <vid> [dx] [dz]` | yes | create vehicle `<vid>` at host station + (dx, 200, dz) (default 600, 600), parent it to the host station, and transfer user control/view into it. Needed because the level-start user unit is the host-station robo, which ignores manual driving input. The parent link is required — AI targeting (`GetEnemyCandidateInSector`) dereferences `_parent` unconditionally |
| `QUIT` | yes | clean engine shutdown after the state reply |

Vehicle death aborts the mission back to the menu (the engine treats the
loss of the user's vehicle that way headless); clients should detect the
mode change and `START` again. On client disconnect the FPS cap is
restored but rendering stays off — re-enabling it under SDL's dummy
video driver (no GL context) would crash the render path.

All other input fields are zeroed each stepped frame, so the real
keyboard/mouse cannot perturb a connected session.

## State line (engine → client)

Single JSON object:

- `mode` — 1 menu, 2 game, 3 replay; `ts` — sim time (ms); `dt` — fixed step
- `level` — `{id, state}` (state: 0 PLAYING, 1 COMPLETED, 2 ABORTED, ...)
- `player_owner` — the player's faction index
- `player` (game mode, when a user vehicle exists) — gid, type, vehicle id,
  owner, status, energy/max, position, velocity, 3×3 rotation, sector
- `robo` — host station gid, energy/max, position, sector
- `units` (if `units=1`, default on) — every live unit: gid, type (`ty`),
  vehicle id (`vid`), owner (`own`), status (`st`), energy (`e`/`em`),
  position (`p`)
- `sectors` (if `sectors=1`, default off) — `w`, `h`, flat `owner` and
  `energy` arrays in row-major order

## Hook points

- `ProcessNextFrame` (main.cpp): `PreFrame` right after the `Period`
  increment — blocks for a command, injects inputs, overrides `Period`
  with the fixed dt (so `DTime`, `TimeStamp`, and GUI timers all see it).
- `ProcessMenuFrame`: `ApplyMenuAction` after `ProcessGameShell()` —
  applies queued `START`/`QUIT` to `userdata.envAction`.
- End of `ProcessNextFrame`: `PostFrame` — sends the state line, returns
  0 to the main loop on `QUIT`.

Determinism note: the engine never calls `srand`, so `rand()`-driven AI
is reproducible run-to-run by default; use `CFG seed=<n>` to vary it.
