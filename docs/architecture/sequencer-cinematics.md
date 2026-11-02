# Sequencer, cinematics and controlled image sequences

Status: design recorded November 2, 2026, against `ec0cd669`. The first SQ0
runtime foundation is implemented under [runtime sampling](#runtime-sampling).
Source/cooked sequence assets, durable binding resolution, the editor timeline,
shot selection and the sequence capture runner remain proposed. Existing camera,
geometry and capture facilities are identified under [integration](#integration-and-ownership).

The first deliverable is a repeatable camera traversal through an authored
World, exported as numbered rendered images. Authors can also animate scene
objects to exercise transforms, occlusion and lighting under controlled inputs.
Use a small typed sequencer with absolute-time evaluation and an offline capture
runner. Interactive playback and export consume the same evaluated scene.

This page owns sequence timing, binding, track generation, playback and
multi-frame capture. [Camera systems](camera-systems.md#rail-cameras-and-cinematic-tracks)
owns camera evaluation; [geometry](curves-surfaces.md) owns prepared curves and
distance maps; [frame capture](render-graph-frame-capture.md) owns converging and
reading one frozen render-graph image. Keep those contracts in their owners.

## Runtime sampling

The installed `Ludus::GameplaySequencer` target under `modules/gameplay/sequencer`
provides `time.hpp` and `sequence.hpp`. It depends on Base, Containers, Math and
GameplayCamera; it has no Qt, World, RHI, filesystem or renderer dependency.
This is the runtime portion of SQ0, not completion of SQ0's asset/cooker contract.

`TryGetSequenceFrameTime` computes exact nonnegative whole/fractional ticks from
an absolute output ordinal and rational rate. It reduces fractions and decomposes
the ordinal before checked multiplication, without nonstandard integer types or
floating-delta accumulation. Negative times and pre-roll are not admitted yet.

`TryPrepareSequence` accepts borrowed object/camera groups and copies them into
an immutable move-only `PreparedSequence`. All groups have unique nonzero
sequence-local binding IDs. The caller resolves durable source identities before
constructing these groups; IDs are not persisted runtime handles. Each group owns
one writer per channel by construction. Duplicate groups are rejected rather than
merged or prioritized. Preparation validates all key times/values before allocating
exact array capacities. Failed preparation retains the accepted revision.

Object channels cover parent-local `TransformTRS` translation/rotation/positive
scale and Step visibility. These local transforms use FoundationMath's float32
representation. Camera positions remain float64 world coordinates; camera
orientations and lenses use the existing camera types. Sampling does not compose
parents, narrow world positions, select shots or update camera metadata. Those
operations stay at the future scene/viewport adapter boundary.

```cpp
#include <ludus/gameplay/sequencer/sequence.hpp>

using namespace ludus::gameplay::sequencer;

SequenceStatus SampleEntrance(ludus::gameplay::camera::CameraSample& outSample) noexcept
{
    const SequencePositionKey entrance{0, {0, 1, 10}};
    const SequenceCameraTrack cameraTrack
    {
        .BindingId = 1,
        .PositionKeys = {&entrance, 1},
    };
    PreparedSequence sequence;
    const SequenceStatus preparation =
        TryPrepareSequence({ .Cameras = {&cameraTrack, 1} }, {}, sequence);
    if (preparation != SequenceStatus::Success)
    {
        return preparation;
    }
    SequenceTime time;
    const SequenceFrameSchedule schedule{ .Rate = {30000, 1001}, .TicksPerSecond = 24 };
    const SequenceStatus timing = TryGetSequenceFrameTime(schedule, 1, time);
    if (timing != SequenceStatus::Success)
    {
        return timing;
    }
    // time is exactly 1001/1250 of one tick; the single position key holds.
    return TryEvaluateSequenceCamera(sequence, 1, time, outSample);
}
```

Prepare once and retain the revision when sampling a traversal repeatedly; this
small example deliberately contains its whole ownership/error boundary.

`TryEvaluateSequenceObject` and `TryEvaluateSequenceCamera` publish one complete
binding value at a time, allocate nothing and preserve outputs on failure. They
are independent of earlier calls, service cadence and seek direction. A future
scene adapter must stage all binding values before publishing a complete frame;
these per-binding APIs do not establish that frame transaction by themselves.

Empty channels retain base values, single keys are constant, exact interior keys
select outgoing spans and times outside a channel hold its nearest endpoint.
Step/Linear vector and lens tracks and Step/shortest-path spherical rotation
tracks are implemented. Projection changes require a Step. Quaternion keys/base
rotations within the named unit tolerance are normalized in the owned copy;
larger errors are rejected. The half-turn policy below is shared by object and
camera rotation sampling. Source values are never rewritten.

Span durations are bounded by `MAX_SEQUENCE_SPAN_TICKS` (2^53 ticks) so their
integer local timing values convert exactly to binary64. Fractional offsets and
interpolation are rounded numerical values, not exact rational transforms.
Absolute epochs may use the full uint64 tick range. Camera lens endpoints are
validated during preparation; rare interpolation rounding can still collapse
near/far planes. Sampling then returns `OutOfRange` and preserves the prior output.
Whole-span export preflight remains future capture-runner work.

`SequenceLimits` defaults to 256 groups, 65,536 total keys and an 8 MiB prepared
payload. `GetPreparedBytes` counts the storage object and exact capacities;
allocator metadata and an old accepted revision coexisting during replacement
are additional peak memory. Admission and payload arithmetic are checked before
allocation. Tests exercise an exact-byte allowance and one-byte-short rejection.

The sequencer tests cover rational schedules including the 96-frame example,
overflow and large epochs, copied source lifetime, Step boundaries, constant/base
channels, reordered seeks, no container allocation during repeated evaluation,
quaternion sign/half-turn handling, lens validation, duplicate groups, invalid
views/keys, budget rollback, moved ownership and failed-output preservation.
The installed-SDK consumer also prepares and samples a camera through exported
headers and the `GameplaySequencer` component. Renderer traversal, image export,
parent composition, browser execution and UI acceptance are not established by
these runtime tests.

## Author workflow

1. Open a World and choose **Create sequence**. The sequence references that
   World; it does not contain another copy of the scene or its imported assets.
2. Choose **Add camera track**, pilot the viewport to an entrance pose and choose
   **Set key**. Move into the scene, choose a later time and set another key.
   **Generate traversal** can instead turn ordered poses or an existing prepared
   path into keys. Preview the path, timing and orientation before applying it.
3. Select an object and choose **Add object track**. Available channels are
   translation, rotation, scale and visibility when the scene adapter supports
   them. For example, rotate a test object while the camera approaches it.
4. Scrub, play and step the timeline. The viewport shows the sequenced scene;
   stopping preview restores the ordinary World view. Saving the sequence never
   bakes sampled values into World properties.
5. Choose **Render image sequence**. Select a saved capture preset, output
   directory and frame range; inspect the resolved graph/output, camera,
   resolution, image count, memory/storage estimate and unsupported capabilities.
6. Start the job. It captures every requested time, even if one image takes
   minutes. Inspect completed images and the run manifest; cancellation retains
   committed frames and marks the run incomplete.

The initial traversal needs one camera and no animated objects. Creating a track
means creating authored animation data, not generating geometry or inventing
scene lights. Object population belongs to [procedural generation](procedural-content-generation.md).
Manual camera navigation recording is optional later work; the first workflow
uses explicit poses and deterministic generators.

## Scope and selected decisions

| Concern | Initial contract |
| --- | --- |
| Scene behavior | Kinematic sequence sampling; physics, scripts, audio and gameplay do not advance |
| Time | Integer authored ticks, rational output frame rate and absolute evaluation |
| Object animation | Typed translation, quaternion rotation, scale and visibility channels |
| Camera | Pose and lens channels plus a camera selection/cut track |
| Multiple writers | Reject conflicting writers; no hidden track priority or implicit blending |
| Preview | Transient overrides over an immutable World snapshot |
| Export | Offscreen image sequence, frozen inputs, bounded work and explicit completion |
| Test default | Independent images: reset render history at each output time |
| Reproduction | Exact times/inputs; image tolerances tied to renderer/backend capabilities |

Nested sequences, skeletal clips, audio synchronization, arbitrary property
reflection, event tracks, automatic object spawning, motion blur, editorial shot
blends and video encoding are outside the first implementation. A sequencer
does not require the proposed skeletal animation runtime. It must remain useful
with ordinary raster rendering; reference path tracing is optional.

## Assets and bindings

Introduce a versioned source sequence document, provisionally `.luseq.json`,
under the project's authored content, and a cooked sequence asset for runtime
and SDK consumers. These names are proposals, not recognized file types today.
The source records a sequence identity, World reference, timebase, playback
range, bindings, tracks and generation provenance. A separate capture preset
records render/output settings so the same motion can test several graphs.

Each binding has a durable sequence-local identity and an explicit target kind:

- **World object:** World identity plus source-object identity, resolved through
  the existing [object identity](object-handles-uuid.md) boundary. Display names,
  list indices, pointers, runtime entity handles and GPU indices are not durable
  bindings. A source object that expands into several draw objects needs an
  adapter-owned stable subobject key; ambiguous expansion is rejected.
- **Sequence camera:** a camera definition owned by the sequence. It is available
  without adding a camera entity to the World. An adapter may also bind an
  authored World camera when it can supply the required typed fields.
- **Legacy World object:** an explicit compatibility binding to the saved World
  resource and its version-1 scoped entity key. Pin its content identity for
  export. Renaming the display name is safe; changing/removing the key requires
  explicit rebinding. Never silently migrate or mint UUIDs while opening a sequence.

Current version-2 World records do not support the imported `scene` extension;
the first imported-scene traversal must support the explicit version-1 route.
A camera-only sequence can reference a frozen imported scene without object
bindings. Do not make broader World UUID migration a prerequisite for that case.

The scene adapter advertises capabilities per binding: local transform,
visibility, rigid camera pose and supported lens fields. Imported instances
must preserve source identity through extraction before their object tracks can
be enabled. An object that has no writable transform reports that limitation;
there is no fallback to a nearby name or similarly numbered mesh.

Preparation resolves all active bindings against one scene revision, validates
types and parent relationships, and publishes an immutable evaluation plan.
Track identifiers survive rename/reordering. Source keys remain editable; the
cook owns sorted spans, binding tables and source maps. Unknown schema versions,
duplicate identities, duplicate key times and unknown channel kinds are errors.
Load/prepare failure preserves the accepted document and prepared revision.

Define source-byte, track/key-count, prepared-memory, evaluated-object and capture
budgets in their respective owners. Checked arithmetic validates combinations,
including image bytes and frame counts. The built-in fixture must fit these
limits. Numeric budgets are selected with the first implementation's measured
fixtures, rather than copied from unrelated World or renderer limits.

## Time and evaluation

The sequence declares positive integer ticks per second and integer key times.
Its output rate is the positive rational `P / Q` frames per second; support
rates such as `30000 / 1001` without rounding them to an integer. Reduce rational
values and check multiplication, addition and conversion ranges before use.

Capture uses an explicit sequence start tick `A` and output-frame ordinal `n`:

```text
sampleSeconds(n) = A / ticksPerSecond + n * Q / P
sampleTicks(n)   = A + n * ticksPerSecond * Q / P
```

`sampleTicks` is rational: do not round it onto the authored tick grid. Compare
it with key times using checked rational arithmetic; convert only the local
span offset/duration to floating point for interpolation. Never accumulate
floating deltas from the previous frame. Reject time ranges whose local
conversion cannot meet the declared sampling precision.

An export range is `[firstFrame, endFrame)` with a positive frame step. Emit
`firstFrame + k * step` while it is less than `endFrame`, retaining that absolute
ordinal in file names and the manifest. Empty ranges are rejected. The UI shows
the count, first time and last sampled time before starting. A terminal key at
the exclusive end can shape interpolation but is not an extra output image.
To capture it, extend the requested range explicitly. Looping preview wraps into
the playback range; export performs one explicit finite range.

Within a channel, the outgoing span owns an exact interior key. Step holds the
left value until the next key, whose value applies at its own time. Before the
first and after the last key, hold the endpoint value. A single key is a
constant channel; an absent channel uses the frozen base value. Channel mute
restores that base value. Use the same boundary rules in preview, cook and CLI.

`EvaluateSequence` conceptually takes a retained plan, immutable base snapshot
and absolute time and writes a complete value-only override packet. It has no
wall clock, GPU calls or World mutations. The caller reserves output storage;
evaluation allocates nothing and returns an explicit status. Any failure
preserves its previous complete output. Scrubbing in any order and repeated
evaluation at one time produce the same values under the same numeric profile.

The adapter applies all overrides to a candidate extraction, then publishes it
atomically. Never combine the new camera with the preceding time's objects.
An accepted document revision and its pending renderer preparation are distinct:
retain the last drawable preview while preparing a replacement or after failure.

## Transform and camera channels

Use FoundationMath conventions: right handed, +Y up, local camera forward -Z,
column-vector transforms, radians and quaternion orientation. Authoring may
display degrees, but the saved schema declares units and converts at the UI
boundary. Reuse FoundationMath vectors, quaternions, `TransformTRS` and `Affine3`;
do not introduce sequencer-specific matrix arithmetic.

| Channel | Supported interpolation and validation |
| --- | --- |
| Translation | Step/Linear first; prepared Hermite/path motion in the generation phase; finite values in declared world units |
| Rotation | Step or shortest-path quaternion interpolation; finite unit keys, hemisphere continuity and a deterministic tie policy at a half turn |
| Scale | Step/Linear first; finite strictly positive components; no zero crossing, negative scale or shear keys initially |
| Visibility | Step only; controls render inclusion without creating/destroying the bound object |
| Camera lens | Step/Linear for supported continuous fields; projection-kind changes are cuts; validate all evaluated lens values |
| Camera selection | Nonoverlapping half-open intervals with exactly one camera throughout the export range; adjacent intervals meet without a gap |

Object tracks replace parent-local channels. Unanimated channels come from the
base local transform. Compose parents before children into world `Affine3`,
preserving shear produced by that composition. Initial binding rejects cycles,
unsupported parent extraction and base transforms that cannot be represented by
the requested local TRS override without loss. World-space position authoring
may be converted using a valid parent transform at the keyed time; do not label
that a world-space constraint at every later time. Reparenting tracks are deferred.

Use float64 world positions and float64 path origins. Existing scalar prepared
tracks store float32 values: prepare bounded local offsets, retain their origin
and validate the [geometry precision floor](curves-surfaces.md). Add the origin
in float64 when publishing camera/world positions. Requests exceeding the
precision budget fail preparation; silently narrowing large world coordinates
is unacceptable. Any missing general interpolation primitive belongs in Math
or Geometry, with its own tests, rather than another spline library here.

Use Step/Linear first to keep the traversal independent of camera-director C4.
Reuse `CameraSample` and its validator, then the renderer's view construction at
the actual captured extent. Do not bypass the renderer's depth convention adapter.
The camera is rigid: animated scale on a parent cannot enter its orientation or
projection. Initial sequence cameras are unparented world poses.

Looking along a path is optional and uses Geometry's prepared transported frames
with authored roll and named cusp/up/closed-loop policies. Keyed orientations
are the baseline. Never differentiate noisy samples and improvise a global up
axis. Initial rotation tracks promise positional orientation continuity only;
they do not promise continuous angular velocity. Intentional long rotations
require intermediate keys with unambiguous adjacent arcs; winding/smooth
quaternion tracks need a later explicit contract.

For a zero dot product between adjacent quaternion keys, choose the endpoint
sign whose relative quaternion's first nonzero vector component in `(x, y, z)`
order is positive. Otherwise negate the endpoint only for a negative dot product.
This fixes the half-turn tie without a history-dependent choice; cook and runtime
must share it. Tests cover equivalent signed keys and exact half turns.

Cuts, seeks, loop wraps and definition/binding replacements signal a camera
discontinuity. Camera easing, shake and collision avoidance are off by default
for controlled tests. A later collision adapter must expose prevent, report-only
or ignore and record the selected policy and any result; geometry avoidance must
not silently change an authored benchmark path.

Only one enabled writer may own a binding/channel at a time. An ordinary
translation track conflicts with a path track that writes translation. Camera
pose writers and camera lens writers can coexist because their channels differ.
Reject conflicts with both track names; row ordering never changes the answer.

## Generate editable tracks

Generators prepare a preview candidate, not a hidden procedural evaluator.
Applying a result is one undoable sequence edit. The UI displays which tracks
are added or replaced, their key counts, time range and generated interpolation.
Manual edits survive until the author explicitly replaces them on regeneration.

| Generator | Inputs and result |
| --- | --- |
| Between poses | Two or more ordered camera/object poses with explicit times; produces ordinary transform keys |
| Travel along path | Existing path identity/revision or authored points, start time, duration or speed, timing mode and orientation policy |
| Transform sweep | Start/end translation, rotation or positive scale and duration; produces a deterministic object track |

Travel timing chooses **authored time** or **constant distance speed** explicitly.
The latter uses the existing prepared distance map and its residual/error
contract; uniform spline parameters are not constant speed. Duration derives
from path length and positive speed, or speed derives from a positive duration.
Stationary paths become an explicit hold; undefined tangent orientation requires
authored orientation or a repair, not zero-vector normalization.

Store generator type/version, source revision, parameters and any named random
seed as provenance. A generated path track may retain a prepared path reference
and time-to-distance channel instead of baking thousands of keys; export pins
both. **Bake to keys** is a separate bounded approximation with position/angular
error tolerances and a reported maximum error. Constant-speed claims apply to
the prepared path result within its error contract, not automatically to a bake.
Seeded variation, orbit and multi-object patterns can extend this interface after
the baseline; they must reuse addressed randomness and stable object identities.

## Preview and editor interaction

Add a **Sequence** document tab with a time ruler, playhead, transport controls
and rows grouped by bound object/camera. The World viewport remains the visual
surface. It shows a visible **Sequence preview** state and a choice between the
sequence camera and a detached editor camera. Detached navigation never edits
camera keys or changes the camera chosen by export.

Support add/bind track, Set key from the current pose, numeric key editing,
delete/move keys, channel mute, frame stepping, range selection and **Render
image sequence**. Keep auto-key off initially. A viewport manipulation during
sequence editing previews a candidate; **Set key** commits it to the sequence.
An explicit **Apply pose to World** is a separate, validated World edit with Undo.
Do not update World history on every playback sample.

Menus, keyboard commands and timeline context menus share document operations.
Every field available in the inspector is also reachable from the track/key
context editor, with Properties hidden. A drag is one transaction; Escape or
lost capture cancels it. A rejected key move shows its collision and repair next
to that key editor and preserves the accepted draft and buffered input.

Missing targets show the binding and offer **Rebind**. Conflicting writers offer
**Mute track** or an explicit replacement. A destructive repair, such as removing
keys or tracks, names exactly what will change in a confirmation with Cancel as
default and commits repair plus edit in one undo step. Safe normalization during
preparation may be automatic only within the documented quaternion tolerance;
larger errors preserve authored values and request a fix.

Interactive playback may skip presentations to follow its preview clock; export
never skips requested frames. Each preview sample still uses absolute time.
Leaving preview drops the overlay, including after deletion, project close or
failure. It must not restore an old copied World over newer authored edits.
Edits prepare a new base/sequence pair; callbacks carry document, sequence,
World and generation identity so stale work cannot publish.

## Capture execution

### Freeze and preflight

Starting a job snapshots the saved sequence, World, extraction bindings,
resolved assets, graph generation/configuration/output, capture profile and
preset. Unsaved documents must be saved through the normal save workflow first;
failed save leaves drafts intact and starts no job. Retain exact source/package
bytes or content-addressed copies for the run; storing paths alone cannot freeze
files that a subsequent build replaces. Later editor changes do not enter it.

Preflight validates every active channel, all bindings, camera coverage, frame
range, time arithmetic, extent, scene/renderer admission and output route.
Animated values require whole-span checks where constraints can fail between
keys, especially cubic lens/scale values. It verifies the selected renderer can
update the animated scene and resets it correctly; a camera-only implementation
must report unsupported object animation rather than ignore tracks.

Check memory/readback capacity and estimated disk use with checked arithmetic;
disk estimates are advisory because free space can change. Never raise an
authored limit implicitly. A stale/unbuilt package offers the existing explicit
Build operation; a sequence export never silently compiles a different draft.
Unsupported graph features or renderer profiles are explicit failures with the
required next step. No silent raster/CPU fallback is permitted for a test run.

### Per-frame transaction

```text
Preflight -> Preparing -> Evaluating -> PreparingScene -> Rendering
                         ^                                |
                         |                                v
                         +---- Committing <- Encoding <- ReadingBack
                                  |
                             Finalizing -> Complete
```

Each arrow represents an explicit Ready/Pending/failure boundary. The runner:

1. Evaluates the complete override packet at the exact requested time.
2. Extracts the matching scene/camera and waits for its private producer to be
   Ready. Geometry/transform changes may require acceleration rebuilds; never
   capture the last drawable frame as if it were the requested candidate.
3. Resets per-image render history and stochastic state under the preset, then
   renders a raster image or converges an admitted graph to its exact sample
   target. The evaluated scene and camera stay frozen throughout those samples.
4. Waits for readback completion, retaining producer/consumer ownership and GPU
   resources until their completion tokens retire. Encoding cannot read an
   unfinished buffer. Device loss fails the job with the failing frame ordinal.
5. Encodes into temporary files, publishes the required files, then commits a
   frame record to the run manifest. Only that record makes the frame complete.
   Advance sequence time only after commit. The initial runner permits one
   frame in flight; parallel capture is not needed for correctness.

Service work is bounded and nonblocking on the current owner thread. A worker
may encode/write owned completed CPU buffers; it cannot call RHI or borrow
mutable scene storage. GUI responsiveness and offscreen GPU workloads need
separate measurements. Do not claim that bounded CPU service prevents long GPU
dispatches; use the existing capture-profile/tile contract for expensive graphs.

Cancel stops new work and reaches a safe retirement boundary. Preserve committed
frames, mark `Cancelled`, and remove only owned temporary files after their
writers finish. A stopped/partial single-frame accumulation is diagnostic data,
not a completed sequence frame. Failure records `Failed`, its first cause and
ordinal. Completed images remain inspectable. Disk-full errors identify the
directory and offer export to another location; GPU failures name backend/device
and offer a new run after recovery.

### Render modes and history

The required mode is **Independent images**. Each output ordinal starts with
empty temporal history and the same explicitly derived initial sampling policy.
Derive a stable stochastic identity from the sequence/run seed and absolute
frame ordinal, independent of wall time, worker order, readback delay or retries.
Current render-graph capture uses scene revision in sampling; the adapter must
map this identity deterministically into that input, record it, and prove it is
decoupled from incidental editor revision counters before claiming resume parity.

Raster capture is a single completed draw of the selected output. Reuse the
private producer model, but do not inherit `ViewportRasterCapture`'s 64-frame
cap as an export convergence rule. Accumulating ray routes use
`RenderGraphCapture` and its exact target/count/failure contract. In neither case
does render sample count advance sequence time.

Temporal reconstruction, exposure adaptation and other history-dependent graph
features need declared reset support. Otherwise reject them for independent
images. A later **Continuous traversal** mode may retain history across adjacent
frames with explicit warm-up, cut reset and seek/replay rules; skipped output
frames would still require history advancement. That mode is a distinct test
profile, not a transparent optimization of independent images. Motion blur
requires shutter-time scene evaluation and is deferred, rather than approximated
by accumulating different camera times in the existing frozen-frame API.

Graph capture is not a reference oracle. Optional reference comparisons use
the existing CPU oracle and GPU reference with the same sequenced inputs and
[reference parity requirements](renderer-systems.md#cpu-and-gpu-reference-implementation-requirement).
No new transport or material feature is part of this proposal.

## Artifacts, reproduction and test use

Create a fresh run directory; do not overwrite existing frames. Proposed layout:

```text
out/captures/entrance/run-0001/
    run.json
    frames/000000.png
    frames/000001.png
    linear/000000.lurgcap   # accumulating graph profile only
    linear/000001.lurgcap
```

PNG is the required viewable output. For accumulating graph comparisons, also
save the existing `.lurgcap` linear values and exact counts. Reference captures
retain their own existing format. Raster PNG comparisons are display-space
tests; never infer linear radiance by treating display bytes as linear values.
Additional linear raster formats require an explicit renderer/encoder contract.

The manifest has a schema version and records:

- Sequence/World identities and hashes, dependencies, generator provenance and
  source/cooked revisions; frozen inputs sufficient to rerun or a precise list
  of unavailable content-addressed dependencies.
- Engine/toolchain build, renderer/backend/device/driver, graph/package/capture
  profile hashes, configuration/output route, selected capabilities and extent.
- Timebase, rational output rate, frame range/step, track/binding identities,
  history mode, seed policy, sample target, exposure and display encoding.
- Per frame: absolute ordinal and exact rational time, camera identity/pose/lens,
  evaluated scene identity, stochastic identity, file hashes, committed sample
  counts and diagnostics. Profiling times are observations, not test inputs.
- Run state (`Running`, `Complete`, `Cancelled`, `Failed`), committed frame set
  and first failure. `Complete` requires all requested records and files.

Write files and manifest replacements transactionally in the destination's
supported filesystem. A crash between file publication and manifest commit
leaves uncommitted files, never invented success. On reopening, validate hashes
and treat those files as incomplete. Preserve the accepted manifest on a failed
replacement. Browser export has its own storage/publication contract.

Resume is admitted only for independent images with identical frozen inputs,
numeric profile and render settings, verified file hashes and a tested seed/reset
contract. Initially a partial run may be inspected and the traversal restarted
in a new directory; do not claim automatic resume before that evidence exists.
Retain absolute ordinals when rendering a subset, so a subset and full run can
be compared directly.

The same prepared evaluation and capture runner should serve the editor and a
future installed-SDK/CI executable. A CLI request names World, sequence, preset
and destination and returns nonzero for invalid, unsupported, cancelled or
failed runs. This is an interface requirement, not existing command syntax.
Separate capability skips from passes in CI.

## Integration and ownership

| Owner | Reuse and required addition |
| --- | --- |
| FoundationMath / FoundationGeometry | Existing checked transforms, quaternion operations, prepared paths, scalar tracks, distance maps and transported frames; add general missing kernels here |
| FoundationIdentity / Content identity | Existing durable IDs; resolve through source adapters and preserve the explicit legacy World route |
| Proposed `ContentSequenceFormat` | Header-only Base-dependent cooked tags, versions, widths and limits; serialize fixed-width fields explicitly, never host structs/padding |
| Proposed `ContentSequenceCook` | Source parsing, validation and preparation into deterministic cooked data; tools depend on runtime math/evaluation contracts, never the renderer |
| Proposed `GameplaySequencer` | Typed immutable plan, binding/channel conflict checks and allocation-free absolute-time evaluation; depends on Foundation and GameplayCamera, not Qt, Platform, RHI or World component layouts |
| Scene adapter | Resolve source objects and apply value packets to candidate render extraction; game-specific World access stays outside GameplaySequencer |
| GraphicsRenderer | Existing `CameraSample` adapter, scene submission, GPU ownership and `RenderGraphCapture`; add the missing portable raster single-frame capture boundary |
| Proposed `RuntimeSequenceCapture` | Freeze/preflight/frame state machine and manifests using Sequencer, Resources and Renderer; no editor dependency |
| Editor / CLI | Document commands, sequence editing, job controls and filesystem/export adapters; both call the same runtime runner |

SequenceFormat's wire records store identity bytes; typed UUID conversion belongs
above that Base-only boundary. Runtime decoding belongs with Resources, not the
cooker. Prepared storage has explicit owners; borrowed views cannot outlive a
plan/snapshot revision. Public APIs use Ludus types, bounded views and explicit
status/output-preservation contracts, with `noexcept` on evaluation/service
paths. Document each new public declaration with Doxygen when implemented.

Existing [`RenderScene`](../../apps/editor/src/internal/render_scene.h) and
[`WorldDocument`](../../apps/editor/src/internal/world_document.h) are editor
adapters, not portable engine APIs. Do not link RuntimeSequenceCapture to Qt to
reuse them. Extract the narrow data/submission boundary needed by the runner,
preserve existing publication ownership and add SDK-consumer evidence.
[`ViewportRasterCapture`](../../apps/editor/src/internal/viewport_raster_capture.h)
and [`ViewportGraphCapture`](../../apps/editor/src/internal/viewport_graph_capture.h)
demonstrate private producer lifetimes; their UI/file helpers remain editor-side.

Native export is the first qualification target. Browser editing/evaluation
should retain the same asset/timing semantics, but multi-image download, memory
budgets, offscreen GPU capabilities and cancellation require separate acceptance.
An unavailable browser export explains the limitation and offers saving the
sequence/preset for a supported native runner. Do not advertise native filesystem
or GPU behavior as browser support.

## Delivery and acceptance

| Phase | Deliverable | Required evidence |
| --- | --- | --- |
| SQ0: asset and sampler | Schema, bindings, preparation, Step/Linear TRS, quaternion rotation, visibility and camera/lens/cuts | Boundary/time overflow, duplicate keys, unit/finite checks, channel conflicts, parent composition, missing bindings, precision and output preservation; reordered seeks equal direct evaluation |
| SQ1: camera traversal capture | Camera-only sequence, independent raster images, runtime runner and SDK/CLI consumer | Exact frame count/times; no skipped frames under injected preparation/readback delay; source pinning; offscreen readback ownership; same ordinal in subset/full capture; reset evidence |
| SQ2: editor and object tracks | Shared sequence commands, pilot/Set key, override extraction, animated object capture and complete UI failure paths | Properties-hidden edits, cancel/Undo, World save unchanged by playback, rejected edits preserving drafts, stale jobs, last drawable during pending/failure and correct new publication |
| SQ3: generators and converged graphs | Path/distance timing, generation preview/transactions, graph capture profile and linear artifacts | Bounded path error, frame reset/seed repeatability, exact per-image counts, animated scene rebuild correctness, graph and raster profile distinction |
| SQ4: operational qualification | Validated resume if supported, CI fixture, native platform matrix, separately qualified browser capabilities | Cancel/device-loss/disk-full/crash recovery, hashes, memory bounds, SDK parity, complete manifest criteria, visual and numerical comparisons |

SQ1 establishes the camera-only raster test path. The requested object-track
feature requires SQ2; generated traversal and converged graph workflows require
SQ3. No phase is complete merely because the editor timeline looks plausible.

The built-in acceptance fixture contains an entrance, a corridor with visibly
distinct near/far objects, an occluder and a final target. A 4-second, 24-fps
camera track exported over `[0, 96)` produces exactly 96 images at times
`n / 24`; the terminal pose key at 4 seconds is excluded. Check the first,
middle and last sampled camera poses analytically. Use authored lighting and
record the renderer profile's lighting limits.

Add one positive-scale object translating behind the occluder and rotating,
plus a visibility step on a second object. Include parent-local composition,
a camera cut on an exact frame boundary and both legacy and UUID binding cases.
Record projected landmarks/visibility expectations so a frozen camera, stale
transform or wrong output route cannot pass on image count alone.

Repeat the fixture with different GUI service cadence and injected GPU/readback
delays. Evaluated transforms, rational times and selected inputs must match;
pixel hashes are required only within a qualified deterministic renderer/device
profile. Cross-device comparisons use declared absolute/relative pixel error,
image metrics and, for stochastic graphs, statistical tolerances over raw linear
outputs. Establish thresholds with evidence; sequence determinism alone does not
promise bit-identical GPU floating-point results across drivers or backends.
Register the runnable case and evidence in the existing
[benchmark scene guide](../development/benchmark-scenes.md) when implemented.

## Consulted references and departures

Thanks to Epic Games for [Cinematics and Sequencer](https://dev.epicgames.com/documentation/unreal-engine/cinematics-and-movie-making-in-unreal-engine),
especially Tracks, Cameras and Rendering: the object/camera track authoring model
and separate image-output job inform this workflow. Ludus starts with a typed
kinematic evaluator and does not adopt the broader non-linear editing framework.

Thanks to Epic Games for [Image Settings](https://dev.epicgames.com/documentation/unreal-engine/cinematic-rendering-image-quality-settings-in-unreal-engine),
especially Output and Anti-Aliasing: explicit frame ranges, output rate and
sampling controls inform preflight. Ludus initially isolates each time sample;
temporal shutter sampling, warm-up and history-dependent rendering need their
own contracts rather than being inherited as implicit export settings.

Thanks to the Blender documentation contributors for
[Rendering Animations](https://docs.blender.org/manual/en/latest/render/output/animation.html):
image-sequence output informs retaining completed frames after interrupted work.
Ludus adds explicit committed-frame manifests and hash checks; restart/resume
claims require its own tests. These are workflow references, not copied code.
Carry concise source attribution into affected implementation boundaries when
these ideas are implemented, following the contributor guide.
