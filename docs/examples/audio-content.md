# Audio content sample

This is a standalone game-owned consumer of `Ludus::AudioContent` and
`Ludus::Resources`. It loads
stable sound/music IDs, demonstrates copied outbox requests with entity
generations, and plays resident SFX with file-backed looping BGM. The source
checkout, Qt editor and DAW are not runtime dependencies.

The Stage 2 sample composes a private sound adapter with the resource coordinator.
Game and preview scopes request the same sound; cancelling preview leaves the game
request successful. Game code retains a move-only exact-version lease while using
the sound descriptor. Music remains an independent AudioContent playback instance.
See the [implemented Stage 2 profile](../architecture/resource-management.md#implemented-stage-2-profile)
for fixed capacities, peak reservation accounting, owner lifetime and the adapter's
single pending/resident sound limitation. Use an immutable packaged content root
throughout a session. Shutdown must drain the coordinator before its adapter goes
away. Cold process cleanup joins any outstanding acquisition through the legacy
Loader boundary; frame-loop request cancellation and manager shutdown remain
polling-based. This sample's adapter exclusively owns resident clips in its AudioSystem.

## Build with an installed SDK

Use the pinned CMake 3.29+, Ninja and Clang 23. Set `LUDUS_SDK_DEBUG` to the
installed Debug SDK directory. From the Ludus checkout root, enter the sample:

```sh
cd examples/audio-content
cmake --list-presets=configure
cmake --list-presets=build
cmake --list-presets=test
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug
out/debug/audio_content_sample --native out/debug/content
```

The Development and Release presets use `LUDUS_SDK_DEVELOPMENT` and
`LUDUS_SDK_RELEASE`, respectively. Match the SDK's build flavor to the consumer.
No shader tools are required by this sample. For IDE use, select the same CMake
executable and enable preset mode; keep host SDK/tool paths in local settings.
The full SDK's system prerequisites still apply during CMake package discovery.
After moving/changing an SDK or toolchain, configure with `cmake --fresh --preset
native-debug` (or the corresponding profile) to clear stale package/tool caches.

`--offline` runs a deterministic production-renderer smoke test without a device.
`--native` runs for approximately ten seconds and requires a working Linux
PulseAudio or ALSA playback service. It reports startup failure explicitly.

## Composer workflow

1. Export mono/stereo WAV or FLAC from REAPER or another DAW. Keep the DAW project
   separately. MIDI is rendered to audio for this first workflow.
2. Open a game project in the optional Ludus editor. Its **Audio content** dock
   uses `<resolved source_dir>/content/catalog.json`.
3. Choose **Import audio**, select the export and give it an ID such as
   `source/impact`. Reusing that ID replaces its registered source revision
   while preserving the ID/path. Editor imports are limited to 32 MiB.
4. Choose **New sound** or **New music**, give the definition an ID, and enter
   registered source IDs. Sound variations are comma separated (maximum 16).
   The starter editor profile uses buses `master`, `sfx`, `music` and groups
   `default`, `impacts`. Games supply their own explicit routing profile.
5. Set gain, priority and looping. Sound also exposes rate, spatial defaults,
   cooldown and active suppression. Loop points are source frames; left/right
   mouse dragging sets beginning/end in the waveform overview. For variations,
   a shared loop requires matching source rates and valid bounds in every file.
6. **Play draft** auditions without saving. A failed candidate keeps the previous
   admitted preview; the message identifies the failure. **Stop** stops preview.
7. **Save audio** writes canonical JSON with a saved-digest conflict check.
   Close/reopen or **Reload** reads those same runtime definitions. Dirty switching
   offers save/discard/cancel. Reload unsupported versions without rewriting them.

Native preview is lazy and uses its own worker/control owner. Game launch waits
for preview shutdown. Interactive GUI acceptance is still pending; see
[implementation evidence](../development/audio-content-evidence.md).

## Package explicit roots

After building the engine's native tools, run from its root:

```sh
scripts/package-audio-content \
  --validator out/build/linux-clang-debug/apps/content_pack/ludus_content_validate \
  --content examples/audio-content/out/debug/content \
  --output out/audio-release sound/impact music/theme
```

The command prints an immutable revision directory. Pass that directory to the
sample as its content root. `current.json` identifies the last completed revision.
Only the requested sound/music definitions and their source dependencies are
included, with relative paths and SHA-256 digests. Missing/invalid/changing inputs
leave the previous publication usable. Copy the sample executable plus the
revision directory and required SDK/system license notices for delivery.

From `examples/audio-content/`, generated tones can also exercise longer or FLAC input:

```sh
python3 generate_fixtures.py out/long --long-seconds 240
python3 generate_fixtures.py out/flac --flac
```

The optional FLAC fixture generation uses host `ffmpeg`; playback has no ffmpeg
dependency. Dynamic stems/transitions, runtime MIDI/tracker playback and browser
content acquisition are later milestones.
