# Drift game engine integration

Status: Engine boundary for the M0/M1 slice, October 3, 2026. This document
records the public engine boundary for a ripple-controlled boat game. It adds no
runtime code and does not require a new engine gameplay subsystem.

The game lives in the separate [Ludus-Sandbox repository](https://github.com/Alegruz/Ludus-Sandbox).
Its `docs/RIPPLE_GAME_DESIGN.md`, `docs/RIPPLE_GAME_ARCHITECTURE.md`, and
`docs/RIPPLE_GAME_MILESTONES.md` own mechanics, physics, content, and delivery.
Sandbox implements the first boat/ripple trial. Its
`docs/RIPPLE_GAME_IMPLEMENTATION.md` records the actual SDK, rendered browser
evidence and remaining physical-device acceptance. Rocks and docking are the
next gameplay milestone.

## Existing public rendering is the initial path

Use [public fullscreen rendering](../development/fullscreen-rendering.md),
[ADR 0011](../decisions/0011-public-fullscreen-rendering.md), and the
[SDK handoff](../development/fullscreen-rendering-handoff.md). The current public
API exposes opaque shaders/uniform/pipeline handles, one uniform binding, and
one fullscreen triangle draw per frame. Uniform data is bounded to 16384 bytes;
resource readiness, resize, synchronization, and shutdown remain engine-owned.

Sandbox can draw water, rings, boat, circular rocks, and dock analytically in one
fragment pass. The M1 upload block is 432 bytes: original ocean data, boat/game
state and sixteen contiguous vec4 ripple records. Slang uses individually named
ripple fields to accommodate the current WebGL 2 reflection subset. The CPU
array has the same layout. Per-target reflection, SPIR-V validation and real
WebGPU/WebGL 2 software rendering pass; these results do not establish a general
shader-array guarantee. No texture/sprite API, multiple draws, compute water simulation,
or GPU-to-CPU wave readback is required for the first playable version.

The separately pinned [Slang toolchain](../decisions/0010-shader-toolchain-feasibility.md)
and `ludus_compile_shader` produce game-owned SPIR-V/WGSL artifacts and generated
GLSL ES for the existing WebGL 2 fallback at build time.
Game source and generated artifact integration stay in Sandbox. Keep private
probe headers and native/WebGPU handles out of its includes. Validate any larger
uniform block independently; the earlier diagnostic layout is not a universal
packing guarantee.

## Repository ownership

| Requirement | Owner and action |
| --- | --- |
| Ripple contact/impulse, water drag, collision, docking, retry | Sandbox simulation |
| Boat/ring/hazard/dock appearance and ocean palette | Sandbox shader/render snapshot |
| Fixed gameplay ticks, input action queue, camera fit, levels | Sandbox session and game data |
| Generic pointer events and cancellation | Existing Ludus Platform/Input facilities; extend only for a reproduced gap |
| Mouse/touch placement semantics and DOM/HUD focus | Sandbox action mapping and web shell |
| Shader generation, public GPU upload/draw/lifecycle | Existing Ludus tooling/RHI |
| Installed SDK dependency closure and consumer build failures | Ludus packaging when independently reproduced |
| Optional native module reload and editor services | Existing GameHost work, with a separate Sandbox adapter |

The subsequent [CPU fluid field](fluid-field.md) supplies reusable grid
simulation through `Ludus::PhysicsFluid`. Sandbox retains authored wind modes,
splash lifetimes, rock-to-mask conversion, boat response and shader extraction.
This extends the original boundary with a generic numerical module; do not add
engine Boat, Ripple, Dock, or game-specific current APIs.
Use bounded game records for one boat and small rock/ripple pools. The
[game world proposal](game-world.md) supplies ownership/update guidance; the jam
slice need not wait for generic entity storage, jobs, or a complete ECS.

## Coordinate and input acceptance

The game uses world meters with positive Y up; engine fragment coordinates use a
top-left origin. Sandbox owns the conversion and a camera that fits the complete
level at different aspect ratios. Use the actual acquired frame extent and the
canvas CSS rectangle consistently so touch/click and visible rings agree under
DPR and resize. Resizing changes presentation, not world collision geometry.

Audit the public pointer event path for primary touch identity, exactly-once
down edges, cancellation, blur, and visibility behavior before claiming mobile
support. Pointer position fields alone do not prove these contracts. An engine
fix should preserve generic event behavior; the game's cooldown and placement
rules remain outside Platform. Real mobile browser/GPU acceptance is separate
from software-rendered browser and native headless evidence.

## GameHost coordination

Native GameHost integration remains an optional later adapter. The browser
trial uses the existing statically linked application and DOM input bridge; it
does not require a parallel module loader or certify native pointer services.

The inspected [project SDK design](project-sdk-workflow.md) establishes external
game ownership and installed SDK consumption. The host design uses a
versioned game function table, a separate play process, explicit checkpoints,
and static gameplay linkage for shipping. Browser dynamic Wasm reload is deferred.
Do not create a parallel loader or infer complete ocean rendering/pointer service
support from a demo host's ability to present a frame.

Sandbox's pure simulation should support a future host adapter without making
the jam game depend on it. Verify the actual ABI and host service capacity before
adopting the host. A later shader/uniform service must define generation ownership,
staging restrictions, GPU retirement, and invalidation across reload. Game
checkpoints encode owned state explicitly and exclude pointers, padding, GPU
handles, and callbacks. An incompatible game schema requests migration/restart;
an engine ABI change rebuilds/restarts the host.

## Engine work admission and verification

No engine implementation change is requested by this document alone. Reproduce
an SDK, pointer, or rendering limitation with a small external consumer before
opening a new engine slice. Sandbox's existing validation note records a historic
Threads dependency workaround; check the selected SDK/current package config
before treating it as an unfixed current issue.

Prioritize a real running ocean baseline, then one steerable boat, then a rock
and dock. Keep optional host/editor integration independent. Reuse existing
shader, RHI, lifecycle, SDK consumer, header, sanitizer, format, and tidy gates
for any engine change. Gameplay tests belong in Sandbox. Record actual rendering,
device, packaging, and touch evidence without converting compilation or historical
chat progress into acceptance.

## Filter cosmetic ripples by pixel footprint

Thanks to **Huw Bowles, Daniel Zimmermann, Chino Noris and Beibei Wang**,
*Crest: Novel Ocean Rendering Techniques in an Open Source Framework*,
SIGGRAPH 2017, slides 29–30 and 44, for tying wave frequencies and normal detail
to sampling resolution. Drift's top-down fullscreen shader reconstructs the
solver's slope and adds three cosmetic sinusoidal terms. The shader already
computes metres per pixel from the view height and acquired target height. At
a 100 m view height, 360 framebuffer rows and the minimum 3 m wave-scale setting,
the finest cosmetic term has about one pixel per wavelength. Its vertical phase
step exceeds the Nyquist limit, so the shader can sample the term as a false
lower frequency. The calculation establishes a sampling risk. The visibility
of that artifact still depends on the scene and display.
[SIGGRAPH course slides](https://www.advances.realtimerendering.com/s2017/Ocean_SIGGRAPH17_Final.pptx) ·
[Sandbox shader at the reviewed revision](https://github.com/Alegruz/Ludus-Sandbox/blob/09f3d0b35781ab37f5eed247b176e88d4d73132b/shaders/ocean.slang#L265-L288)

For a sinusoid over an unwarped square pixel, the exact box average multiplies
its amplitude by `sinc(phaseStepX / 2) * sinc(phaseStepY / 2)`, with `sinc(x)`
defined as `sin(x)/x` and continuous at zero. Averaging 32 × 32 equal-area
subpixel samples agreed with this formula to within 0.0002 RMS in normalized
mode amplitude over the sampled phases at 360, 720 and 1080 rows with the 3 m
setting. Box averaging alone leaves residual energy above Nyquist. A prototype
can compare it with a smooth frequency cutoff before treating either as an
image-quality improvement. The measured scalar check excludes advection and
the final nonlinear shading.

Validate an app-owned filter against a supersampled image of the cosmetic
terms. Use a fixed-time size sweep and an animated run with the minimum wave
scale on the packaged WebGPU and WebGL 2 app. Keep the unfiltered shader as the
control. Check that the filter reduces aliasing where the wave is undersampled
and retains the resolved detail at larger frame sizes. Scope the change to the
cosmetic slope contribution. Solver heights, transported foam and boat response
come from the CPU field and need to agree across presentation resolutions.
