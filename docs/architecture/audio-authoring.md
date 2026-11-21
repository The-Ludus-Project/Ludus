# Audio authoring and game integration

Status: Architecture contract. The initial native implementation and pending
acceptance gates are recorded in [implementation evidence](../development/audio-content-evidence.md).
This document defines the composer workflow and runtime
contracts to implement through [the milestones](audio-content-milestones.md).
Read [content resources](content-resources.md) for identity/loading/saving and
[the Codex handoff](audio-content-codex-handoff.md) for an implementation prompt.

## Quick workflow

Ludus audio authoring keeps source audio, persistent definitions and runtime
preview state separate. Begin with a loaded game project; the editor's Audio
workspace uses its `content` root.

### A practical authoring loop

1. Compose and keep editable projects in your DAW.
2. Export supported WAV or FLAC source audio.
3. Import/register content and create a sound or music definition.
4. Edit playback settings and audition using the supported runtime-backed preview.
5. Save, reopen and validate the definition.
6. Include the resources in the game's explicit package installation rules and
   test a real runtime trigger.

An editor preview alone does not prove packaged playback or device behavior on
another machine. Check the linked evidence for accepted native paths and pending
device/platform tests.

### Identity and ownership

Content catalogs associate stable logical IDs with project-relative paths.
Definitions save IDs and authored playback data; transient device, clip, voice
and lease handles remain runtime state. Reimporting an asset should retain its
intended logical identity.

Keep source masters and authoring files under your own asset workflow. Ludus does
not replace DAW projects, instrument plugins or composition tools. For stems in
a music section, deliberate rate/length/start/loop alignment matters; validation
should report problems rather than silently trim or normalize them.

### Save and switch safely

Before changing projects or launching work that owns audio preview, stop audition
and save or discard the audio draft explicitly. Cancellation and failed save
leave the authored draft available for recovery.

References: [authoring contracts](audio-authoring.md),
[implemented content/preview evidence](../development/audio-content-evidence.md),
and [resource formats](content-resources.md).

## Historical baseline and implementation scope

The following audit describes historical baseline HEAD `9a71dd1`; the current
native C0–C5 implementation and remaining acceptance gates are in the linked
evidence. At that baseline, Audio has resident WAV/FLAC preparation, voice mixing,
buses, spatial behavior, event descriptors, scheduled clip starts and offline
rendering. `PrepareStream` and `PlayStream` return `Unsupported` in
[facade.cpp](../../modules/audio/src/facade.cpp). The
[audio task ledger](../../.kiro/specs/audio/tasks.md) records partial A4 and pending
A5/A6/A7. API declarations are not evidence of working streaming/device output.

Two additional implementation gaps are visible in that file: Device initialization
currently reports Ready without attaching a backend, and PrepareClip currently
treats source loop indices as prepared-rate indices and clamps the end. C2 must
fix source-rate conversion/rejection, and C3 must make device readiness truthful.
The intended contracts below are requirements, not descriptions of those stubs.

The editor has project/build/run code; its
[evidence](../development/editor-workspace-evidence.md) must be rechecked against
the current host. E0 deferred asset editing. This is an additive next milestone,
not a claim that E0 already includes content or native GUI acceptance.
The [world proposal](game-world.md) is also architecture, not implemented gameplay.

The first outcome is lossless source audio -> persistent definition -> preview ->
save/reopen -> packaged playback. Dynamic music follows separately. Runtime MIDI,
tracker modules, DAW project parsing, arbitrary instrument plugins, custom codecs,
general sound graphs and live game IPC are outside the initial implementation.
A composer may use MIDI in a DAW and render it to audio for delivery.

## Composer workflow

1. Compose in any tool. Keep its project and plugin/instrument dependencies there.
2. Export mono/stereo WAV or FLAC. Recommended delivery master: 48 kHz, 24-bit
   PCM WAV. Other rates accepted by the existing decoder remain valid.
3. Import/register the files, choose stable resource IDs and create sound/music
   documents. Ludus never modifies the DAW project.
4. Edit playback properties and audition the same runtime behavior the game uses.
5. Save, reopen, validate, package, and test a game trigger.
6. Export revisions and reimport under the same IDs.

A stem is a rendered musical layer. Within a section, all stems have the same
sample rate, sample count, start position and loop range, including leading
silence. Export effect tails deliberately; v1 dynamic sections use loop-ready
files without separate tail regions. Do not independently normalize stems after
balancing them together. Import validation reports alignment or peak problems;
it does not silently trim silence or normalize audio.

## Sound definition version 1

This complete example describes a resident one-shot. All shown fields are required.

```json
{
  "version": 1,
  "id": "sound/impact",
  "bus": "sfx",
  "group": "impacts",
  "priority": 4,
  "gain": 0.8,
  "rate": 1.0,
  "spatial": { "mode": "point", "min_distance": 1.0, "max_distance": 20.0 },
  "loop": { "enabled": false, "begin_frame": 0, "end_frame": 0 },
  "policy": { "cooldown_ms": 0, "suppress_while_active": false },
  "variations": [ "audio/impact-a" ]
}
```

The ID must equal its catalog ID. Bus/group names resolve against a game-supplied
immutable audio profile; they are not engine hard-coded names. Preview uses the
same profile. Reconfiguring its bus tree requires a new audio session and a
reprepare, not live mutation of the existing tree.

Priority is 0..7; gain is finite 0..1; rate uses the existing AudioSystem legal
range. Variations contain 1..16 unique audio-source IDs, selected uniformly using
the existing seeded adapter. Weighted choice and pitch randomization are deferred.
`spatial.mode` is `point` or `none`; distances are finite, min >= 0 and max > min.
In `none`, retain valid explicit distance values but ignore them; use centered
playback. Cooldown is an integer 0..600000 ms. Convert to application ticks with
checked ceiling arithmetic using its declared tick rate; editor preview uses a
documented 60 Hz policy clock. Do not derive gameplay cooldown from audio frames.

Loop frame indices are unsigned integers in original decoded source frames,
half-open [begin,end). Disabled loops require both indices zero. Enabled loops
require 0 <= begin < end <= decoded length for every variation, with the same
source rate across variations. Preparation performs checked conversion to session
frames using the audio design's intended loop contract, correcting the current
implementation gap. An enabled loop is explicitly owned;
reject attempts to emit it as a detached one-shot. Loop points live in the
resource preparation descriptor; do not assume current Play can change them.

No custom per-event concurrency graph is introduced. Existing group quotas and
adapter suppression apply. Decode to owned typed definitions; validate every
field/reference before publishing resolved descriptors.

## Simple music definition version 1

```json
{
  "version": 1,
  "id": "music/forest",
  "source": "audio/forest-loop",
  "bus": "music",
  "priority": 6,
  "gain": 0.7,
  "loop": { "enabled": true, "begin_frame": 0, "end_frame": 384000 }
}
```

This document defines one nonspatial, rate-1 music stream. ID, gain, priority and
loop validation follow the sound rules; bus resolves against the profile. No
intro/outro stitching or arbitrary seek is implied. The loader retains a source
lease, prepares a new stream instance for each play, and tracks terminal cleanup.
A stopped stream handle is not reusable. Reject Play until prefill is Ready.

A temporary resident music experiment may be offered under explicit memory limits,
but it does not complete the streaming music milestone. Do not silently load a
large track as PCM when streaming is requested.

## Editor document and preview ownership

Add an Audio workspace to the existing Qt application. Keep one state owner and
typed actions/results; preserve project build/run state. Each document holds its
ID/path, saved digest, draft, validation result and dirty flag. Save does not
require a device. Discard/reload must not lose a draft after a failed read.
Unsaved project/document switching uses the existing save/discard/cancel pattern.

Initial controls: catalog browser, source import/reimport, sound/music property
form, bounded waveform overview, numeric loop frame fields with draggable handles,
Play/Stop, bus meter and diagnostic panel. Dynamic music later adds section/layer
forms and state/intensity audition controls. Use property forms before a node graph.

Waveform analysis and imports are cold asynchronous operations with caps and
cancellation, tagged with project/document revision. Add an opt-in cold metadata
and peak-analysis API to Audio if needed; do not include Audio private headers in
the editor or add a second decoder with different channel/loop rules. Cache coarse
peaks, not entire decoded music merely to draw a waveform. Waveform generation
must yield or run outside the GUI and production render callback.

The preview owns a separate AudioSystem session, isolated from a launched game's
voices. A dedicated preview control owner handles preparation and all public Audio
calls. UI sends bounded typed commands/results; the device callback only renders.
During implementation audit, choose an existing fallible worker/thread mechanism
and prove startup failure and shutdown; never use a throwing std::thread constructor.
No synchronous whole-clip preparation or blocking join on the GUI thread. Keep the
UI responsive through close/cancel; do not free state until owner/renderer quiesce.

Preview uses draft settings, never silently saves them. If a replacement draft
fails preparation, retain the last valid preview revision and identify it visibly.
Before launching a game, stop preview output to avoid accidental double playback.
No live game connection is required: initially save/rebuild/run to hear changes in
context. General IPC and embedded play are separate milestones.

## World and session integration

A game-owned AudioPresentation adapter consumes the proposed persistent presentation
outbox once per request, independently of image acquisition or render success.
Resolve authored keys before warm playback. Requests contain numeric resource
references, unique world/request identities, copied positions, owner identity and
seed. Audio callbacks never inspect ECS/components or retain entity pointers.

The adapter observes accepted and rejected admission separately. A queue/capacity
failure must not create a phantom voice, consume cooldown, or mark a loop active.
Optional dropped sounds have explicit policy/counters; required failures surface.
Do not retry a one-shot indefinitely or duplicate it on catch-up ticks.

Generation-aware entity owner identities must not be truncated into the existing
32-bit OwnerTag. Use a bounded mapping to unique tags for the audio session and
reclaim only after the adapter has no retained state, or revise that adapter with
focused compatibility tests. Exhaustion reports a status; no unsafe tag reuse.
Entity destruction stops owned loops. Detached one-shots can finish at their last
copied pose. Persistent emitters publish changed positions at the presentation
boundary; listener selection and units are game-owned policy.

GameSession owns session music. A world owns entity ambience and level leases.
Level replacement tears down old-world loops and rejects stale requests. A music
ID unchanged across levels continues; a changed ID prepares its candidate before
stopping the previous cue. Reserve two stream instances for that handoff. Failed
candidate loading leaves previous music playing. V1 switches with stop/start;
seamless crossfade requires the later scheduling milestone.

Initial pause policy: stop owned gameplay loops, suppress gameplay one-shots,
continue session music and UI sounds; resume recreates needed loops from current
state. Single stepping has muted gameplay audio by default. Restart clears
outboxes/owner mappings and recreates world audio; session music continues unless
explicitly requested to restart. Browser suspension resets game clock debt and
follows backend suspend/resume; do not promise sample position continuity without
backend evidence. User bus gains remain separate from context-owned attenuation.

## Dynamic music extension

Dynamic definitions use a separate version 2 schema; v1 readers reject them.
The installed SDK sample's [two-section fixture](../../examples/audio-content/generate_transition_fixture.py)
generates a catalog, original stereo PCM stems and this canonical score:

```json
{
  "version": 2, "id": "music/journey", "bus": "music", "priority": 6, "gain": 1,
  "tempo": {"numerator": 120, "denominator": 1}, "meter": {"beats": 4, "unit": 4},
  "initial_state": "exploring",
  "states": [{"id": "exploring", "section": "explore"}, {"id": "arrived", "section": "arrive"}],
  "sections": [
    {"id": "explore", "loop_begin_frame": 96000, "loop_end_frame": 192000,
     "layers": [{"source": "source/explore", "gain_curve": [[0, 1], [1, 1]]},
                {"source": "source/explore", "gain_curve": [[0, 0], [1, 0.5]]}]},
    {"id": "arrive", "loop_begin_frame": 96000, "loop_end_frame": 192000,
     "layers": [{"source": "source/arrive", "gain_curve": [[0, 1], [1, 1]]},
                {"source": "source/arrive", "gain_curve": [[0, 0], [1, 0.5]]}]}
  ],
  "transitions": [{"from": "explore", "to": "arrive", "quantize": "beat", "fade_frames": 0}],
  "layer_ramp_frames": 240, "max_stingers": 1,
  "stingers": [{"id": "accent", "sound": "sound/accent", "quantize": "beat"}]
}
```

Every field is required. Identifiers use catalog ID syntax. Each section, state and
stinger name is unique within its own table. A score admits 1–16 sections,
1–32 states, up to 64 unique directed transitions between distinct sections and
up to eight stinger definitions. An initial state and each endpoint must resolve.
Each layer references an audio source. Its 2–8 intensity/gain pairs lie in [0,1]
with strictly increasing intensity from zero to one. Interpolate gain linearly.
Each directed edge counts both prefilled and outgoing stems against the four-stream cap.
The acquisition worker pins the files and validates matching source lengths and
rates within each section. The half-open loop interval uses source frames and begins
after any initial one-time material. Stingers use objects such as
`{"id":"win","sound":"sound/win","quantize":"bar"}`. The simultaneous stinger limit
is 1–8 when definitions exist and zero for an empty table.

Tempo counts quarter notes per minute. Both rational terms are integers in
[1,65535]. Meter counts 1–32 beats with a power-of-two note denominator through
32. Beat and bar boundaries round up to the next representable session frame,
using absolute beat indices relative to the cue origin. Priority uses the Audio
range and cue gain is linear in [0,1]. Fade and layer-ramp durations are unsigned
32-bit session frame counts.

A streamed hard cut seals when the renderer begins the 128-frame control
quantum containing the requested frame. Before sealing, the control owner may
replace the request or cancel it with a zero frame. After sealing, an edit
returns `TooLate` and the accepted transition plays once. The incoming voice
reports the applied frame. Scheduling on a stale snapshot can produce a later
applied frame. Inspect the renderer acknowledgement to distinguish these cases.

The first implementation supports vertical layering and transitions between
constant-tempo/meter sections of the same cue. Tempo changes, polymeter, intro/outro
and effect-tail regions, randomized playlists and transition bridge clips are later
schema extensions. At most four active/prepared stream instances remain available
under the existing plan: count outgoing, incoming, stingers if streamed, and
prefilled candidates. Reject definitions whose maximum simultaneous requirement
exceeds configuration. Do not silently raise the limit or drop a required stem.

MusicDirector owns state selection and bounded future plans on the control side.
The renderer owns an authoritative music transport position and executes prepared
sample-timed commands. Use an integer frame origin and checked rational conversion
for beat/bar boundaries; compute from absolute beat indices to avoid repeated
rounding drift. Game tick time does not drive musical position. Snapshot age,
command lookahead and device output latency are distinct. Do not claim exact
speaker-time synchronization from a render-frame counter.

Extend Audio with atomic group admission, scheduled stream starts and gain/stop
ramps before claiming dynamic transitions. All required starts and automation are
accepted together or nothing changes. Keep plans beyond the current two-second
clip horizon on the director side and publish within the supported horizon.
Late state changes defer to the next eligible beat/bar; report the defer, never
start one stem late while the others run. Coalesce state/intensity edits by explicit
sequence, reject stale generations, and bound pending plans. Unready incoming
sections keep the old section and retry at a later eligible boundary.

For synchronized groups, hold every stem and the musical transport when any member
starves, fade the whole group to silence, then resume together after refill. Device
RenderFrame continues. This intentionally delays musical progress; expose it in
metrics. Failed group members terminate the cue with an explicit error. Independent
stream hold-cursor behavior remains suitable for simple BGM but cannot certify stem
synchronization. Prepared silence/muted stems still consume their aligned timeline.

Author layer changes on beat/bar boundaries with sample-timed ramps. State section
changes use validated compatible boundaries and bounded overlap. Stingers start
on the next requested musical boundary and have a defined overlap limit. Resident
fixtures may prove scheduling/DSP first; real streaming stress must pass before
calling the feature complete.

## Save games and diagnostics

Saving audio documents saves initial behavior, not current playback. First save-game
support records music ID/revision compatibility, section/state and intensity and
restarts at a section boundary after load. Exact mid-track resume is deferred until
seek, transport and decoder state reconstruction are specified. Never serialize
voice/stream handles, worker jobs or pointers. Bus user settings are a separate
preferences document owned by the game.

Show resource/revision, loading stage, selected variation/seed, resolved routing,
admission/suppression cause, active voices, peak memory, stream runway/starvation,
and music section/beat/pending transition. Bound traces and emit formatted diagnostics
on owners only. Reproduce DSP with offline fixtures, but record actual device and
browser listening/capture separately.
