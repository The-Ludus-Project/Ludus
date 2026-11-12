# Current capabilities

These guides describe the implemented paths in the repository, with a supported
environment stated for each task. A module's API or a design document alone is
not a claim of production readiness on every platform.

| Area | Available starting point | Boundary |
| --- | --- | --- |
| Desktop editor | Project setup, build and run, [document undo](../../development/editor-workspace.md#undo-history), and [World authoring with a rendered viewport](../../development/editor-workspace.md#world-renderer-and-camera-preview) | Linux x64, macOS and Windows x64 have native paths. See [platform coverage](../guides/platform-targets.md#recognition-is-not-engine-support). |
| Independent games | [Installed CLI, SDK selection, local presets, setup checks and staged creation](../../development/project-sdk-workflow.md) | Linux x64 and macOS use the Clang SDK workflow. Windows x64 uses MSVC SDKs with Ninja and Visual Studio project workflows. |
| Audio | Native content definitions, catalog, editing and runtime-backed preview | Check the audio evidence for device and hosted acceptance limits |
| Configuration | Typed runtime schema, cooked layers, sparse preferences and offline editor preview | Preference edits apply to a future launch, not the running host |
| Strings and hashing | [Owned/shared bytes, scoped names, cooked dictionaries and lookup hashes](../guides/strings.md) | Explicit lifetimes and failures; dictionary adoption by a content cooker and editor strings inspection remain future work |
| Filesystem | [Native directory reads, validated packs, bounded background reads and atomic publication](../guides/filesystem.md) | Linux, macOS and Windows have native directory reads. Browser hosts use synchronous memory and pack reads. |
| Randomness | [Caller-owned PCG32 streams and addressed Philox samples](../guides/randomness.md) | Deterministic primitives and the [world demo's room scatter](../../examples/world-demo.md#scatter-placements); game-owned identities/state, no automatic replay or editor RNG inspector |
| Time | [Shared monotonic clock, frame pacing and runtime timing telemetry](../guides/time.md) | Nanosecond storage. The editor's Timing panel shows read-only runtime status. |
| Browser runtime | Pinned Emscripten/WebGPU Foundation, Platform, RHI paths and smoke samples | [Browser/device acceptance](../../development/webgpu-w6-evidence.md) tracks tested combinations. |
| Browser editor | [Project editing, World import and navigation, downloadable projects and playable samples](../guides/browser-editor.md) | Changes live in the browser session until exported. Web Play runs prebuilt samples. |
| Platform detection | [Compiler-owned target facts and constexpr descriptor](../guides/platform-targets.md) | Recognition does not establish an engine port or runtime capabilities |
| Engine systems | Foundation, math, graphics, content, gameplay/world and physics modules | Consult each module's implementation/evidence before relying on a feature |

## Editor workflow details

The desktop editor supports [World authoring](../../development/editor-workspace.md#gameplay-world-authoring)
and [Live Play](../../development/project-live-reload.md) for projects that use
GameHost. The [document interaction guide](../../development/editor-workspace.md#initial-s2-document-interactions)
covers save routing, undo history and remaining work on the portable document core.

## Evidence and design

- [Editor architecture and milestones](../../architecture/editor-architecture.md)
- [Audio implementation and remaining acceptance](../../development/audio-content-evidence.md)
- [Integrated browser smoke evidence](../../development/webgpu-w6-evidence.md)
- [Browser product strategy](../../architecture/editor-browser-strategy.md)
- [Editor art-direction studies](../../architecture/editor-art-direction.md)
