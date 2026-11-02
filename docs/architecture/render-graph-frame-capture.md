# Render graph frame capture

Status: proposed October 29, 2026. Phase 1, the engine capture and its file, is
implemented as of October 30, 2026, with the departures listed under
[Phase 1 implementation notes](#phase-1-implementation-notes). Phase 2, the editor
source and comparison, is implemented as of October 30, 2026; see
[Phase 2 implementation notes](#phase-2-implementation-notes). Phase 3, the capture
budget decision, was measured and decided the same day; see
[Phase 3: capture budget decision](#phase-3-capture-budget-decision). Phase 4a, the
capture budget and its package, is implemented as of October 30, 2026; see
[Phase 4a implementation notes](#phase-4a-implementation-notes). Phase 4b, the tiled
submission, is implemented as of October 30, 2026; see
[Phase 4b implementation notes](#phase-4b-implementation-notes). The editor's
automatic use of the capture profile, split out of phase 4b as phase 4c, is
implemented as of October 30, 2026; see
[Phase 4c implementation notes](#phase-4c-implementation-notes). Phase 5, capturing
a library output the viewport does not draw, is implemented as of November 1, 2026;
see [Phase 5: library captures](#phase-5-library-captures).

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
   contract if that decision requires it. Decided:
   [Phase 3: capture budget decision](#phase-3-capture-budget-decision).
4. **Capture profile.** Implement the decision. This covers:
   - `capture_visibility_queries` in the scene interface, with the build admission
     and the editor control;
   - the cooked capture variant of the ray stages;
   - tiled dispatch for the capture's graph, sized from the measured cost per light;
   - the polygonal-light template's capture budget.

   Show it on a scene with more than eight and at most 128 emissive triangles.
   Delivered in two parts:
   - **4a:** the budget, the capture profile package and the template;
   - **4b:** tiled submission and the editor's use of the profile.

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
- **The route can be checked without a device.** `FindRenderGraphCaptureProblem`
  applies `Start`'s route check to a plan alone. The editor records it for every
  resource when it loads a generation, so the reference window can explain a
  rejected output, and disable capturing it, before anything is created.
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

## Phase 3: capture budget decision

### What the budget bounds

`visibility_queries` is a count and nothing more. The generated wrapper stores the
technique's value in `RayQueryContext::MaximumVisibilityQueries`
(`scripts/python/ludus_tools/render_graph_ray.py`). `TestVisibility` in
`scene_shader.slang` compares a per-invocation counter with it and latches failure
when the count is reached. No storage, register or record size scales with the
budget, so raising the maximum costs nothing until a shader issues more queries.
The maximum of 16 arrived with the shared scene contract (`3f1ecd53`, October 20,
2026), and neither that commit nor the contract records a reason. What the bound
does in practice is cap the work one pixel can do in one dispatch.

### Measurement

The hidden test "Measure the GPU cost of polygonal-light shadow queries per
emissive triangle" (`[.capture-budget-measurement]` in
`render_graph_ray_tests.cpp`) measures this. It renders the polygonal-light
template at 288 × 192 (55,296 pixels) in the many-emitter room with 2, 4, 6 and 8
emissive triangles: same geometry, only the first tiles emit. It reports the
median per-pass GPU time over 64 frames after 8 warm-up frames, from
`rhi::GpuPassTimings`. Apple M5, Metal, two runs:

| Emissive triangles | Clamped MIS (ms) | Balance (ms) | Projected solid angle (ms) | LTC (ms) |
| --- | --- | --- | --- | --- |
| 2 | 1.59 / 1.56 | 1.50 / 1.51 | 0.81 / 0.81 | 0.94 / 0.93 |
| 4 | 3.03 / 3.18 | 2.94 / 3.18 | 1.68 / 1.73 | 1.88 / 1.92 |
| 6 | 5.06 / 4.72 | 5.27 / 4.59 | 2.90 / 3.22 | 3.03 / 3.26 |
| 8 | 6.93 / 6.38 | 6.86 / 6.27 | 3.63 / 3.38 | 4.09 / 3.79 |

The cost is linear in the number of lights.
- **Two-sample passes:** each emissive triangle adds about 0.8 ms, about 15 ns per
  pixel. This covers both samples and both shadow queries.
- **Single-sample passes:** about 0.45 ms, or 8 ns per pixel.

**Caveat.** These are lower bounds. The room's BVH is tiny, and deeper geometry
makes every shadow query more expensive. GPU clocks on this machine also vary
between runs, so compare within a run.

### What the numbers imply

- **Per-sample cost.** At 1920 × 1080, one clamped-MIS sample costs about 31 ms per
  emissive triangle:
  - at today's maximum (8 triangles), about 0.25 s;
  - for the staircase's thousands of emissive triangles, about a minute for one sample
    per pixel of one pass.
- **Budget cost.** Raising the budget costs nothing in itself. The time an author
  actually spends is pixels × lights × about 8 ns per shadow-sampled light per
  technique.
- **Dispatch time.** That time is bounded per dispatch by the platform's GPU timeout.
  Windows resets the display driver after two seconds by default (TDR). Metal ends a
  command buffer that runs too long with `MTLCommandBufferError.timeout`, but
  documents no fixed limit.
- **Interactive graphs.** For an interactive viewport the bound protects frame time
  and the watchdog. A viewport of 2.6 megapixels with 64 triangles would already take
  about 2.5 s per frame. So the interactive maximum should stay low.

### Decision

**Adopt option 1, a capture profile in the scene interface.** Phase 4 implements it.

- **Manifest.** A technique's `scene_interface` may declare
  `capture_visibility_queries`, defaulting to `visibility_queries`, up to a separate
  capture maximum. 256 is proposed: 128 triangles for two-sample estimators.
- **Cook.** It produces a capture variant of the ray stages with that budget, as the
  ray path capture already cooks a capture variant. Only `RenderGraphCapture` loads
  the variant; the viewport keeps the interactive budget.
- **Tiled dispatch.** The capture's graph renders the image in tiles. Each tile's
  estimated time, at the measured cost per light per pixel times its pixels, stays
  under a dispatch target of about 0.25 s, well under the two-second TDR default. This
  needs a renderer extension: a pass dispatched over a pixel sub-rectangle with
  accumulation unchanged.
- **Polygonal-light template.** It declares `capture_visibility_queries: 256`. Peters'
  protocol then covers up to 128 emissive triangles in a capture. At 512 × 512 and 64
  triangles, one clamped-MIS sample is about 0.25 s, and 1024 samples of the four
  passes about 10 minutes.

**Why not the other options.**
- **Option 2**, a batched or emitter-scaled query, removes the count but not the
  linear time. It is a contract change for every traversal, interactive included.
- **Option 3**, raising `MAX_VISIBILITY_QUERIES` for every technique, would let an
  interactive graph exceed the GPU timeout with one authored number.
- **Many-light scenes** such as the staircase remain impractical for Peters' protocol
  under any option, by its own linear cost. The RIS template is their estimator.

## Phase 4a implementation notes

**The contract.** `scene_interface.capture_visibility_queries` is optional, and
admitted from `visibility_queries` up to `MAX_CAPTURE_VISIBILITY_QUERIES` (256,
`render_graph_ray.py`).
- `build_package(capture_profile=True)` replaces `visibility_queries` with it in a
  copy of the ray technique, so every ray wrapper of that build, plain and
  persistent, links the larger budget.
- The recipe carries `capture_profile`. `cook_runtime` refuses such a build, and
  `cook_capture_profile` writes it under `CAPTURE_PROFILE_PACKAGE_MAGIC` ("LURC").

**Packages and entry points.**
- CMake: `ludus_compile_render_graph(... CAPTURE_PROFILE_PACKAGE)` cooks
  `<NAME>.lurgcp`. It cannot be combined with `RUNTIME_PACKAGE` or
  `RAY_PATH_CAPTURE`.
- `RenderGraphPackage::DecodeCaptureProfile` is the only entry point that admits the
  magic. `RenderGraphActivation::Stage` refuses a capture profile, as it refuses a
  ray path capture.

**The template.**
- The polygonal-light template declares `capture_visibility_queries: 256`.
- Its shader now checks the light count against the linked
  `MaximumVisibilityQueries`, two per triangle, instead of a fixed 8. Its random
  layout bound rises to 128 triangles, which is 2 + 4 × 128 dimensions, inside the
  addressed-random range.
- Interactive behaviour is unchanged: eight triangles, then the failure colour.

**The editor.**
- The compute-template merge raises `capture_visibility_queries` like the other
  budgets.
- Editing **Visibility queries** keeps a declared capture budget at least as large.

**Evidence.** "A capture profile lets a frame capture shade the 32-emitter room that
the interactive budget rejects" (`render_graph_ray_tests.cpp`):
- `Decode` refuses the cooked `.lurgcp`, and `DecodeCaptureProfile` admits it;
- `RenderGraphCapture` completes two samples of the 32-emitter room with no latched
  pixel, which the interactive package fails.

Python tests cover the field's admission and the build, cook and refusal paths.

**Phase 4b** tiles the submission; see the next section.

## Phase 4b implementation notes

**Why separate submissions.** Tiles inside one frame would share one command buffer,
and Metal's timeout applies per command buffer. So each band of a sample is its own
submission: one `RenderGraph::Render`, one command buffer.

**The ABI.**
- The ray uniform grows from 224 to 240 bytes with a named `Tile` region: origin x
  and y, then the exclusive end x and y. The C++ `GpuPreviewUniform`, both Slang
  `Parameters` blocks and the cook's reflection check (`PARAMETERS_BYTES`) agree.
- `ComputeMain` adds the origin to the dispatch thread and returns outside the
  region. Pixel indices, sample streams and accumulation are therefore those of a
  whole-image dispatch. The polygonal-light template's hit record, indexed by the
  full extent, keeps working, because every pass of a frame dispatches the same band.
- **Package version 28.** It marks kernels that read the region.
  - Older packages still load for interactive use. Their kernels read only the first
    224 bytes and cover the whole image on every dispatch.
  - `RenderGraphPrograms::HasRayTiles` is true for version 28 packages and for every
    generated header. `RenderGraph::SetRayTileCount` refuses more than one band
    without it.

**The graph.**
- `RenderGraph::SetRayTileCount` splits every sample into up to
  `MAX_RENDER_GRAPH_RAY_TILES` (128) row bands. Band edges are
  `floor(band × height / count)`. When bands outnumber rows, an empty band
  dispatches one group row whose threads all return.
- Before each frame, the renderer prepares for every ray pass one uniform copy
  whose region selects the cursor's band, and its binding set. It builds them from
  the committed camera and replaces the previous band's pair, which retires with
  its completed producer. One pair per band would not scale to 128 bands: the RHI
  bounds live buffers and binding descriptors per kind, by default 64 records and
  512 descriptors. A camera commit releases the pair.
- A sample counts once its last band is submitted. This applies to
  `GetSubmittedRaySamples`, `HasPendingSamples` and the capture's
  `SubmittedSamples`. The band layout changes only at a sample boundary.
- `GetRayWorkload` reports the pixels of one sample, summed over ray passes, and
  the largest pass scene's emissive triangles plus analytic lights.

**The capture.**
- `RenderGraphCaptureSettings::TileCount` defaults to zero, which means
  `ComputeRenderGraphCaptureTileCount` of the scene's workload.
- That estimate charges each pixel one camera query plus two shadow queries per
  light. The shadow queries are capped at the capture maximum of 256, since a
  kernel cannot exceed its linked budget. Each query costs the measured 8 ns, and
  each band targets 0.25 s.
- At 1920 × 1080 with 32 triangles that is five bands per pass.
- A sample that needs bands from a pre-28 package fails with `UntiledPackage`.
- **Stop** between bands keeps exact per-pixel counts. Rows of the finished bands
  then hold one more sample than `SubmittedSamples`.

**Evidence** (`render_graph_ray_tests.cpp`, `render_graph_tests.cpp`):
- "A tiled render graph capture accumulates exactly the pixels of an untiled one"
  compares the polygonal-light template's four-pass capture at 1, 5 and 128 bands
  of 64 rows, bit for bit.
- "A tiled render graph counts a sample once every band was submitted" checks the
  counters and the per-pixel counts after one round.
- Further cases cover the refusal of bands from an untiled package, and the band
  estimate.

**Phase 4c**, the editor's use of the capture profile, follows.

## Phase 4c implementation notes

**The rebuild.**
- `render-graph capture-profile <generation> <profile-build> <destination>` compiles
  a published generation's capture profile, in `render_graph_reload.rebuild_capture_profile`.
  Like `ray-path-capture`, it uses the generation's saved `sources/`, never newer
  edits, so the profile renders what the viewport's generation renders.
- A graph whose `capture_visibility_queries` equals its interactive budget, or is
  absent, compiles nothing. The rebuild reports `"capture_profile": null`, because
  the runtime package already is the capture package.

**The editor.**
- `RenderGraphCaptureProfile` holds the profile of one generation: Building, Ready
  (with a path, or empty for the runtime package) or Failed. A newer generation
  supersedes a build, and a later request retries a failure.
- `RendererWorkspace` answers its requests with the `capture-profile` job.
  - Profiles live in `out/render-graph/<graph>/capture-profiles/<generation>.lurgcp`
    and are reused per generation. A new profile removes earlier ones.
  - A generation without saved sources, such as the prebuilt engine default, is
    Ready with the runtime package.
  - The workspace runs one job at a time, so a request during another job fails
    with that reason, and the next request retries it.
- The Reference window requests the profile while it is shown with **Render graph**
  or a comparison selected, and again when the viewport's generation changes.
  - It reads the generation identity from the runtime package, decoding it again
    only when the file changes.
  - **Capture viewport** waits while the profile builds, so the frozen camera is
    the one current when the capture starts, not one from before a long compile.
  - `ViewportGraphCapture::Load` decodes the ready profile with
    `DecodeCaptureProfile` in place of the runtime package. The profile shares the
    package's plan, so the output resource and pass indices carry over. The
    capture file's package identity names the profile's bytes.

**Evidence.**
- Python: "test_capture_profile_rebuilds_a_generation_only_when_it_declares_a_larger_budget"
  covers both the compile-nothing case and the saved-sources rebuild.
- Editor: "A viewport graph capture decodes its generation's capture profile in
  place of the runtime package" completes a capture from a cooked profile fixture.
- Further editor cases cover the request state machine and the refusal of a
  missing profile or a runtime package named as one.

## Phase 5: library captures

The goal "capture any cooked project graph, not only the one the viewport draws"
was left at the viewport's own selection by phases 2 to 4. In practice the graphs
worth capturing offline are slow reference estimators, such as the
`salesin-jarosz-lines` template, applied as library configurations beside the
interactive graph. Making one of them the viewport's output to capture it is
impractical: it runs at a few frames per second.

**The source.** The window's **Graph** list offers **Viewport output** and every
ColorTexture output of every library configuration in the package's saved graphs
(`ListCaptureTargets`). A library output is a `RenderGraphCaptureTarget`: library
graph, configuration and output.

**The package.** `render-graph capture-library <generation> <output> <destination>
--library L --library-configuration C --library-output O`
(`ludus_tools.render_graph_library_capture`) rebuilds the published generation's
saved sources, like `capture-profile`, so the capture renders exactly what that
generation was built from. It copies them into its own build tree and adds a
generated root, `capture.lurender.json`, with a copy of the build manifest:

- Scene, Camera and a RenderSize colour Target feed the library's Scene, View and
  ColorTexture inputs, as in the built-in template roots. Other input types are
  rejected with a repair.
- The chosen output goes through Accumulate (at `MAX_ACCUMULATION_SAMPLES`) and
  ToneMap to the `output` Export. An output whose producer is already an Accumulate,
  ToneMap or Denoise goes straight to it.
- Every other required output feeds its own Export, so the graph validates.

It cooks the runtime package to `destination` and, when the build declares a larger
capture budget, the capture profile beside it. The viewport's generation, build
tree and runtime package are never written.

**The editor.** `RenderGraphLibraryCapture` holds one request (generation and
target): Building, Ready or Failed, like `RenderGraphCaptureProfile`.
`RendererWorkspace` answers with the `capture-library` job and keeps packages in
`out/render-graph/<graph>/library-captures/`, named by generation and target and
reused per generation. A finished build removes other generations' packages. The
window requests the package while it is shown with that output selected, waits for
it before capturing, and loads it with its own capture profile. The root's other
Exports, for required outputs, can come first in execution order, so the window
names the captured Export (`LIBRARY_CAPTURE_EXPORT`, `LibraryCapture/output`) and
resolves its resource through the package's names sidecar
(`ViewportGraphCapture::FindExportResource`).

**Limits.**
- Only libraries the root graph imports are saved with a generation. Another
  library fails with a request to insert it on the root canvas and build again.
- The first build of a generation compiles every program of the package in its own
  build tree, since it does not share the viewport build's cache.
- **Force raster** and the raster fallback do not apply to a library capture.

**Evidence.**
- Python `test_render_graph_library_capture.py`: generated roots compile for each
  `salesin-jarosz-lines` output, accumulated outputs skip Accumulate, unknown
  names and unsupported inputs explain the choices, and, with the shader tools, a
  package is cooked from a generation that later edits and the build tree do not
  change.
- Editor `render_graph_library_capture_tests.cpp`: target listing and the request
  state machine. "The Reference window offers each saved library output, and a
  library capture needs a built generation" covers the Graph list and the
  unbuilt-generation repair.
- Editor GPU test "A library capture names its captured Export rather than the
  package's first exported colour": a configure-time fixture writes the
  `salesin-jarosz-lines` capture root with `write_capture_sources`, and the capture
  resolves `LibraryCapture/output`, which differs from the required colour's Export,
  and completes.

**Resource names fix.** The cooker's names sidecar left out every Buffer resource,
while the runtime plan keeps per-pixel Buffers such as a template's hit record. The
editor then rejected the sidecar for listing a different number of resources, so
every graph with a per-pixel Buffer showed numeric resource names and no Export
could be found by name. `lowered_resources` now gives `lower_plan` and
`encode_resource_names` one list; only structured buffers, which have no record,
are left out.

## Open questions

- **Why 16?** No reason is recorded; see [what the budget bounds](#what-the-budget-bounds).
  The capture profile above leaves 16 as the interactive maximum. A future change
  could derive both maxima from a frame-time target instead.
- **Seed control.** Should a capture accept an explicit seed, as RT1 does, beyond
  the camera and scene revision that already determine graph sampling?
- **Several outputs.** Should one capture save every output of a configuration,
  such as polygonal-light-sampling's four estimators, sharing the same frozen
  inputs?
- **Memory.** Readback buffers at the 2,048-pixel long side cost about 64 MiB each.
  Should captures share the reference capture's 1 GiB CPU budget, or have their own?
