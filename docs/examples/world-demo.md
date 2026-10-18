# Game world reference

This small game demonstrates the first implementation of the
[world architecture](../architecture/game-world.md). Move the orange player
with WASD, attack nearby red guards with Space, and reach the green exit to load
the other room. P pauses, N advances one neutral tick while paused, and R restarts.
Click or focus the browser canvas before using keyboard controls.

The reusable engine module contains only identity and component storage. This
application owns the level schema, component roster, simulation policies, input
bridge, session and renderer. The model builds and tests with the normal native
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
presentation records, 16 outstanding completion tickets and 64 render records.
Content permits 32 initial entities and 16 static boxes. These limits leave room
for every active entity plus static geometry in the render frame. Queue overflow
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
The old render frame is an owned value and the renderer's one pipeline survives
level changes. Browser loading returns between construction steps.

`BeginRequest` returns an owned ticket and reserves completion capacity before
admission. `SubmitCompletion` is an owner-thread mailbox API; a worker would need
its own synchronized transport. It rejects foreign worlds or mismatched tickets,
and BeginTick sorts one bounded result batch by request ID. Superseded and destroyed
entities reject delayed results. Reservations persist until their matching result
or cancellation acknowledgement is consumed. This sample provides no worker pool.

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

The game-owned renderer draws up to 64 procedural rectangles through the existing
public fullscreen RHI API. Its 3104-byte uniform block uses fixed vector arrays;
C++ offsets are asserted and independently checked against SPIR-V, WGSL and GLSL
ES reflection. Collider boxes are axis-aligned; visual rotation affects drawing. Narrow windows
expand the camera height to keep the authored horizontal bounds visible.
This adapter demonstrates render extraction without adding textures, arbitrary
meshes or a general sprite renderer to the RHI.

Native tests cover identity retirement, stale/foreign handles, sparse removal,
compiled and JSON definitions, canonical round trips, candidate ownership,
input/catch-up/pause, commands, combat/contacts, completions and replay. A separate
non-sanitized allocator test injects every candidate storage allocation failure
and proves prepared simulation/extraction paths allocate no storage.

The contributor workflow also checks native ASan/UBSan, format/tidy, header/include
boundaries and installed SDK consumption. Browser QA uses the repository's pinned
[Playwright/PNG prerequisites](../development/tools/web-browser-tests.md):

```bash
node apps/world_demo/tests/browser-test.mjs   out/build/web-emscripten-development/apps/world_demo out/world-browser
```

Set `LUDUS_WORLD_WEBGL=1` to exercise the Auto fallback without WebGPU. The harness
verifies the selected backend and player/guard/exit pixels, captures canvas and
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
