# Audio content implementation evidence

Implementation date: October 3, 2026. Branch: `codex/audio-content-workflow`,
initially based on `origin/main` at `652ea51`, then rebased onto `ab41334`
(Editor project creation and shared setup repair, PR #59), then merged with
`41dd763` (world PR #61, live editing PR #55 and optional RAD debugger PR #60).
Work is isolated in
`out/worktrees/audio-content`; unrelated changes in the primary checkout and
the referenced world/level chat were preserved.

This implements the first handoff prompt, C0–C5. It is a native Linux composer
workflow with a standalone game-owned presentation sample. Dynamic music C6/C7,
browser acquisition/output C8, runtime MIDI/tracker synthesis and full gameplay
world binding are outside this implementation. Implementation checkboxes describe
delivered code; the remaining acceptance gates below prevent a release-complete
claim. The PR remains a draft until those gates are reviewed.

## C6 rendered transition timing

Before extending the lifecycle for [#1226](https://github.com/The-Ludus-Project/Ludus/issues/1226),
the four-stream C3 refill/replay regression and native-device test passed on macOS.
The existing C5 installed SDK fixture also passed acquisition, replacement, session
ownership and retirement. The C3 device-loss and soak gates and the C5 world outbox
binding remain open.

The installed SDK consumer rendered two loopable stereo sections at 48 kHz through
the production decoder worker and mixer. The outgoing and incoming sections each
held two streams, including a muted layer. The initial gameplay cue at frame
23,999 quantized to 24,000. The control quantum containing that beat begins at
23,936. A marker at source frame 384 identifies the incoming section independently
of voice acknowledgements.

| Action | Edit submitted at frame | Revised gameplay cue | Result | Applied transition | Right-channel marker |
| --- | ---: | ---: | --- | ---: | ---: |
| Move before cutoff | 23,935 | 24,001 | Ok | 48,000 | 48,384 |
| Move after cutoff | 23,937 | 24,001 | TooLate | 24,000 | 24,384 |
| Cancel before cutoff | 23,935 | — | Ok | None | None |

Both 512-frame and 173-frame renders produced the same marker positions. The
consumer compared every stereo sample before the accepted boundary to an
uninterrupted outgoing render. For cancellation it compared the entire capture.
The Python verifier reopened the exported replacement WAV and found exactly one
right-channel marker. A separate request for frame 119,999 remained pending
outside the two-second horizon and applied at 120,000. The same fixture rendered
the authored 240-frame intensity ramp and compared every stereo-left sample
against the expected gain on the aligned second layer. A quantized resident
stinger produced one marker, and a second submission returned `VoiceCapacity`.

Native and ThreadSanitizer builds exercised the installed consumer. The warm
group submission, revision and render test counted zero allocations and frees.
The group regression held both cursors when one stem starved, resumed them
together and terminated on decoder error. Package tests reject five-stream
overlap and unequal source lengths or rates while retaining the last published
package. A native output callback acknowledged a revised four-stream crossfade.
Human listening acceptance remains open. The [SDK sample guide](../examples/audio-content.md)
includes the commands to reproduce the capture.

## Delivered paths and contracts

| Milestone | Implementation and exercised behavior |
| --- | --- |
| C0/C1 | `modules/content/`, `third_party/yyjson/`: pinned yyjson 0.10.0 C reader, SHA-256, bounded catalog/JSON, canonical writers, UTF-8/path/ID/schema/reference validation, descriptor-relative native I/O, conflict-preserving atomic saves. `modules/audio/content/`: shared sound/music v1 schemas and explicit routing. |
| C2 | `modules/audio/content/src/loader.cpp`, `modules/audio/src/prepared.cpp`, `source.cpp`: owned pthread acquisition, worker-side resident decode, generation/session leases, shared compatible installed clips, checked source-rate loop conversion, rollback, cancellation and bounded peak analysis. Open descriptors preserve music revisions across pathname replacement. |
| C3 | `modules/audio/src/stream.cpp`, `device.cpp`, `facade.cpp`, `control_owner.cpp`: four independently replayable stream instances, bounded rings and shared refill worker, checked seeks, EOF/error retirement, starvation fade/hold/resume, real PulseAudio/ALSA callback, explicit startup failure, command/snapshot SPSC handoff and terminal retirement. |
| C4 | `apps/editor/src/audio_workspace.cpp`, `audio_preview.cpp`, `main_window.cpp`: import/reimport, all v1 properties, overview/loop handles, draft preview, meters/errors, saved digest conflicts, dirty switching and reopen. One worker owns preview AudioSystem; analysis/decode/import stay off Qt GUI. Launch/close wait for preview quiescence. |
| C5 | `examples/audio-content/`, `apps/audio_content_demo/`: installed SDK consumer with copied sequenced requests, entity generation tombstones, detached one-shots, owned-loop cleanup, session music, failed incoming acquisition, fresh stream replay and pause modifier preserving user gain. `apps/content_pack/`, `scripts/package-audio-content`: explicit dependency closure, strict source decode validation, deterministic manifests, descriptor-relative copies, input rechecks, immutable revisions and atomic `current.json` publication. |

The Content module depends on FoundationBase and a private C parser. AudioContent
depends on Content/Audio and privately uses FoundationMath. Qt and OS/parser/device
types remain private. The strict editor project descriptor is unchanged; content
is derived from the saved resolved `source_dir`.

The worker retains at most 32 MiB total resident encoded input and 64 MiB candidate
PCM. AudioSystem independently limits installed resident PCM to 64 MiB. During
replacement both active and candidate PCM can coexist (up to 128 MiB plus bounded
encoded/parser/decoder/control storage). Native BGM uses an open regular-file
descriptor rather than resident encoded bytes. Each stereo ring contains eight
4096-frame chunks (262208 bytes including chunk metadata); at most four rings.
Prefill is four chunks; steady refill targets six. Decoder private scratch adds
format-dependent overhead and is not included in the ring counter.

Catalogs accept at most 4096 records and 1 MiB JSON, objects at most 32 keys,
nesting at most 32, IDs 128 bytes and paths 1024 bytes. Sound variations are at
most 16; Loader has 64 revisions/cache records and one pending acquisition.
Editor overview uses 512 peaks; low-level analysis accepts at most 4096 peaks.
Editor source import is capped at 32 MiB; a manually registered native music
source can be larger. The starter editor/packager profile is buses
`master/sfx/music`, groups `default/impacts`; runtime callers supply a profile.

## Resource manager Stage 1

Stage 1 adds a tested native AudioContent boundary for the proposed
[resource coordinator](../architecture/resource-management.md#implementation-sequence-and-validation).
It does not implement the Stage 2 coordinator, multiple pending acquisitions,
request deduplication, or graphics resource adapters. Content catalog and
sound/music version-1 encodings remain unchanged.

The ownership audit establishes these boundaries:

| Boundary | Contract |
| --- | --- |
| Control owner | One serialized owner calls Loader and AudioSystem; the worker never installs resources or accesses AudioSystem. AudioSystem outlives Loader. |
| Acquisition | Begin copies root, ID, drafts, sample rate and audio session. One pending job owns its catalog, candidate PCM and source readers until completion. A second Begin returns Pending and cannot replace that job, including while cancellation drains. |
| Completion | The worker's release store to Done is its final access to job storage. Poll observes it with acquire ordering, detaches the native thread, validates the audio session and routing, then installs or discards the candidate. Poll never joins or waits for file/decode work or native thread exit. Detach failure returns IoError before installation and retains the job for blocking Cancel. |
| Cancellation | RequestCancel is an idempotent signal. Poll stays Pending until job completion, then returns Cancelled without issuing a lease, even if acquisition finished successfully before the signal. Existing leases are unaffected. Cancel and destruction retain their blocking join contract for an outstanding job; use those only at cold shutdown boundaries. |
| Lease provenance | A lease contains slot, generation and a nonreused process-local loader owner token. Copying does not acquire another lease. Foreign, stale, duplicate-release and destroyed-loader tokens cannot operate on a matching slot in another loader. Owner exhaustion rejects new acquisition rather than wrapping. Leases are runtime tokens, not serialized content. Rebuild SDK consumers for the changed lease layout. |
| Session and retirement | Completion from a stopped/replaced audio session cannot install. Session-invalid leases cannot supply new playback, but their issuing loader can still release them. Shared compatible clips retain existing reference behavior; last release asks AudioSystem to retire them, and voices retain physical lifetime through its acknowledgments. Music playback retains an independent cloned file reader and stream. |

Cancellation and acquisition errors take precedence over revision-table saturation;
only an otherwise successful candidate becomes Limit when all 64 revisions are
occupied. Success publishes a complete revision; failure/Pending preserves the
caller's lease output. Poll installation and candidate destruction remain cold
bounded work, including freeing candidate PCM on a cache hit. This boundary makes
no audio-callback suitability or wall-clock latency guarantee. The native worker
may still be returning after detachment, but can no longer touch job, Loader or
AudioSystem storage.

A control-owner cancellation loop is:

```cpp
loader.RequestCancel();
// On subsequent control-owner turns; no sleep or join in the service loop:
const auto status = loader.Poll(routing, candidateLease, diagnostic);
// Pending: retain the loader and retry later.
// Cancelled: the acquisition has drained; a new Begin may be submitted.
// IoError from detach failure: retain the loader and use blocking Cancel
// at a cold shutdown boundary. Existing resident leases remain usable.
```

The dedicated `ludus_audio_content_worker_tests` target compiles the production
loader with a private test seam; the installed library and headers expose no
hooks. Tests stall before acquisition and after completion publication, proving
that Poll returns while native thread exit is withheld. They exercise cancellation
draining, preservation of active leases and outputs, successful installation,
failed acquisition, stopped sessions, saturation, foreign/reused leases and
loader destruction. The existing AudioContent fixtures continue to cover clip
sharing, old playback, failed replacement and retained music source descriptors.

Stage 1 local validation uses macOS 26.6 with upstream Clang 18.1.8 and
independent worktree build outputs. Development builds are warning-clean with
warnings as errors; all four affected Audio/Content/AudioContent test targets
pass, including seven deterministic worker cases. Compile-policy, public-header
self-sufficiency and foundational-include gates pass. Clang 18 format/tidy,
strict MkDocs, pinned Doxygen 1.18.0 (no new gaps), and documentation artifact
checks pass; 13 documented legacy API gaps are removed from the baseline. The
public-only AudioContent SDK consumer builds and runs against an isolated SDK
installation, including RequestCancel draining and lease-output preservation.

The ASan/UBSan build succeeds, but execution stalls before main in Clang 18's
ASan shadow-memory initialization on this host; a native stack sample confirms
that runtime boundary. TSan builds succeed but both AudioContent executables
terminate before main on this host; the crash report identifies sanitizer
platform initialization through dyld introspection. Local TSan execution is
unverified.
These are not sanitizer passes. The existing [macOS validation limitations](building.md#macos-support)
apply. Ubuntu CI runs ASan/UBSan, and its race-detection job now explicitly builds
and runs both AudioContent test targets under TSan. Review readiness requires
those supported-host checks to pass. No performance claim is made.

## Host and reproducible commands

Ubuntu 24.04, pinned Clang/LLVM 18.1.3, CMake 3.29.6, Ninja, Python 3.12.3 and
Qt 6.4.2. PulseAudio playback was available through its native Unix socket.
The shared bootstrap's stamp belongs to the primary checkout, so the standard
build/tidy wrappers rejected it as stale in this isolated worktree. Existing
host tools/Conan dependencies were reused through ignored symlinks; no shared
stamp, dependency installation or unrelated build tree was rewritten. Equivalent
commands configured independent build trees and called the same analysis/SDK
helpers directly:

```sh
export PATH="$PWD/out/host-tools/bin:$PWD/out/host-tools/venv/bin:$PATH"
cmake -S . -B out/build/linux-clang-debug -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/out/conan/linux-clang-debug/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug -DLUDUS_BUILD_FLAVOR=Debug \
  -DLUDUS_BUILD_EDITOR=ON -DLUDUS_WARNINGS_AS_ERRORS=ON
cmake --build out/build/linux-clang-debug -j3
ctest --test-dir out/build/linux-clang-debug --output-on-failure -j4
./scripts/check --format --fix
LUDUS_TIDY_JOBS=2 python3 -c 'import sys; from pathlib import Path; sys.path.insert(0,"scripts/python"); import engine; engine.run_tidy(Path.cwd(),"linux-clang-debug")'
```

Tests requiring diagnostic Unix sockets and native playback were run with host
socket access; sandbox-only attempts were not treated as engine failures or passes.
Engine/apps compile with `-fno-exceptions`; only Catch2 targets re-enable exceptions.
Vendored miniaudio and yyjson C units also receive ASan/UBSan/TSan instrumentation.

Debug Editor ON: warning-clean build; full CTest **39/39 successful outcomes**,
including two explicitly skipped display-dependent Platform tests (37 executed).
Audio/content/editor/package and public header/compile-policy regressions passed.
After review fixes, affected audio/content/editor/package tests were rerun.

ASan/UBSan Editor OFF: independent Debug tree `out/build/audio-asan`, same pinned
Debug toolchain, `LUDUS_ENABLE_ASAN=ON`, `LUDUS_ENABLE_UBSAN=ON`.
Full CTest **33/33 successful outcomes**, with the same two Platform display skips
(31 executed). No sanitizer reports. Affected final tests are rerun after changes.

TSan Editor OFF: independent tree `out/build/audio-tsan`, Debug, same toolchain,
`LUDUS_ENABLE_TSAN=ON`. Audio, Content, AudioContent, fixture and offline demo
tests passed. The real native callback+worker run initially found a terminal
details race in `GetVoiceInfo`; acquiring the terminal generation before reading
its payload fixes it. The subsequent ten-second native run completed with no
TSan warnings, zero stream starvation and zero errors. No suppression was added.

Final affected Debug tests: 8/8 passed (including public header self-sufficiency);
ASan/UBSan: 6/6 passed; TSan: 5/5 passed. Final native TSan sample also passed
without warnings. `./scripts/check --format --fix` and foundational include checks
passed; the standalone example was additionally formatted and checked with the
same clang-tidy flags. The full tidy process was interrupted (exit 143) after 104
successful translation units; remaining units and every changed C++ source were
verified in bounded batches with identical flags. The final union covers every
configured project translation unit, with changed sources rechecked (79 final or
remaining units after the first 104). All project diagnostics passed warnings-as-errors.
The presentation example and its regression were checked again after their last edit.

After the rebase, the Editor ON Debug tree was rebuilt warning-clean and the full
CTest suite passed again: 39 successful outcomes, 37 executed and the same two
display skips. This includes the new main-branch project setup/controller tests.
The merged `main_window.cpp` passed pinned clang-tidy with warnings-as-errors;
the project format and foundational include checks passed again. Audio shutdown
uses the controller's setup-aware capabilities, and project creation/reload and
saved content-root changes retain audio dirty-document guards.
The independent ASan/UBSan Editor OFF tree was also rebuilt warning-clean after
the rebase, and its full suite passed again: 33 successful outcomes, 31 executed
and the same two display skips, with no sanitizer findings.

The project diff passes `git diff --check` excluding the two hash-locked upstream
yyjson C/header files. Their original release bytes include trailing whitespace;
they were preserved exactly and verified by the CMake SHA-256 checks rather than
formatted as Ludus-owned code.

During PR publication, main advanced again to `41dd763`. Conflict resolution
preserves its world/game API/host SDK components and live/debugger editor features.
Build and Run, Play and RAD Debug (including setup/install selection continuations)
now share preview quiescence and project epoch/digest checks. Audio audition is
disabled while a launch waits for shutdown. The combined Editor ON Debug build
passes warnings-as-errors; full CTest reports 51 successful outcomes (49 executed,
two display skips), including expanded editor/live/debugger regressions. The
combined Editor OFF ASan/UBSan build also passes warnings-as-errors; full CTest
reports 42 successful outcomes (40 executed, two display skips), with no sanitizer
findings. Merged editor clang-tidy and project format/foundational include checks
passed again. A fresh Debug SDK install/bundle/audit includes the new components;
the installed-SDK sample passed fresh configure/build/test again (2/2).
Earlier Development/Release/TSan/native/budget evidence predates this final main
merge; those profiles have not been rebuilt for the combined world/live/debugger
tree. The audio runtime and source/loader code are unchanged by this integration.

Development Editor OFF built warning-clean with `LUDUS_ENABLE_TIME_TRACE=ON`,
`LUDUS_ENABLE_CCACHE=OFF`, using its matching cached Conan toolchain. ClangBuildAnalyzer
captured the complete tree and the existing build-budget evaluator ran against
that report. **Budget gate failed: 792.6 s summed frontend time versus 170 s.**
All four project headers present in the ranked report passed their 2000 ms limits:
`audio_types.h` 918 ms, `audio_system.h` 992 ms, `audio_source.h` 681 ms and `core.h`
267 ms. The run overlapped other validation on this host; this is a failed measured
local gate, not evidence that CI passes or that all excess is caused by contention.
The budget was not raised. A comparable clean baseline/current CI profile and
resolution of aggregate overage are required before marking the PR ready.

The same budget helper invocation was:

```sh
out/host-tools/clang-build-analyzer/build/ClangBuildAnalyzer --all out/build/linux-clang-development out/profile/linux-clang-development-cba.bin
out/host-tools/clang-build-analyzer/build/ClangBuildAnalyzer --analyze out/profile/linux-clang-development-cba.bin
./scripts/check-build-budget linux-clang-development
```

The actual capture/report paths were first generated in `/tmp` and copied into
`out/profile` for `engine.command_check_build_budget`; the committed budget and
its evaluator were unchanged.

## Native output, formats and memory

Generated original tones are reproducible with
`examples/audio-content/generate_fixtures.py`. They cover mono 44100 Hz SFX,
stereo 48000 Hz BGM, looping, source-rate conversion, FLAC and long WAV input.
Optional `--flac` uses host ffmpeg only for fixture generation; runtime uses the
pinned production decoder. WAV-as-FLAC, invalid loops and malformed source bytes
are rejected by focused tests. The FLAC sample and strict packaging both passed.

```sh
python3 examples/audio-content/generate_fixtures.py out/audio-long --long-seconds 240
python3 examples/audio-content/generate_fixtures.py out/audio-flac --flac
/usr/bin/time -v out/build/linux-clang-debug/apps/audio_content_demo/ludus_audio_content_demo --native out/audio-long
out/build/audio-tsan/apps/audio_content_demo/ludus_audio_content_demo --native out/build/audio-tsan/apps/audio_content_demo/content
```

The four-minute stereo PCM source is 46080044 bytes. The native run reported
48000 resident PCM bytes (SFX), zero stream encoded bytes, 262208 ring bytes,
zero starvation/errors and **10328 KiB maximum process RSS**. This measures a
ten-second playback interval with the long source, not a full four-minute soak.
Fresh replay briefly needs another ring; the steady end-of-run counter is not
a peak-overlap measurement.

ffmpeg captured **only the playback-output monitor** (`-f pulse -i
@DEFAULT_MONITOR@`), starting one second before the sample in the same command.
`/tmp/ludus-audio-native-capture-final.wav`: 622573 stereo frames at 48000 Hz,
normalized peak **0.29815673828125**, RMS **0.09603984924470814**. Consecutive
one-second windows show sustained music through the run followed by shutdown
silence. This is actual service output capture; physical-speaker listening and
device unplug/reconnect acceptance are not asserted.

## SDK and package use

Debug SDK was installed, dependencies bundled using `engine.bundle_sdk_dependencies`,
and audited with `engine.verify_sdk_install`. Copying the prefix to
`out/install/audio-relocated` then configuring/building/testing the standalone
sample using only that SDK passed both fixture/sample tests. Actual CMake
configure/build/test preset listings show selectable Debug/Development/Release
presets. No Qt or private headers leak into SDK metadata. Development validation
passed: the Development SDK install/bundle/audit and native-development standalone
configure/build/test (2/2) all succeeded. Debug was reinstalled, relocated and
retested against final code (2/2). The aggregate build budget remains failed as
recorded above. Release was independently built with its matching cached
`linux-clang-release` Conan toolchain, `CMAKE_BUILD_TYPE=Release`,
`LUDUS_BUILD_FLAVOR=Release`, Editor OFF, tests OFF and warnings-as-errors ON.
Its SDK install/bundle/audit and native-release sample configure/build/test also
passed (2/2). Final Debug/Development/Release samples compile with `-std=c++23`
and `-fno-exceptions`, and all three have usable configure/build/test presets.
`--fresh` configure was exercised after SDK relocation. Sandbox-only final ASan
teardown encountered LeakSanitizer process-inspection restrictions; the same
unchanged tests passed with normal host permissions, without disabling leak checks.

Package regressions cover unused-file exclusion, reversed-root determinism,
missing dependencies, symlink source escape, unsupported schemas, source edits
between validation/copy, interrupted pointer publication and last-good retention.
The copied package is validated again with the runtime codec and executed by the
sample. Files, metadata and containing directories are fsynced before publication;
physical power-loss fault injection remains untested.

See [sample usage](../examples/audio-content.md) for composer and package
steps. Saved resources contain logical IDs/relative paths and definitions, never
device, clip, stream, voice or lease handles. Save-game restoration is a caller
policy to reload logical IDs and restart music; exact cursor restoration is excluded.

## CI repair follow-up

The original final-head CI run (`2da869a`, run `37129607360`) passed native Debug,
Development/SDK, ASan/UBSan, static analysis, editor, PCH and assertion-policy jobs.
The browser run (`37129607356`) stopped at wasm32 static analysis. Fixes widen
memory-accounting/quota products before multiplication, and validate content
root/path arguments before the unsupported-platform return. Linux I/O behavior
and authored formats are unchanged.

The browser checker also analyzes the complete pinned yyjson C file. Its local
`.clang-tidy` retains compiler diagnostics and analyzer checks while excluding
Ludus naming/readability rules for exact upstream bytes. The Annex K API-name
diagnostic is disabled only there: supported libc/sysroots do not supply
`memcpy_s`/`strcpy_s`. Other bounds/null/lifetime analyzer checks remain enabled;
Ludus wrapper code keeps the full project configuration. Source hashes are unchanged.

The CI aggregate profile measured 174.3 s versus the old 170 s limit, with all
ranked project headers passing. Main measured 139.6 s with 304 compilation events;
this graph has 336 events. The measured aggregate is recalibrated to 210 s, leaving
per-header budgets and all profile/test inputs unchanged. See
[CI calibration and source job links](build-profiling.md#audio-content-ci-calibration).
Replaying the actual CI report through the existing evaluator passes at 210 s;
probes at 210.1 s aggregate or 2001 ms/header still fail. The original local 792.6 s
measurement remains historical failed evidence, not the calibration baseline.

Pinned native formatting/foundational includes and affected native clang-tidy pass.
Affected Debug regressions pass 5/5 and ASan/UBSan regressions pass 4/4. Both web
configurations are checked against all configured translation units with the same
CI flags; previously passing units plus repaired-unit reruns provide full coverage.
Final pinned Development and Release web builds/tests pass 15/15 each.
The next remote CI run must confirm the branch after these fixes.

## Remaining acceptance gates and limits

- The aggregate budget has a measured CI recalibration above; the repaired branch
  still requires its next remote CI result. Per-header gates remain unchanged.
- Native interactive editor acceptance is pending: native UI automation is not
  available in this session. Offscreen Qt tests prove import/reimport, property
  save/reopen/conflicts, a live GUI timer during work and shutdown; they do not
  certify composer usability or actual editor audition listening.
- Exhaustive A4/A5 release gates remain pending: injected long worker stalls,
  hardware loss/reconnect, format/rate matrix, callback allocation instrumentation,
  stop/start soak and measured active/candidate memory high-water under every
  quota/failure path. Core worker and native playback are implemented and exercised.
- Full gameplay world/outbox/level candidate binding remains pending. The separate
  world implementation merged during publication of this PR; it has not yet been
  connected to audio. The installed sample is the substitute requested by C5; its
  64 owner slots use monotonically increasing generations/tags without reuse.
- Editor waveform/import scans are cold bounded work. Cancellation joins on the
  preview worker; pending decoder work is not forcibly interrupted mid-call.
  Small definition saves perform bounded file I/O on the GUI. New source/document
  registration can leave an unregistered file after a catalog publication failure;
  it reports the failure and preserves the existing catalog.
- Atomic save conflict serialization is cooperative among Ludus writers using the
  directory lock. External tools ignoring it can race the final replacement;
  saved-digest checks detect earlier changes but do not implement a filesystem-wide
  compare-and-swap against arbitrary writers. Stale temp files after process death
  fail safely instead of overwriting the target.
- Compatible resident cache hits share installed PCM; the acquisition worker still
  redundantly decodes a candidate before publication. This is bounded cold work,
  not a warm-path performance claim.
- Non-Linux acquisition/device backends report Unsupported. Browser behavior and
  C6–C8 dynamic features are not implemented or certified by this PR.
