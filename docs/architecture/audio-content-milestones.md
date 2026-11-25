# Audio content implementation milestones

Status: C0–C6 implementation is available. Release acceptance remains partial. Read
[resource architecture](content-resources.md), [audio authoring](audio-authoring.md)
and [Codex handoff](audio-content-codex-handoff.md). Checked boxes identify delivered implementation; see
[actual validation and pending gates](../development/audio-content-evidence.md).
Existing [A0 through A7 audio tasks](../../.kiro/specs/audio/tasks.md) and
[E0 editor tasks](../../.kiro/specs/editor-workspace/tasks.md) retain their scope and
evidence. This package adds the content workflow rather than replacing them.

## Completion rules and order

An implementation checkbox needs changed paths, exact commands, outcomes and
remaining gates in `docs/development/audio-content-evidence.md`, created by the
implementer. Historical evidence is a starting point, not proof of current success.
No measurements, hardware output, GUI acceptance or browser behavior may be invented.

| Milestone | Deliverable | Prerequisites |
| --- | --- | --- |
| C0 | Current audit, fixtures, codec selection and interface plan | Repository instructions |
| C1 | Shared catalog and persistent sound definitions | C0 |
| C2 | Resource leases, resident preparation and safe replacement | C1 |
| C3 | Complete simple BGM streaming and audible native sample | C0, existing A4/A5; C2 for content sample |
| C4 | Audio editor import, preview, save and reopen | C2, native output; E0 compatibility |
| C5 | Game/session binding and release packaging | C2; C3 for streamed BGM |
| C6 | Dynamic music grammar and synchronized playback | C3, C5 |
| C7 | Dynamic audition UI and complete composer acceptance | C4, C6 |
| C8 | Browser content playback and release acceptance | C5, existing A6; C6 for dynamic parity |

C1/C2 and offline fixtures can proceed while device gates are unavailable. C4
forms/saving can proceed without a device, but audible preview remains incomplete.
C5 uses a standalone sample if the world proposal is not yet implemented. Do not
block all independent work on another chat, and do not edit its files opportunistically.

## C0 Audit and freeze the first implementation

- [x] Read AGENTS.md, steering, all four documents in this package, current audio
  implementation/spec/evidence, editor E0, and world/level/frame proposals.
- [x] Record actual toolchain, current diffs, APIs/stubs and available native/browser
  environments. Preserve unrelated work and verify codec/channel support in code.
- [x] Select a JSON codec and show explicit errors, duplicate-key rejection,
  bounded allocation and no-exception compatibility. Pin dependency/license if new.
- [x] Freeze catalog, sound v1 and music v1 typed structures and writer fixtures.
  Fix contradictions in this package with evidence before coding, not by silently
  choosing incompatible behavior. Dynamic v2 stays for C6.
- [x] Select the fallible acquisition/preview worker mechanism, command ownership,
  native/browser status progression and lifetime proof. Keep parser/Qt/OS private.
- [x] Create generated or clearly licensed mono/stereo WAV/FLAC fixtures, including
  different rates, loop seams, malformed/truncated data and long streaming input.

Acceptance: compile the chosen parser/thread boundary under pinned Clang 18 and
`-fno-exceptions`; validate every complete JSON example; record limits and no
unsupported API claims. Do not add an allocator, generic task graph or codec here.

## C1 Catalog and sound documents

- [x] Implement the bounded Content catalog, IDs, kind/path validation and cold
  lookup. Add AudioContent typed sound/music v1 definitions and dependency validation.
- [x] Resolve routing names against an explicit profile; reject missing buses/groups.
- [x] Implement canonical readers/writers and field-specific diagnostics shared by
  runtime and editor; GUI value objects do not become the source of truth.
- [x] Implement temporary-file replacement, saved-digest conflict checks and
  failure-preserving document state. File operations expose explicit outcomes.

Acceptance fixtures: valid read/write/read semantic equivalence, byte-stable
canonical writes, duplicate keys/IDs, wrong kinds/types, unknown versions/fields,
missing dependencies, invalid UTF-8/paths, traversal/symlink escape, nonfinite
values, bad loops, oversized input, excessive nesting, allocation/write/replace
failures and externally changed files. Reordered catalog entries must preserve
references. Writer rejection must leave old files and dirty drafts intact.

## C2 Preparation and replacement

- [x] Implement owned acquisition, asynchronous loading stages, cancellation tokens,
  revision-specific dependency leases and explicit Ready/Failed states.
- [x] Prepare resident clips and resolve saved sound definitions into existing event
  descriptors. Uniform seeded variations reuse the existing adapter.
- [x] Retain decoded source rate and implement checked source-to-session loop
  conversion in preparation. Reject invalid bounds rather than silently clamping;
  verify direct AudioSystem callers as well as content-loader callers.
- [x] Pin active revisions through admitted playback and preparation jobs; release
  with AudioSystem retirement and Service acknowledgments.
- [x] Add staged reimport with ID preservation and changed-length/rate loop checks;
  account for active plus candidate peak memory and all rollback paths.
- [x] Add opt-in cold metadata/peak analysis without exposing decoder internals.
  Reuse production decoding and bounded cache records.

Acceptance: two clients sharing a compatible clip, different loop descriptors,
loop conversion at 44100/48000, retire during active play, repeated reloads,
failed candidate retaining old playback, late completion after project/session
change, cancellation at each stage, shutdown with pending work, quota failure and
memory high-water counters. Cache hits must be invalidated by changed source,
settings or audio session. Warm submission/render remain allocation-free.

## C3 Simple music and native output

- [ ] Complete existing A4 streaming and A5 native device work through their actual
  tasks, including worker/rings, loop seeks, cancellation and underrun behavior.
  Reuse the current design; do not build another mixer for the content system.
- [x] Remove the current false Device Ready behavior. Backend setup failure is a
  failure; no ready/audible claim or implicit Disabled fallback without caller policy.
- [x] Load music v1 through source leases and independent stream instances; show
  explicit NotReady/Unsupported/failed states and terminal reclamation.
- [x] Add a content-based native demo with SFX, looping music and level-like cue
  replacement. Preserve old music when incoming preparation fails.

Acceptance: all A4/A5 gates, actual audible/captured output, long-track memory
measurement, repeated stream replay using fresh instances, malformed sources,
worker stalls, device loss/shutdown and supported format/rate/channel reporting.
Check cap accounting for outgoing plus incoming cue; no silent resident fallback.
TSan must exercise the real worker/renderer path, not only an offline scheduler.

## C4 Audio editor

- [x] Extend existing Editor state/actions with catalog and audio document ownership;
  preserve build/run/stop and optional Qt/runtime SDK isolation.
- [x] Implement import/register/reimport and forms for all sound/music v1 fields,
  source waveform overview, loop fields/handles, preview Play/Stop, meters/errors.
- [x] Keep analysis/preparation off the GUI and all preview AudioSystem calls on one
  owner. Prove cancellation and shutdown quiescence before freeing buffers.
- [x] Save and reopen drafts with conflict handling. Unsupported versions remain
  untouched. Show which valid revision is being previewed after a failed edit.
- [x] Stop preview before game launch. Do not introduce game IPC or embedded play.

Acceptance: composer imports an external file, authors a variation/loop, auditions,
saves, closes/reopens, and replaces the export without changing its resource ID.
Test dirty switching, missing files, failed disk writes, reentrant widget updates,
project switch during analysis, repeated Play/Stop/close, device failure and a large
waveform request with responsive UI. Native interactive acceptance is required;
offscreen tests alone do not prove usability or audible preview. Verify E0 process
ownership/build failure/argument regressions remain intact.

## C5 Game integration and packaging

- [x] Implement a game-owned presentation adapter using copied request data and
  safe generation-aware owner mapping. Exercise the existing event policy adapter.
- [x] Demonstrate exactly-once delivery, entity-loop teardown, session music across
  levels, restart, pause/single-step policies and user-gain preservation.
- [x] Provide equivalent compiled requests in a standalone installed-SDK sample
  (the world was not implemented at the first handoff baseline).
- [ ] Connect the world outbox and level candidate leases with focused integration.
  The world implementation merged during this PR's publication; the sample remains
  the exercised substitute, and full world binding is pending.
- [x] Implement explicit dependency-root validation and loose-file staging with
  catalog/digest manifest, atomic completion publication and last-good preservation.
- [x] Package a game sample that runs without the source checkout, DAW or editor.

Acceptance: zero/multiple ticks per frame, skipped rendering, duplicate/stale
requests, owner tag exhaustion/reuse, detached one-shot after removal, old-world
loop cleanup, same music across transitions, failed incoming cue, required/optional
admission failures, shared leases and cancellation during level preparation.
Package with unrelated/unused assets, missing references, interrupted staging,
source edits during packaging (reject inconsistent input), deterministic output
manifest and no absolute host paths. An SDK sample proves installed dependency
closure. Mark full world binding pending if only its substitute sample exists.

## C6 Dynamic music

- [x] First freeze complete music v2 grammar, canonical JSON examples, limits and
  typed structures in audio-authoring.md. Include all fields required to reproduce
  state/section mapping, layer curves, rational tempo/meter, quantization, fade
  durations, overlap limits, stingers and capacity validation. V1 stays readable.
- [x] Implement authoritative group transport and atomic scheduled group starts,
  gain ramps and stops. Document command acknowledgement, late/defer policy and
  lookahead relative to the existing scheduling horizon.
- [x] Implement aligned stream consumption and group starvation fade/hold/resume;
  keep independent stream policy intact for ordinary BGM. Protect group capacity.
- [x] Implement bounded MusicDirector state/intensity planning, compatible section
  transitions and resident stingers. Expose transport and pending-plan diagnostics.

Acceptance: sample-frame reference output across render block partitions, many
bars without cumulative rounding drift, identical stereo stem positions, rational
boundary overflow, late commands, atomic capacity/queue rejection, rapid conflicting
state changes, unready incoming section, unequal stem length/rate rejection,
starvation of one stem, recovery/error/cancellation of whole group, muted stem
alignment and lookahead beyond two seconds. Exercise actual streaming with TSan,
allocation checks and real listening; offline clips alone do not certify it.
Definitions requiring more than four stream instances fail before playback.
Do not mark complete by increasing a capacity constant without budget evidence.

See the [rendered C6 timing evidence](../development/audio-content-evidence.md#c6-rendered-transition-timing) for the native results and remaining listening gate.

## C7 Dynamic authoring and composer acceptance

- [ ] Add section/layer/transition property forms, tempo/meter display, state buttons,
  intensity slider, stinger trigger and visible beat/transition diagnostics.
- [ ] Preview the same MusicDirector/runtime as the game with bounded draft reload.
- [ ] Document DAW export alignment, lossless delivery, IDs/reimport, supported
  transitions, loading limits and save-game restart policy.

Acceptance: export aligned stems, import, author exploration/combat and intensity,
audition changes on musical boundaries, save/reopen/package, hear the same behavior
in the demo, reimport a valid revision and diagnose a broken one. Verify representative
composer actions interactively, maximum-stem transition budgets, dirty switching,
missing dependencies, failed candidate preparation and preview teardown.

## C8 Browser and final acceptance

- [ ] Complete existing A6 output/lifecycle gates and adapt Content acquisition to
  bounded async fetch. Package emitted files and required isolation headers.
- [ ] Prove simple SFX/music content playback and, when C6 is in release scope,
  synchronized dynamic behavior through the real browser renderer.
- [ ] Record download/encoded/decoded/peak memory, readiness latency, stalls and
  suspension/gesture behavior. Keep unverified browser/platform claims explicit.
- [ ] Run final applicable SDK, build, analysis, sanitizer and regression checks.

Acceptance: current Chromium and Firefox versions recorded, real captured output,
missing/failed fetch, isolation/worklet failure, user activation, suspension/resume,
close while loading, repeated sessions and main-thread/worker stalls. Mocked fetch,
Node wasm or offline rendering are additional checks, not live browser acceptance.
Follow the existing web audio preset policy; do not impose threading/isolation on
unrelated browser builds.

## Validation commands and evidence

Use the actual current CLI/preset support and the pinned toolchain:

```bash
./scripts/build linux-clang-debug
./scripts/test linux-clang-debug
./scripts/check linux-clang-development --all
./scripts/build linux-clang-asan-ubsan
./scripts/test linux-clang-asan-ubsan
./scripts/install-sdk linux-clang-development
./scripts/check-build-budget --profile
```

Also run warnings-as-errors, public header self-sufficiency/foundational include
checks, Editor ON and an independent OFF build, installed content/audio consumers,
TSan separately from ASan where concurrency changes, and applicable audio/web/E0
regressions. Add failure/lifetime/scheduling tests that verify contracts, not tests
that merely repeat the implementation. Never weaken gates, enable engine exceptions,
raise header budgets without measurements or mark unavailable checks as passing.

Evidence entries identify milestone, revision/paths, commands, outcomes, measured
limits and outstanding gates. Content-only documentation changes validate examples,
links and whitespace; they do not claim the implementation commands above ran.

## Deferred features

Runtime MIDI/synthesizers, tracker modules, lossy encoding, HTTP range streaming,
binary banks, plugin importers, tempo maps, time stretching, arbitrary seek/exact
save-game playback restoration, live game IPC, node graphs, convolution/reverb
and generalized spatial activation are separate designs. Do not add these to
complete the first composer workflow.
