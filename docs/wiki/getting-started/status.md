# Current capabilities

These guides describe the implemented paths in the repository, with a supported
environment stated for each task. A module's API or a design document alone is
not a claim of production readiness on every platform.

| Area | Available starting point | Boundary |
| --- | --- | --- |
| Desktop editor | Project creation/open/close, build/run/debug, recent history, panel layout and shortcuts | Linux x64 native Qt; the game has its own window |
| Independent games | Installed CLI, SDK selection, local presets, setup checks and staged creation | Initial native reference toolchain; release identity and SDK compatibility are validated |
| Audio | Native content definitions, catalog, editing and runtime-backed preview | Check the audio evidence for device and hosted acceptance limits |
| Configuration | Typed runtime schema, cooked layers, sparse preferences and offline editor preview | Preference edits apply to a future launch, not the running host |
| Strings and hashing | [Owned/shared bytes, scoped names, cooked dictionaries and lookup hashes](../guides/strings.md) | Explicit lifetimes and failures; dictionary adoption by a content cooker and editor strings inspection remain future work |
| Filesystem | [Immutable mounts, validated packs and bounded background reads](../guides/filesystem.md) | Native directories on Linux/macOS; async memory/pack workers on native targets; browser async reports Unsupported; persistence/watchers remain F5 |
| Randomness | [Caller-owned PCG32 streams and addressed Philox samples](../guides/randomness.md) | Deterministic primitives and the [world demo's room scatter](../../examples/world-demo.md#scatter-placements); game-owned identities/state, no automatic replay or editor RNG inspector |
| Time | [Shared monotonic clock and owner-local stopwatch/deadline/frame helpers](../guides/time.md) | Nanosecond storage; simulation integration, scheduling, pacing and editor timing telemetry remain separate work |
| Browser runtime | Pinned Emscripten/WebGPU Foundation, Platform, RHI paths and smoke samples | Runtime work has separate browser/device acceptance; this website contains documentation |
| Platform detection | [Compiler-owned target facts and constexpr descriptor](../guides/platform-targets.md) | Recognition does not establish an engine port or runtime capabilities |
| Engine systems | Foundation, math, graphics, content, gameplay/world and physics modules | Consult each module's implementation/evidence before relying on a feature |

## Next editor stages

S1 established the workspace and panel structure. The pre-S2 usability increment
added automatic project creation, a location picker, Close Project and shortcuts.
This wiki is the documentation increment before S2.

**S2** will establish focused document editing, save routing, undo/redo and
interaction preservation during background updates. General scene authoring,
embedded play and a full online editor are separate later work.

## Evidence and design

- [Editor architecture and milestones](../../architecture/editor-architecture.md)
- [Audio implementation and remaining acceptance](../../development/audio-content-evidence.md)
- [Integrated browser smoke evidence](../../development/webgpu-w6-evidence.md)
- [Browser product strategy](../../architecture/editor-browser-strategy.md)
- [Editor art-direction studies](../../architecture/editor-art-direction.md)
