# Render graph frame capture

Status: proposed October 29, 2026. Phase 1, the engine capture and its file, is
implemented as of October 30, 2026, with the departures listed under
[Phase 1 implementation notes](#phase-1-implementation-notes). Phase 2, the editor
source and comparison, is implemented as of October 30, 2026; see
[Phase 2 implementation notes](#phase-2-implementation-notes). Phase 3 is not
implemented.

A render graph is the project's rendering scheme. Today it runs in only two ways:
as the World viewport's interactive renderer, or for one frame as the raster side
of a [reference comparison](../development/editor-workspace.md#compare-with-the-viewports-render-graph).
Neither way produces a saved, converged image of the graph's own estimator. This
design adds a third way: **capture a frame** with a chosen graph, configuration and
output, at a target sample count. The capture freezes the scene and camera, runs
off the display loop and saves linear HDR.

The motivating case is the
[polygonal-light-sampling template](../development/render-graph-authoring.md).
Its four outputs compare Peters' estimators. Comparing their variance requires
converged images, and the viewport can show those but not save them.

## Goals and non-goals

Goals:

- Capture any cooked project graph, not only the one the viewport draws, using
  the same package, configuration, output route and renderer choice semantics as
  the viewport.
- Reach an exact, user-chosen sample count per pixel. The image is the graph's own
  compensated linear mean, with exact counts when the capture stops early.
- Save linear HDR with the identity of everything that produced it: package bytes,
  configuration, output, scene, camera and sample count.
- Leave the interactive viewport untouched. Its drawable view, accumulation and
  device work continue as they would without the capture.
- Keep the capture loop in the engine so the editor, an installed-SDK tool and CI
  can share it.

Non-goals:

- **The capture is not a reference.** A graph capture is the graph's estimate. It
  does not satisfy the
  [CPU and GPU reference requirement](renderer-systems.md#cpu-and-gpu-reference-implementation-requirement),
  and it does not replace RT1.
- **It does not relax compiled contracts.** Query budgets, payload sizes and
  package limits are compiled into the package. A capture runs the package as it
  was built; see [Budgets](#budgets-the-polygonal-light-case).
- **No new render-graph node kinds.** The capture reads what the graph already
  produces.

## Current building blocks

| Piece | What it already does | What a capture needs from it |
| --- | --- | --- |
| `ViewportRasterCapture` (`apps/editor/src/internal/viewport_raster_capture.h`) | Loads the viewport's package, fallback, output and **Force raster** choice into a private `RenderGraphPreview` on the viewport's device, with a frozen camera. Renders offscreen, reads the output back and never presents. Stops waiting for an accumulating graph after `RASTER_CAPTURE_MAXIMUM_FRAMES` (64). | The producer model; a sample target instead of a frame cap; any graph, not only the live one. |
| `ViewportReferenceCapture` and the **Reference…** window | Progress, **Stop**, partial saves with exact counts, `LUHDR01` plus PNG, time-sliced `Service(budget)` on the GUI thread. | The window, its life cycle and its save path. |
| **Accumulate** node | A compensated running mean of linear radiance in an FP32 structured buffer (sum/count and compensation per pixel), fused into the producing ray dispatch. It stops at its sample limit, at most `MAX_ACCUMULATION_SAMPLES` (4096, `render_graph_format/plan.h`), and restarts on camera, scene, extent or shader replacement. | Its FP32 mean and count, rather than the RGBA8 or RGBA16F texture written after it. |
| `RenderGraph::HasPendingSamples()` | Tells the host that a final ray writer is still averaging. | Also the committed sample count. |
| `LUHDR01` ([raw capture](renderer-systems.md#raw-capture-and-installed-sdk)) | FP32 RGB mean plus a uint32 count per pixel. The header is specific to RT1: profile, sampler, depth, roulette and light-sampling flags. | The pixel record; a header that identifies a graph instead of RT1. |

## Design

### Capture source

A capture source names:

- the cooked runtime package, bound by its runtime identity (the SHA-256 of its
  bytes, which already binds the configuration);
- the root configuration;
- the output route: an exported color, or another attachment the viewport's output
  list offers;
- the raster fallback package and the **Force raster** choice, with the viewport's
  meaning.

The capture window defaults the source to the World viewport's current selection.
It can name any built package in the project. A source that is not built, is
stale or does not match its saved draft is rejected with the existing build
diagnostics. The capture never builds implicitly.

### Frozen inputs

`Start` copies everything the capture depends on: scene snapshot and revision,
camera (view description), extent, source and settings. Later edits, navigation,
rebuilds or a change of graph in the viewport do not affect a running capture;
capture again to include them. This is the reference capture's
[existing rule](../development/editor-workspace.md#reference-capture-of-the-viewport).

### Samples

The capture's target `N` ranges from 1 to the capture maximum. The proposal uses
the reference window's `MAXIMUM_REFERENCE_SAMPLES` (65,536) so both sources admit
the same range.

- **The captured route must have an Accumulate on it.** The capture resolves the
  last Accumulate upstream of the selected output and reads that operation's mean.
  A ToneMap after it still produces the PNG.
- **Raster-only routes** are deterministic. They capture one frame and record
  `N = 1`.
- **A ray route without an Accumulate is rejected.** The repair message says to add
  one. Averaging is an authored statement that the output is a Monte Carlo mean, and
  a capture should not infer it.
- **Sample limit override.** The private producer gets a new
  `RenderGraph` capture option: an accumulation limit that replaces the authored
  Accumulate limit, up to the capture maximum. It applies only to that producer. The
  authored package, the live viewport and `MAX_ACCUMULATION_SAMPLES` for
  interactive rendering stay unchanged. Without this override, a target above the
  authored limit is rejected with the limit named.
- **Seeds.** Graph sampling is already deterministic for an identical camera and
  scene revision. The capture records that revision. An explicit seed setting needs
  a renderer option and is listed under [Open questions](#open-questions).

### Readback

The linear image comes from the Accumulate's FP32 mean and count, not from its
output texture. That texture may be RGBA16F, which rounds radiance to about three
decimal digits. A new `RenderGraph` operation copies the mean and the committed
count of one Accumulate. It follows the GPU-compute reference's schedule: after the
first sample, every 16th sample and at the end. A copy is a separate readback that
never stalls the frame that produced it.

Every pixel of an accumulating route has the same count, so the per-pixel counts in
the file are uniform. They stay in the record so partial and complete captures share
one layout.

### Execution

The engine owns the loop. A new `graphics::renderer::RenderGraphCapture` class
provides `Start`, `Service(budgetNanoseconds)`, `Release`, `GetInfo`, `CopyPixels`
and `Encode`. It drives a private `RenderGraph` on a borrowed device. It renders to
offscreen targets and never presents. `Service` submits at most one frame per call,
never waits for the GPU and returns its phase:

- `Preparing`
- `Rendering`
- `Complete`
- `Failed`

The editor wraps it as the reference window wraps `ViewportReferenceCapture`. An
installed-SDK tool can use it with a scene snapshot and a camera, without the
editor.

- **Isolation.** The capture has its own producer, resources and accumulation. The
  live viewport keeps drawing, and its drawable view and accumulation history are
  unchanged by a capture's start, failure or cancellation.
- **Cancellation.** **Stop**, closing the window and stopping the viewport device
  cancel the capture. Committed samples stay readable for a partial save, as with
  the reference capture.
- **Failures.** A latched shader failure (`queries.HasFailed`), a nonfinite or
  negative estimate, or an exhausted query budget makes the graph draw its failure
  colour. The capture counts latched pixels at each readback. Any latched pixel makes
  the capture `Failed`, with the count, rather than `Complete`. The reason depends on
  the separate failure-reporting work that carries a failure code from the shader to
  the host. Until that lands, the capture reports the count and the first pixel's
  coordinates.

### Budgets: the polygonal-light case

A capture does not lift the 8-emissive-triangle limit of polygonal-light-sampling.
That limit comes from the
[scene interface's query budget](shared-scene-shader-contract.md#build-history-and-rollout).
`visibility_queries` is a per-sample bound, at most 16, compiled into the package,
and `TestVisibility` enforces it per launch invocation. It is not a frame-time
setting, so running off the display loop leaves it unchanged. The repository does
not record why the maximum is 16.

Removing the limit for capture is a contract decision, separate from this design.
The options, in order of preference:

1. **A capture profile in the scene interface.** The manifest declares interactive
   and capture query budgets, and the cook compiles a capture variant of each ray
   stage. Larger budgets make each dispatch longer, so the capture variant
   dispatches the image in tiles to keep each command buffer below the GPU watchdog
   (seconds on macOS and Windows). Tiled dispatch needs a renderer extension, and
   its measured cost sets the capture maximum.
2. **A batched or emitter-scaled visibility query** whose budget follows the scene's
   emitter count. This changes the contract for every traversal, interactive
   included.
3. **Raising `MAX_VISIBILITY_QUERIES`** for every technique. This is simplest, but
   the limit stays and every interactive graph pays for it.

Peters' method still costs two shadow rays per light per pixel, so a scene with
thousands of emitter triangles stays expensive with any option. The
[RIS template](many-area-light-sampling-reference-review.md) covers many lights by
selecting among them.

### File format

`LUHDR01`'s header describes RT1 settings that a graph does not have. A graph
capture uses its own header with the same pixel record (three FP32 mean channels and
a uint32 committed count, top-left order):

- magic and version;
- runtime identity (SHA-256), configuration name, output route and Accumulate
  operation id;
- extent, committed and target samples, scene digest and revision;
- world-to-clip matrix, eye position and projection flags (orthographic, reverse-Z,
  infinite far);
- flags: partial, failed, raster-only and sample-limit override.

Following the module layout rule in [AGENTS.md](../../AGENTS.md),
the record layout lives in a header-only format target that the encoder,
`scripts/reference-hdr` and readers share. `scripts/reference-hdr` learns the second
header, and comparisons accept either file. The PNG beside it uses the graph's own
display output, with its ToneMap, so it matches what the viewport shows.

### Editor workflow

The **Reference…** window becomes **Capture…**, with a **Source** choice:

- **RT1 reference**, the current behaviour.
- **Render graph**, with package, configuration and output defaulting to the
  viewport's selection.

Long side, samples, **Stop** and **Save…** keep their meaning. Scattering depth and
traversal apply only to RT1. The comparison can then put a graph capture against an
RT1 capture of the same frozen camera. That replaces today's frame-capped raster
side (`RASTER_CAPTURE_MAXIMUM_FRAMES`) with a converged graph image, and the
difference statistics keep their meaning.

## Acceptance

1. **Determinism.** Two captures with identical inputs produce bit-identical files
   on one device and build.
2. **Sample count.** A capture with target `N` commits exactly `N` samples, both
   below and above the authored Accumulate limit (the latter with the override). A
   capture stopped early saves exact counts and the partial flag.
3. **Linear parity.** The captured mean equals the graph's displayed mean after
   the same sample count, up to the output texture's rounding. The FP32 file is
   never rounded through RGBA16F.
4. **Isolation.** Regression tests show the live viewport's drawable generation,
   accumulation count and pending samples are unchanged by a capture's start,
   success, failure and cancellation.
5. **Failure.** A route with latched pixels, such as polygonal-light-sampling on a
   scene with more than eight emissive triangles, ends `Failed` with the latched
   count. It is never `Complete`.
6. **Rejections.** A ray route without an Accumulate, a target above the limit
   without the override, and a stale or unbuilt package each reject with a repair
   message and change nothing.
7. **Format.** Encode, decode and `scripts/reference-hdr` conversion round-trip, and
   an RT1 `LUHDR01` file still converts unchanged.
8. **Platforms.** Metal first, Vulkan in batch CI. The browser editor has no project
   graph viewport, so graph capture is unavailable there and the window says so.

## Phases

1. **Engine capture and file.** `RenderGraphCapture`, the Accumulate mean/count
   copy, the sample-limit override for a private producer, the graph capture
   header and `scripts/reference-hdr` support. Includes acceptance items 1–7, with
   tests on the existing template fixtures.
2. **Editor.** The **Capture…** window with its source choice, and a graph capture
   as the comparison's raster side.
3. **Capture budgets.** Decide among the [budget options](#budgets-the-polygonal-light-case),
   measure tiled dispatch against the GPU watchdog, and revise the scene interface
   contract if that decision requires it.

## Phase 1 implementation notes

Phase 1 delivers `graphics::renderer::RenderGraphCapture`
(`modules/graphics/renderer/include/ludus/graphics/renderer/render_graph_capture.hpp`),
two `RenderGraph` operations it uses, the `.lurgcap` layout
(`modules/content/render_graph_format/include/ludus/content/render_graph_format/capture.h`)
and `scripts/reference-hdr` support. Acceptance items 1–7 are tests tagged
`[render-graph-capture]` in `render_graph_ray_tests.cpp`, using the
polygonal-light-sampling template on the Cornell and many-emitter rooms, and the
`GraphCaptureExportTests` in `scripts/python/test_ray_reference.py`.

**Graph operations.**
- `RenderGraph::SetAccumulationLimitOverride` replaces every positive authored
  Accumulate limit for one graph instance, up to `MAX_RENDER_GRAPH_CAPTURE_SAMPLES`
  (65,536). Like a ray path capture request, it is called after initialization and
  before the first scene. Interactive graphs keep `MAX_ACCUMULATION_SAMPLES` (4096).
- `RenderGraph::GetAccumulation` borrows a ray pass's FP32 accumulation buffer after
  its producer completes. This promotes the test-only accessor to a supported
  operation.

**Departures from the design above.**
- **Raster routes are rejected, not captured as one frame.** A raster writer stores
  display-encoded 8-bit colour, which is not linear radiance. The capture reports
  `RasterRoute`.
- **The scene is submitted separately.** `RenderGraph::Update` needs an initialized
  graph and borrows its inputs only for the call, so `Start` resolves the route and
  initializes, and `SubmitScene` follows once the phase is `AwaitingScene`. The
  scene and camera are frozen from then on.
- **`Service()` takes no time budget.** It submits at most one frame per call and
  never waits. The graph cannot render again before its previous producer finishes,
  so a budget could not admit more frames.
- **Read-back happens only at the end.** The accumulation is copied when the target
  is reached or after `Stop`. Reading it mid-capture would stall the next frame, so
  progress previews belong to phase 2.
- **The override is always set.** The capture sets it to the target even below the
  authored limit, so the number of submitted frames equals the committed count. The
  file sets `LimitOverride` when the target differs from the authored limit.
- **The file carries identities instead of names.** The configuration is bound by the
  caller's package identity (the SHA-256 of the cooked bytes), and the scene by an
  optional caller-supplied scene identity. The header has no configuration name or
  scene digest. Latched pixels are counted from the accumulation's odd history tag,
  and the count is stored with the `Failed` flag.
- **The surface is the caller's.** Frames are submitted to it as with
  `RenderGraph::Render`. Phase 2 must give the editor's capture a surface that does
  not present over the viewport.

## Phase 2 implementation notes

Phase 2 adds `ViewportGraphCapture`
(`apps/editor/src/internal/viewport_graph_capture.h`) and the reference window's
**Source** choice, documented in
[Capture the render graph](../development/editor-workspace.md#capture-the-render-graph).

- **Engine additions.**
  - `RenderGraphCaptureSettings::PreviewInterval` adds progress read-backs after the
    first sample and every interval; the editor uses 16. Each one pauses rendering
    until the slices are copied, then resumes. A latched pixel found by a progress
    read-back ends the capture.
  - The capture now rejects a surface-backed output, since rendering it would present
    the capture's frames.
- **Surface.** The capture submits through the viewport's surface, as the existing
  raster comparison does. The editor previews only exported offscreen colour, so no
  frame reaches the viewport.
- **Comparison.** With **Compare** on, the window first tries a graph capture at the
  reference's sample count. If the output cannot be captured, it falls back to the
  frame-capped raster capture and states the reason.
  `CompareGraphCaptureWithReference` shares one per-pixel core with the raster
  comparison and compares in linear space.
- **Departures.**
  - The toolbar keeps **Reference…** rather than becoming **Capture…**, which keeps
    the toolbar within its width budget.
  - **Stop** discards a graph capture like a reference capture.
  - A graph capture inherits the viewport's single camera for every plan camera, as
    the preview does.
  - It rejects incomplete scene coverage instead of drawing the diagnostic colour.

## Open questions

- **Why 16?** The scene interface's maximum of 16 visibility queries has no
  recorded reason. The capture-budget decision should establish one by measurement
  before choosing an option.
- **Seed control.** Should a capture accept an explicit seed, as RT1 does, beyond
  the camera and scene revision that already determine graph sampling?
- **Several outputs.** Should one capture save every output of a configuration,
  such as polygonal-light-sampling's four estimators, sharing the same frozen
  inputs?
- **Memory.** Readback buffers at the 2,048-pixel long side cost about 64 MiB each.
  Should captures share the reference capture's 1 GiB CPU budget, or have their own?
