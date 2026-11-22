# Game world reference

This small game demonstrates the first implementation of the
[world architecture](../architecture/game-world.md). Move the orange player
with WASD, attack nearby red guards with Space, and reach the green exit to load
the other room. P pauses, N advances one neutral tick while paused, and R restarts.
Click or focus the browser canvas before using keyboard controls. The green and
grey rectangles behind the actors are [scatter placements](#scatter-placements)
generated for each room from the session's world seed.

The reusable engine module contains only identity and component storage. This
application owns the level schema, component roster, simulation policies, input
bridge and session. It draws through the engine Renderer, the same renderer the
Editor's World viewport uses. The model builds and tests with the normal native
presets even when the graphical application is disabled.

The renderer now consumes the [C0 camera sample and checked view adapter](../architecture/camera-systems.md#c0-runtime-and-usage).
The authored fixed shot remains the default. Use native `--follow-camera` or
browser `?camera=follow` to opt into immediate planar player follow. Camera
extraction uses the same interpolated target as scene draws and does not change
authoritative gameplay. The existing level schema and minimum-width resize rule
are preserved.

## Run the application

Prepare the normal pinned native tools and dependencies with `./init.sh --cli`.
The optional renderer also requires the pinned Slang/SPIR-V tools:

```bash
./scripts/shader-probe bootstrap
out/host-tools/venv/bin/cmake --list-presets=configure
out/host-tools/venv/bin/cmake --list-presets=build
out/host-tools/venv/bin/cmake --list-presets=test
out/host-tools/venv/bin/cmake --preset linux-clang-development   -DLUDUS_BUILD_WORLD_DEMO=ON   -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"   -DLUDUS_SPIRV_VALIDATOR="$PWD/out/shader-tools/spirv-tools/usr/bin/spirv-val"
out/host-tools/venv/bin/cmake --build --preset linux-clang-development --target ludus_world_demo
out/build/linux-clang-development/apps/world_demo/ludus_world_demo
```

The CMake cache stores local tool paths; do not commit those paths. Existing CMake
Tools settings select the pinned CMake and selectable presets. `--headless` runs
120 simulation ticks without a window; with `--headless`, `--frames N` runs `N`
ticks instead. Without `--headless`, `--frames 10` presents ten native frames and
exits through ordinary shutdown. `--trace-output <path>` captures the whole run
with FoundationProfiling and writes a Perfetto trace there; it fails in a
profiling-disabled Release build and is not available for the browser window.
[Runtime profiling](../development/runtime-profiling.md) runs these modes on a
schedule.

In Perfetto, the trace includes [allocation-domain gauges](../modules/foundation/profiling.md#memory-gauges)
on the main thread alongside the timing events. Headless runs sample before
loading the World, after activation and after the last tick while the session is
still alive. Native windowed runs sample before application startup and on
both sides of explicit shutdown. Use `LiveRequestedBytes` to compare the
covered footprint and the change in `AllocatedBytes` and `FreedBytes` to see
cumulative traffic between samples. The memory coverage manifest lists excluded
paths. Concurrent activity can affect the consistency of a sample.

The native loop is [paced](../architecture/high-resolution-time.md#native-frame-pacing).
The FIFO swapchain paces presented frames. Frames that present nothing (startup,
loading) wait at 60 Hz instead of polling. While the window is minimized or
occluded, every frame waits at 10 Hz (or a lower cap) and the simulation pauses;
showing the window restarts pacing at once. `--frame-rate N`
caps every native frame at `N` per second (1 to 1000); with `--headless` it paces
each tick iteration without changing the ticks run or the replay hash.
`--pacing-report` logs wake lateness, frame-start intervals and EndFrame-return
intervals at exit; it requires `--frames`. The browser build rejects both
options, because its presentation callbacks already pace the page.

To investigate uneven motion, run the native player with `--follow-camera` and
hold a movement key in an open part of the room. The player should stay centered
while the room moves around it. If the player shifts relative to the center,
compare its draw position with the follow camera from the same extracted frame.
If the player stays centered but the whole image holds, compare frame-start and
EndFrame-return intervals with `--frames N --pacing-report`. EndFrame-return
time measures the CPU boundary and can differ from when an image appears on
screen. The Editor's Timing panel measures the separate live-reload game host.
For this demo launched via **Play saved World**, use the player's own report.
[Huw Bowles, Jitternator debugging experiments](https://github.com/huwb/jitternator/blob/8830f3734d4bf8ff97b27132fbd8ef5c251b1232/README.md#debugging-experiments--tests).

For the browser, prepare `./init.sh --cli web-emscripten-development --preset-only`,
enable `LUDUS_BUILD_WORLD_DEMO=ON` in that preset's local CMake cache, and run
`./scripts/build web-emscripten-development`. Serve
`out/build/web-emscripten-development/apps/world_demo/` over HTTP. The graphical
application is opt-in so normal native onboarding does not require shader tools.
Browser builds use the existing pinned Slang, SPIRV-Tools and SPIRV-Cross helpers.

### macOS Metal

The graphical demo is supported on macOS. Its level parser uses Ludus-owned
binary32 decimal conversion, so floating-point `std::from_chars` availability
is no longer a requirement. Prepare the native SDK/dependencies with `init.sh`
and bootstrap shader tools as above, then configure the engine workspace:

```bash
out/host-tools/venv/bin/cmake --preset macos-clang-development -DLUDUS_BUILD_WORLD_DEMO=ON -DLUDUS_SLANG_COMPILER="$PWD/out/shader-tools/slang/bin/slangc"
out/host-tools/venv/bin/cmake --build --preset macos-clang-development --target ludus_world_demo
out/build/macos-clang-development/apps/world_demo/ludus_world_demo
```

For Debug, substitute `macos-clang-debug` in both commands and the executable
path. In VS Code, configure that engine preset with `LUDUS_BUILD_WORLD_DEMO=ON`
and the prepared Slang compiler path, then select `ludus_world_demo` as the
launch target. Tests with `world` in their names are separate executables.
`--headless` runs the simulation without a window; `--frames 3` renders three
frames through Cocoa/Metal and exits. No installed SDK consumer project is
needed for this engine application.

### Windows Direct3D 12

Use an x64 Visual Studio 2022 developer shell with MSVC 14.44 and Windows SDK
10.0.26100.0. Prepare CMake, Ninja and the pinned Conan dependencies as in the
[Ninja runtime step](../../.github/workflows/windows-direct3d12.yml). Set
`LUDUS_WINDOWS_DEPENDENCIES` to the CMakeDeps output directory containing
Catch2, FreeType and HarfBuzz metadata for RelWithDebInfo. Configure and build
from the source checkout.

```powershell
python scripts/shader-probe bootstrap
cmake --preset windows-msvc-runtime
cmake --build --preset windows-msvc-runtime --target ludus_world_demo ludus_world_frame_renderer_tests
python scripts/pack apps/world_demo/levels out/world-levels.lupak
out/build/windows-msvc-runtime/apps/world_demo/ludus_world_demo.exe --level-pack out/world-levels.lupak first-room.world.json
```

`--level-pack <archive> <relative-entry>` loads the starting room from an
existing version-1 Ludus pack. Native Linux and macOS builds accept the same
pack. The named entry goes through the same World validation as `--level` and
the embedded room. The existing restart and exit actions still select the
embedded rooms. On Windows the player logs the selected Direct3D 12 adapter
and whether it is hardware or software.

For a bounded check, add `--frames 3 --follow-camera`. The
`ludus_world_packed_room_d3d12` test compares loose and packed replay hashes,
requires three presented frames and rejects a corrupt pack. The
`ludus_world_frame_renderer_pixels` test reads back the authored room's
raster output. CI sets `LUDUS_TEST_REQUIRE_WARP=1` for those checks. For a
physical GPU run, remove that environment variable and verify that the player
logs a hardware adapter. Exercise WASD, Space, pause, minimize and restore,
resize, and close on the same packed room.

## Edit levels and gameplay

Edit [first-room.world.json](../../apps/world_demo/levels/first-room.world.json) or
[second-room.world.json](../../apps/world_demo/levels/second-room.world.json), then build. CMake embeds these files;
there is no duplicated C++ level literal. `ReadLevel` owns the result, rejects
unknown/duplicate fields, and validates kinds, references and procedural visual
keys. IDs use lowercase ASCII letters, digits and hyphens, with at most 31 bytes.
The Worlds share the editor’s version-1 entity/component envelope and the
game-owned `ludus.world-demo.v1` schema. `ReadWorldLevel` uses the bounded Ludus
JSON parser during loading; it owns the resulting typed Level and releases all
parser storage before returning. Input is limited to 64 KiB, 49 authoring
entities, bounded nesting/strings and no duplicate or unknown fields.

`ReadWorldLevel` also reads version-2 World records, which carry a document UUID
and one UUID per entity, keep each `id` as `label`, and name enemy targets by UUID.
It resolves targets back to labels, so gameplay and the replay hash do not change.
The committed rooms stay version 1. [identity-migration.json](../../apps/world_demo/levels/identity-migration.json)
holds their committed UUIDs, and `ludus-cook identity apply` produces the version-2
form without minting; see [UUID authoring records](../architecture/object-handles-uuid.md#stage-3-uuid-authoring-records).

Use the [World workflow](../development/editor-workspace.md#s4-scene-authoring)
for transforms, player/enemy/exit fields, collision boxes, camera/bounds and seed.
The same saved World feeds both trusted preview and the sample player.

### Scatter placements

Each room may author a room-scoped scatter of static placements, the first
consumer of the [addressed randomness](../architecture/randomness.md#milestone-1-procedural-placement)
outside the renderer and slice P0 of the
[procedural content generation architecture](../architecture/procedural-content-generation.md#p0-room-scoped-scatter).
The recipe is ordinary World content, so the Editor inspects it with the same
schema-driven fields as every other component:

| Component | Entity count | Properties |
| --- | --- | --- |
| `scatter` | one per world | `units_per_metre` (1 to 1000 lattice units per metre), the region `min_x`/`min_y`/`max_x`/`max_y` in metres inside the level bounds, and `keep_out_margin` in metres |
| `scatter_layer` | one to four | `layer` (stable ID 0 to 255, unique in the room), `cell_size` and `candidates` per cell (1 to 64) in lattice units, `min_scale`/`max_scale` in thousandths, and an optional `harvestable` boolean (default false; at most one layer per room) |
| `scatter_variant` | one to four per layer, in document order | `layer` it joins, `exclusion_radius` in lattice units (at most the cell size), drawn `half_x`/`half_y` in metres at scale 1000, and `red`/`green`/`blue` |

The kernel works on an integer lattice; `units_per_metre` converts authored
metres (rounded to the nearest unit) and converts generated positions back for
drawing. Keep-outs are not authored separately: every player and enemy spawn
point, every exit and every collision box, each expanded by `keep_out_margin`,
excludes candidates from all layers. Layer IDs are persistent: a retired ID is
never reused, because it is part of every placement identity together with the
room ordinal, cell and candidate index.

The room ordinal comes from the checked-in catalog in
[room_catalog.h](../../apps/world_demo/internal/room_catalog.h), which maps each
level's document UUID (the committed values in `identity-migration.json`) and,
for version-1 records without a UUID, its settings `id` to a nonzero `uint16`
that is never reused. The random domains (placement, variant, transform, regrow
and cosmetic) are the checked-in manifest in
[random_domains.h](../../apps/world_demo/internal/random_domains.h); both tables
assert their uniqueness at compile time. The session owns an explicit
`WorldSeed` with a fixed default and records the value each world was prepared
with (`GetWorldSeed`); the level's authored `seed` stays content data and does
not influence placements, so two rooms never repeat each other's scatter.

Placements are generated once, when a room's candidate world is prepared, and
stored as compact records (`GetPlacements`), never as entities. Preparation
performs the only allocations: one scratch buffer for every candidate of every
layer and the room's placement storage. The tick and extraction paths allocate
nothing. Extraction draws every placement behind the entities, scaled and
rotated from the generated transform, and samples only the cosmetic domain at
event zero for a per-placement tint. Placements, the scatter recipe, the room
ordinal and the world seed enter the replay hash; cosmetic tints enter nothing,
so rendering, frame pacing, interpolation and extra extractions never change it.

Loading rejects an unusable recipe with nothing published: a repeated layer ID or
name, a variant naming no layer, an empty region or one outside the level
bounds, an exclusion radius above its cell size, an inverted scale range, more
than 4096 candidates (`MAX_ROOM_SCATTER_CANDIDATES`) or more than 128 accepted
placements (`MAX_ROOM_PLACEMENTS`), or marks more than one layer `harvestable`.
A level with a scatter whose room is not in the catalog fails preparation with
`InvalidConfiguration`; the active world is preserved. Levels without a `scatter`
component need no catalog entry and draw nothing extra. The legacy v1 writer has
no scatter representation, so `WriteLevel` omits it.

#### Harvesting and regrowth

A `scatter_layer` marked `harvestable` lets the player harvest its placements.
When the player overlaps one during a tick, the world records a tombstone keyed
by the placement's packed scatter scope (its persistent identity) in the `Commit`
phase. A tombstone is a world mutation, not a random draw. Because regeneration
reapplies the session's tombstones when a room is prepared, a harvested placement
stays harvested across leaving and re-entering the room within a session.

A harvested placement regrows a new variant after `REGROW_DELAY_TICKS`. The new
variant is drawn with `TrySampleBounded` at `(scatter.regrow domain, placement
scope, ordinal, 0)`. The per-placement regrow ordinal starts at 1 and advances
exactly once, in the `Commit` phase of the tick that commits the regrow;
rebuilding, retrying `BuildNext` or faulting a tick neither advances nor publishes
it. An ordinal that would pass `UINT32_MAX` fails the regrow with an explicit
count (`GetRegrowExhaustions`) and leaves the placement harvested; it never wraps.

Tombstones, regrow ordinals and the current harvested/regrown variant enter the
replay hash; cosmetic tints do not. Harvest and regrow storage is bounded and
allocated when the room is prepared, so the tick, commit and extraction paths
allocate nothing. Weighted variant selection and automatic seed acquisition stay
deferred to later randomness milestones.

#### Session checkpoints

`Session::TrySaveCheckpoint` saves the session's random state into a caller
buffer of at most `MAX_SESSION_CHECKPOINT_BYTES`. It first records the active
room's placements into the tombstone store, as leaving the room would, then
writes this little-endian layout:

| Part | Fields |
| --- | --- |
| Envelope | `LWDS` magic, version 1 (`SESSION_CHECKPOINT_VERSION`), total bytes, each `uint32` |
| Random checkpoint | The session's `WorldSeed` as root seed, `Session::GetCheckpointContentIdentity()`, and one event counter per tombstone: domain `scatter.regrow`, the placement's packed scope and its regrow ordinal |
| Tombstone overrides | A `uint32` count, then per tombstone in the same order: scope `uint64`, current variant `uint8`, harvested flag `uint8` |

The [random checkpoint](../wiki/guides/randomness.md#write-and-restore-a-checkpoint)
carries the RNG contract versions. The content identity folds in the random
domain manifest, the room catalog's ordinals, the scatter layout version and the
regrow address, so a checkpoint from changed content is rejected rather than
reinterpreted.

`Session::TryRestoreCheckpoint` is a reload. It validates the whole checkpoint
first: envelope, contract versions, content identity, unique counter owners in
the regrow domain, one override per counter with the same scope, nonzero
ordinals and a harvested flag of 0 or 1. Any rejection leaves the session
unchanged and names the cause through `RandomCheckpointStatus`. On success it
replaces the seed and tombstone store together and discards the active world and
any load in progress; the next `RequestLoad` regenerates the room from the restored
state. The discarded world's addressed commands are retired as on activation
([addressed commands](#addressed-commands-and-session-flow)); a restore returns
`CapacityExceeded`, unchanged, while unconsumed retired outcomes leave no room. A harvested placement becomes due to regrow on the first tick, exactly as
when re-entering its room, and draws the same variant as the uninterrupted
session. `Prepare` rejects a tombstone whose variant lies outside its layer or
whose ordinal is zero with `InvalidConfiguration` and keeps no placements.
Entity, combat and pacing state are not part of the checkpoint.
`apps/world_demo/tests/harvest_tests.cpp` checks the reload replay, every
rejection and the short-buffer case.

`Level` is also directly constructible in code. `Prepare` and `BuildNext` use the
same world semantics for compiled definitions and decoded JSON. `WriteLevel`
produces the legacy v1 interchange JSON into fallible owned storage, preserving the output on
failure. Entity records sort by authored ID. The codec has no filesystem writes;
an editor must provide atomic source-file replacement through its own platform
boundary.

[world.h](../../apps/world_demo/internal/world.h) defines the game-owned component roster and observable
state. [world.cpp](../../apps/world_demo/world.cpp) has the explicit BeginTick, Intent, Motion, Gameplay,
Commit and publication sequence. Add a component pool's initialization, recipe
insertion and destruction removal together. Gameplay uses player/enemy/exit
capabilities rather than branching on recipe kinds after construction.

`RunTick` advances exactly 1/60 second (`TICKS_PER_SECOND`). The application
reads one FoundationTime timestamp per callback and passes it to
`TickDriver::Advance`. The driver owns the [FrameClock](../wiki/guides/time.md#keep-frame-stalls-visible):
it accepts at most 250 ms per frame (`MAXIMUM_FRAME_DELTA`) and runs at most four
ticks (`MAXIMUM_CATCH_UP_TICKS`). Debt is an integer phase: each frame adds
accepted nanoseconds times the tick rate and a tick consumes `PHASE_UNITS_PER_TICK`
(10^9), so one second at 60 Hz is exactly 60 ticks however it is split into frames.
Whole ticks beyond the budget are counted by `GetDiscardedTicks()`, separately from
`GetClampedTime()`; the sub-tick remainder (`GetTickPhase()`) carries and sets the
interpolation alpha in [0, 1). The FrameClock is the only clamp. `Configure`
accepts a `TickPolicy` (rate, clamp, budget) and returns `InvalidConfiguration`,
keeping the previous policy, for a zero rate or budget or a phase that could
overflow `uint64`. A different rate changes real-time speed, not `RunTick`'s
step, so record the policy with replays. A failed tick, including tick-number
exhaustion, publishes no tick and faults the driver. Input edges survive zero-tick frames and apply once
during catch-up. Cancellation takes priority over presses; the bridge waits for
neutral controls before rearming.

Pause/resume, focus changes, queued browser input resets (hide or blur), hidden
frames, faults and activation reset debt and the frame baseline, so the next Playing frame is a
zero-elapsed baseline and never catches up excluded time. Focus changes preserve
pause/fault mode. Diagnostics separate accepted time, time clamped by the frame
cap, excluded lifecycle time and clock regressions; for a monotonic sequence the
three totals equal the observed span. A regressed sample adds no time and cannot
become a baseline below the highest observed reading. A simulation fault retains
the previous render frame until restart and never publishes that tick's presentation.

## Ownership and limits

The reference world reserves 48 entities, 32 commands/outcomes, 64 optional
presentation records, 16 outstanding completion tickets and 193 render records
(`MAX_RENDER_DRAWS`: the room floor, 48 entities, 16 collision boxes and 128 placements).
Content permits 32 initial entities, 16 static boxes and 128 scatter placements.
These limits leave room for every active entity, static geometry and placement
in the render frame. Queue overflow
and commit preflight return explicit errors; optional presentation overflow counts
drops. Runtime spawning currently accepts guards targeting the existing player.

Queued destruction immediately excludes the entity from systems. The registry
and components remain alive until commit. Spawns publish complete bundles after
preflight; they appear in that tick's render frame and first simulate on the next
tick. Preflight conservatively ignores slots freed by that same batch. Spawn
outcomes remain available until the following BeginTick boundary. Combat uses
creation order for ties, and trigger contacts distinguish enter, stay and exit.
The first entering exit in stable order determines the next level. That required
outcome publishes only after the tick succeeds; a fault discards the pending
transition together with its presentation records.

A session parses an owned candidate, prepares all pool capacity, builds one recipe
per `PollLoad`, binds forward references and activates through a nonallocating move
at the frame boundary. Failure, cancellation and supersession preserve the active
world. The two procedural visual keys need no external asset loading or GPU leases.
The old render frame is an owned value; the Renderer and its one quad mesh survive
level changes. Browser loading returns between construction steps.

`BeginRequest` returns an owned ticket and reserves completion capacity before
admission. `SubmitCompletion` is an owner-thread mailbox API; a worker would need
its own synchronized transport. It rejects foreign worlds or mismatched tickets,
and BeginTick sorts one bounded result batch by request ID. Superseded and destroyed
entities reject delayed results. Reservations persist until their matching result
or cancellation acknowledgement is consumed. This sample provides no worker pool.

### Addressed commands and session flow

These are the [Gameplay G1](../architecture/gameplay-foundations.md#debugging-performance-and-acceptance)
contracts: typed addressed commands, cancellation scopes and a small session
state machine. They are game-owned code in this application, not engine API.

`PostDamage` admits a typed `DamageCommand` addressed to an active entity at an
idle boundary and returns a token. The next BeginTick captures every admitted
command. Gameplay delivers the batch after player and enemy combat and before
exit triggers, in admission tick, producer and sequence order. A command admitted
during delivery waits for the next tick, so delivery never drains recursively.
Each token receives exactly one `CommandOutcome`: `Applied`, `StaleTarget`,
`MissingCapability`, `Cancelled` or `Superseded`. Outcomes publish only when their
tick commits, accumulate across catch-up ticks until `ConsumeCommandOutcomes`, and
a faulted tick publishes none. Admission reserves the eventual outcome: pending,
captured and unconsumed outcomes share `MAX_ADDRESSED_COMMANDS` (16), and a full
world rejects admission with `CapacityExceeded`. A paused world holds its
commands until a tick runs. `GetReplayHash` returns zero while commands await a tick.

`OpenCommandScope` returns a generation-checked cancellation token, optionally
owned by an entity (at most `MAX_COMMAND_SCOPES`, 8, open). A command carries its
scope from admission, and delivery revalidates it. Cancelling or closing the scope,
or queuing its owner's destruction, resolves the command as `Cancelled`, even
after its batch was captured. Closing retires the generation, so an old token never
aliases a reused slot. `Session::Activate` retires the replaced world's commands:
unconsumed outcomes move to `GetRetiredCommandOutcomes`, and every unresolved
command is `Superseded`. Activation waits instead of dropping outcomes when that
list is full.

The game drives the session through typed `FlowRequest` values and one
`ProcessFlowBoundary` per frame between ticks; `PollLoad` advances the candidate.
The states are Boot, Frontend, Preparing, Faulted, Shutdown and the Session
superstate's Playing, Paused and Result. A boundary captures the admitted requests
and observes a ready or failed candidate, an authoritative fault and a reached
level outcome. It applies at most one transition: Quit first, then Fault, a
candidate result, Leave, Load, Complete/Continue, Pause/Resume and ServicesReady,
with ties going to the earlier request. Every request receives one
`FlowRequestResult`: `Applied`, `Rejected` (no transition from this state),
`Superseded` or `PreflightFailed`. Requests made during a boundary wait for the
next one. A Load preflight parses its source and reserves the next load's
cancellation scope before any exit action runs, so an unknown room leaves the
state unchanged. Preparing owns its load: leaving it for any reason other than
activation or candidate failure cancels the load exactly once
(`GetCancelledLoads`). A failed candidate returns to the retained Playing,
Paused, Result or Faulted state, and to Frontend for an initial load. A retained
session is held while its replacement prepares. Each state's `FlowPolicy`
(simulation, input route, step) drives the tick driver. P, N and R become Pause or
Resume, `StepFlow` and Load requests; Quit has a reserved request slot. Tools
and the headless runner keep the direct `RequestLoad`, `PollLoad` and `Activate`
calls and leave the flow in Boot. Observers poll `GetLastChange`; scoped
`FlowChanged` subscriptions and gameplay-produced commands are later work.

Ludus profiling scopes measure ticks, Intent, Motion, Gameplay, Commit and render
extraction. A 64-record tick trace ring stores consumed input, phase/status and
queue counts.
Records carry tick numbers; the physical ring is not a chronological span after
wrapping. Const pool access exposes health, transforms and AI action phases to a
debugger. Same-build replay tests feed recorded tick inputs through `RunTick` and
compare canonical hashes that include content and gameplay state while excluding
world IDs, addresses, pool order and rendering history. Hashes include runtime
colliders and outstanding request reservations. `GetReplayHash` returns zero
outside an idle boundary or while commands/completions await the next tick.
A replay file format and
cross-platform floating-point determinism are future work.

## Rendering and validation

The player draws each frame through the engine
[Renderer](../architecture/renderer-systems.md#views-and-depth-l1) on one explicit
RHI device; it keeps no shader of its own. `render_items` turns the extracted
`RenderFrame` into scene items. Each of its up to 193 draws becomes one opaque
flat-material item over a shared unit quad, scaled by its half extent and rotated
by its angle. The first draw is the room floor over the level bounds, in
`ROOM_FLOOR_COLOR`. The draw's position in the frame becomes its depth key, so
later draws cover earlier ones without blending. The gameplay camera sample
becomes a reverse-Z orthographic view through the game's checked `camera_view`
builder.

`FrameRenderer` presents through the Renderer's `RasterViewport`, the same
interactive output the Editor's World viewport uses. It prepares the flat, views
and lighting-output profiles, replaces the snapshot every frame, and composites
to the surface in the same submission. The last drawable view keeps presenting
while a newer view's upload is Pending. The standalone game and hosted Play use
an 8-bit linear scene target, per-channel Reinhard tone mapping at exposure one,
and sRGB output. The Editor's Edit view uses PBR Neutral. The game's `Linear8`
profile rounds dark values before tone mapping. See
[Linear composition and capabilities](../architecture/renderer-systems.md#linear-composition-and-capabilities)
for a precision check when adding gradients to the demo.

Outside the room's bounds the Renderer's black clear shows. Collider boxes are
axis-aligned. Visual rotation affects drawing. Narrow windows expand the camera
height to keep the authored horizontal bounds visible. The item and view builders
have no window, device or Qt dependency, so an Editor view of a World-demo World
can draw the same geometry from the same extraction.

### Record GPU passes from the Editor

When the Editor plays the game in its World viewport (**Play saved World** with
**Play mode** set to **In viewport**), the game runs in a hosted window. Only then does it
create its device with GPU timing and attach a `rhi::HostedGpuRecorder` that
reports as `ludus_world_demo`. Recording **GPU** in the
[Profiling window](../development/editor-workspace.md#recording-cpu-and-gpu-profiling)
while it plays saves a trace of the game's own passes:
`Viewport/Scene`, `Viewport/Composite` and `Viewport/Present`. Its process is
named after the game and its process ID. A standalone window pays nothing for
this: it has no recorder and no timing queries. The recorder owns pass timing
while it records, and the game never arms it. On a device without per-pass
timestamp counters the trace holds whole-frame durations instead, and the status
says frame timing only. The
[Hosted recording](../architecture/editor-gui-systems.md#hosted-recording)
contract describes coverage, disconnection and the export.

Native tests cover identity retirement, stale/foreign handles, sparse removal,
compiled and JSON definitions, canonical round trips, candidate ownership,
input/catch-up/pause, commands, combat/contacts, completions and replay.
`ludus_world_session_flow_tests` covers the G1 contracts: command ordering under
different frame partitions, queue bounds, cancellation after capture, once-only
outcomes through faults and replacement, pause and step, failed replacement and
cancelled loads. The
follow-camera regression drives the real tick and extraction paths with an
uneven schedule, including a 52 ms interval within the catch-up budget. It
checks both the player's draw and camera against the constant-velocity trajectory
sampled one simulation tick behind elapsed time, with the spawn pose held until
the first tick completes.

The scatter tests pin the first room's placement digest, and check presentation
isolation (rendering, pacing, interpolation and extra cosmetic draws leave the
replay hash), content isolation (another room's recipe, a new layer and changed
transform ranges leave existing placements), catalog and manifest rejection, and
load failures that publish nothing. A separate non-sanitized allocator test
injects every candidate storage allocation failure, including the two scatter
allocations, and proves prepared simulation/extraction paths allocate no storage.
`ludus_world_render_items_tests` compares every sampled pixel centre of the
example room with the retired fullscreen shader's coverage rule, and checks
draw order, depth range, colours and rejected frames. The GPU test
`ludus_world_frame_renderer_pixels` presents that room on a headless device
through `FrameRenderer`, then reads back the presented display image. It compares
the image with the same coverage rule passed through an independent restatement
of the display transform. It also checks that rejected and zero-extent frames
keep the last drawable view.

The contributor workflow also checks native ASan/UBSan, format/tidy, header/include
boundaries and installed SDK consumption. Browser QA uses the repository's pinned
[Playwright/PNG prerequisites](../development/tools/web-browser-tests.md):

```bash
node apps/world_demo/tests/browser-test.mjs   out/build/web-emscripten-development/apps/world_demo out/world-browser
```

Set `LUDUS_WORLD_WEBGL=1` to exercise the Auto fallback without WebGPU. The harness
verifies the selected backend and player/guard/exit pixels, compares the page's
scatter placement digest with the pinned native value, captures canvas and
narrow-layout screenshots, and checks movement, pause, step,
restart and focus cancellation. Its Chromium/SwiftShader run is software GPU
acceptance; it does not establish hardware performance or public hosted acceptance.

Fiber scheduling, dependency graphs, render threads, generalized timer queues,
prefab reflection, external media preparation and live component patching remain
the conditional extensions described in the design docs. They are not prerequisites
for this jam baseline.

Recorded checks and remaining limits are in the
[validation record](../development/game-world-validation.md).

## Author a level with the Editor

The [World workspace](../development/editor-workspace.md#s4-scene-authoring)
shares this sample's private level codec, not its gameplay state. Native runs
accept `--level <path>` (at most 64 KiB, regular file, strict v1 validation). It
can be combined with `--headless` or `--frames 3` to qualify a saved World. Invalid
files fail before candidate activation; no source file is rewritten. Without
`--level`, the built-in first room remains the default. Browser CLI file loading
is not an authoring workflow.

Legacy v1 level JSON remains readable for import and player compatibility. The
Editor converts it to a sibling World file and preserves the original; new
authoring and the shipped rooms use `.world.json`.
