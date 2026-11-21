# Regional climate and deterministic weather

Three Foundation-only modules implement the climate and weather slice of the
[World environment architecture](../../architecture/world-environment.md#authored-climate-and-resolved-weather)
([#1704](https://github.com/The-Ludus-Project/Ludus/issues/1704)). They build
for native and browser targets and are installed in the SDK.

| Target | Headers | Role |
| --- | --- | --- |
| `Ludus::WeatherFormat` | `ludus/content/weather_format/weather_format.h` | LUWTH cooked climate contract: units, limits, record layouts, codecs and the shared validation rules. Header-only; owned by neither producer nor consumer. |
| `Ludus::ContentWeather` | `ludus/content/weather/climate_cook.h` | Headless cooker. Validates an authored climate in natural units and writes one canonical blob. |
| `Ludus::GameplayWeather` | `ludus/gameplay/weather/climate.h`, `weather_simulation.h` | Loads a blob, resolves weather at World positions on fixed ticks, applies gameplay overrides, emits addressed events and saves its state. |

## Author and cook a climate

Describe the climate with `ClimateSource`:

- **Presets** (`WeatherPresetSource`) give every channel in natural units, with
  a stable nonzero ID and a revision. Change the revision whenever the values
  change.
- **Schedules** (`WeatherScheduleSource`) list entries that hold a preset for
  `HoldTicks`, then blend linearly toward the next entry over `BlendTicks`.
  Entries before `LoopStartEntry` play once from tick zero. The rest then
  repeat, so the last entry blends into the loop entry.
- **Regions** (`WeatherRegionSource`) are World XZ rectangles inside the
  climate domain. Each has a schedule, a priority and an inward feather in
  metres. The **default schedule** drives the default region: everywhere
  outside or between regions.
- The **domain** is the climate's explicit outer boundary. **TicksPerSecond**
  is the session tick rate that the durations assume.

`TryCookClimate` validates the source and quantizes every value to an integer
code. Values round to the nearest code, with ties toward +infinity. It writes
presets and schedules in ascending ID order, and regions in resolution order
(descending priority, then ascending ID). Reordering authored records
therefore produces identical bytes. On failure, `ClimateCookResult` names the
first failing record (`Subject`, `SubjectId` and `Detail`, the entry index or
channel) and leaves the output unchanged. Correct the named record. For
example, a `ValueOutOfRange` preset names the channel whose value lies
outside the range below.

| Channel | Code unit | Admitted range |
| --- | --- | --- |
| Temperature | millidegree Celsius | −100 to +100 °C |
| Relative humidity, cloud coverage, cloud type | 1/65536 | 0 to 1 |
| Ground and cloud-layer wind (X, Y, Z) | mm/s, toward the direction the air moves | ±150 m/s per component |
| Cloud density scale | 1/65536 | 0 to 8 |
| Fog excess extinction | 10⁻⁶ m⁻¹ | 0 to 1 m⁻¹ |
| Fog scale height | mm | 0 to 20 km |
| Precipitation (liquid water equivalent) | µm/h | 0 to 500 mm/h |

A climate holds at most 32 presets, 16 schedules, 64 schedule entries and
8 regions besides the default region. Tick rates range from 1 to 1000 per
second. `Ludus::WeatherFormat` owns these limits. The World frame is +X east,
+Y up and −Z north. Coordinates are millimetre codes within ±10,000 km.

## Run the simulation

```cpp
Climate climate;
if (TryLoadClimate({bytes.GetData(), bytes.GetSize()}, climate) != WeatherFormatStatus::Success) { /* report */ }
WeatherSimulation weather;
if (WeatherSimulation::TryMount(climate, sessionTicksPerSecond, worldSeed, weather) !=
    WeatherSimulationStatus::Success) { /* report */ }

StaticArray<WeatherEvent, MAX_WEATHER_EVENTS_PER_TICK> events{};
usize eventCount = 0;
(void)weather.TryAdvance(1, {events.GetData(), events.GetSize()}, eventCount); // once per fixed tick
WeatherSample sample;
(void)weather.TrySample(positionXWorld, positionZWorld, sample);
```

`TryLoadClimate` applies the same `ValidateClimateRecords` rules as the cooker.
It rejects truncation, trailing bytes, an unknown version and non-canonical
order. Mounting fails with `TickRateMismatch` unless the session runs at the
climate's tick rate. The session's fixed ticks are the only clock: pausing
means not advancing, and stepping means advancing one tick.
`ResetToAuthoredBaseline` returns to tick zero.

`TrySample` resolves the current tick at a World position, not at the camera.
Two cameras, or a camera and a gameplay query, sampling the same position get
the same answer, and sampling never changes the state. Resolution proceeds in
this order:

1. **Regions.** In resolution order, each region claims its feathered
   coverage of the weight still available. Coverage is zero on and outside the
   edge and rises with a smoothstep to one at the feather depth. The default
   region takes the remainder, so `RegionWeights` sum to exactly 65536.
2. **Schedules.** Each weighted schedule contributes its held or blended preset
   at the tick.
3. **Overrides.** Active overrides, highest priority first with ties by
   ascending ID, claim channel by channel their footprint coverage times their
   current weight of what higher overrides left. The running schedule fills the
   residual.

`ConvertWeatherChannels` converts codes to natural units for presentation.
Gameplay thresholds should compare codes, which are identical on every target.

## Override the weather from gameplay

Submit a `WeatherOverrideCommand` for a future tick with `TrySubmit`:

- **Begin** starts an override. It blends in from zero weight toward a preset
  over `BlendInTicks`, then holds. After `HoldTicks` it **expires** and blends
  back to the running schedule over `BlendOutTicks`. `HoldTicks = 0` holds
  until a **Release**, which starts the same return blend. The override
  replaces only its `ChannelGroups`, either everywhere (`IsGlobal`) or inside
  a feathered footprint.
- **A Begin for an active ID interrupts it.** The new blend starts from the
  override's current weight and channel values, before position weighting.
  Weather is therefore continuous at every position, whoever triggered the
  change. An interruption keeps the priority, groups and footprint; use a new
  ID for a different footprint.

Commands apply in (tick, override ID, action) order, so the order of
submission within a tick never matters. `TrySubmit` rejects commands in the
past, unknown presets, invalid groups or footprints, duplicates and a full
queue. A conflict that depends on the state at the apply tick becomes an
`OverrideRejected` event: releasing an inactive override, a full active set or
an interruption that changes the footprint.

## Events and saves

`TryAdvance` emits schedule entry and blend starts per region and the override
lifecycle (`Started`, `Interrupted`, `Released`, `Expired`, `Ended` and
`Rejected`). Phase changes come first, then commands, then schedule
boundaries: the default region, then regions by ascending ID. `(Tick, Ordinal)`
addresses each event. The ordinal counts every event in the session and is
saved, so replays and deduplication see the same addresses. The tick at mount
(zero) emits no events. If `outEvents` is too small, nothing changes;
`MAX_WEATHER_EVENTS_PER_TICK` entries per advanced tick always suffice.

`TrySaveState` writes the tick, seed, event ordinal, active overrides and
pending commands with the climate identity. That identity is its ID, revision,
tick rate and the 128-bit fingerprint of the exact cooked blob, which covers
every preset revision. `TryLoadState` restores the same next tick and ordinal.
It returns `IncompatibleClimate` for a save from another climate or preset
revision, and leaves the state unchanged. Repair it by calling
`ResetToAuthoredBaseline` or by loading a save that matches the mounted
climate. Malformed saves fail with `InvalidState`.

## Author a World climate

A built-in World stores its climate in the optional root member `climate`. A
World without it has no weather, and older Worlds keep their bytes. The Editor
validates the member with `TryCookClimate` on load and on every edit, so a
saved World always cooks for Play. It reports the first failing preset, schedule
entry or region by name, with its repair. Durations are in Play's fixed ticks
(`PLAYABLE_WORLD_TICKS_PER_SECOND`, 60 per second).

```json
"climate": {
  "version": 1, "id": 1, "revision": 3,
  "domain": { "minimumX": -2000, "minimumZ": -2000, "maximumX": 2000, "maximumZ": 2000 },
  "defaultSchedule": 10,
  "presets": [{ "id": 1, "revision": 2, "name": "Clear", "temperature": 22.5, "humidity": 0.35,
                "groundWind": [2, 0, -1], "cloudWind": [4, 0.25, 2], "cloudCoverage": 0.1,
                "cloudType": 0.9, "cloudDensity": 0.6, "fogExtinction": 0.00035,
                "fogScaleHeight": 160, "precipitation": 0 }],
  "schedules": [{ "id": 10, "name": "Lowlands", "loopStart": 0,
                  "entries": [{ "preset": 1, "hold": 2400, "blend": 600 }] }],
  "regions": [{ "id": 7, "name": "Valley", "priority": 1, "schedule": 10,
                "bounds": { "minimumX": -800, "minimumZ": -500, "maximumX": 600, "maximumZ": 700 },
                "feather": 200 }]
}
```

The members and their units follow the cooker's source fields. Names are Editor
labels and are not cooked. `WorldDocument::SetClimate` replaces or removes the
member in one undoable **Edit Climate** step and maintains the revisions: an
unchanged commit adds no history, the climate revision rises with each change,
and each preset's revision rises when its values change.

**Play.** Preparing a World for Play cooks its climate into the `.luplay`
manifest (version 2): `climate` is `null`, or the LUWTH `bytes` as lowercase hex
with their `sha256`. `DecodePlayableSnapshot` checks the digest and admits the
bytes with `TryLoadClimate`. The first-person player then mounts them at Play's
tick rate and reports the weather at the spawn. Advancing the weather with the
session's ticks arrives with the Play session clock of
[#1606](https://github.com/The-Ludus-Project/Ludus/issues/1606).

**Installed SDK.** `Ludus::WeatherFormat`, `Ludus::ContentWeather` and
`Ludus::GameplayWeather` are SDK components. The installed consumer
(`tests/sdk_consumer/weather.cpp`) runs the golden corpus against them. An
Editor test cooks the corpus climate spelled as a World member and checks that
it produces the corpus bytes. The installed SDK therefore mounts the same
cooked input that Editor Play packages.

## Pause, step, preview and reset

The session's fixed ticks are the weather clock, so the session's controls
drive it without a separate weather state:

| Control | Call | Effect |
| --- | --- | --- |
| **Pause** | Stop calling `TryAdvance` | Frames keep rendering. Any number of views can sample the paused tick in any order, and nothing changes. |
| **Step** | `TryAdvance(1, ...)` | Advances exactly one tick, with that tick's events and the next ordinals. |
| **Preview** (scrub) | `TryPreview` | Evaluates another tick without changing the simulation, its saved state or its event ordinals, and emits no events. |
| **Reset** | `ResetToAuthoredBaseline` | Returns to tick zero, with no overrides or pending commands and ordinal zero. Replaying the same commands reproduces the same event addresses. |

`TryPreview` has two modes:

- **Authored** evaluates the climate alone at any tick, including earlier
  ticks: schedules and regions, without overrides or pending commands.
- **Projected** carries the live state forward to a tick at or after the
  current one. It applies pending commands and override phase changes, so it
  shows what the session will see then if no further commands arrive. Only
  commands and phase ends change the state, so the projection jumps between
  them. Its cost does not depend on how far ahead the preview is. A projected
  tick before the current one returns `PreviewInPast`; use an authored preview
  instead.

A weather preview is separate from a sky scrub, which overrides only the
celestial instant ([#1606](https://github.com/The-Ludus-Project/Ludus/issues/1606)).
Each stays independently inspectable. Camera position never enters the
resolution: views inside and outside a region boundary each sample their own
position, and samples at the same position agree whichever view asks.

## Determinism evidence

All authoritative arithmetic is integer fixed point. The only floating-point
steps are quantizing authored values and sample positions, done as separate
binary64 operations that no target can fuse. The golden corpus
(`tests/sdk_consumer/weather_golden_corpus.cpp`) runs a
200-tick route. Its climate has a feathered valley region overlapping a
higher-priority ridge region, plus an interrupted local storm, a pending release,
a rejected release, a temperature-only override and a rejected interruption.
The corpus checks that cooking the climate in reverse authoring order
gives identical bytes. It then compares these runs:

- single ticks and mixed chunks of 5, 15 and 5 ticks;
- a save and load at tick 50, while the storm blends in with commands pending;
- reordered authoring and submission order.

Samples are taken at six positions, including the region feather, the overlap
and a domain corner. Each sample comes with a projected preview 37 ticks ahead
and an authored preview at half the current tick. All runs must produce identical events, samples and final
state, and the digest must equal the one recorded from the native reference
build. `ludus_gameplay_weather_tests` checks it natively. The browser presets
build `tools/web-weather-probe`, and `./scripts/test web-emscripten-development`
runs the same corpus under Node as `ludus_web_weather_corpus`.

## Not yet covered

The runtime, its session control semantics, the World climate member, Play
packaging and the installed SDK components are in place. Still to come under
[#1704](https://github.com/The-Ludus-Project/Ludus/issues/1704): the
**Environment** World tool, for editing presets, schedules and regions with a
region overlay, a preview time and a position inspector; and driving the weather
from the Play session's controls, which the built-in player gains with the
session clock of [#1606](https://github.com/The-Ludus-Project/Ludus/issues/1606). Seasonal schedule selection, altitude profiles, accumulated surface
wetness ([#1707](https://github.com/The-Ludus-Project/Ludus/issues/1707)) and
the shared wind provider
([#1705](https://github.com/The-Ludus-Project/Ludus/issues/1705)) are separate
slices.
