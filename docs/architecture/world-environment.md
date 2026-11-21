# World environment architecture

**Status.** Proposed. Build one World in which regional climate sets the
weather schedule, wind carries clouds, a passing shower produces rain under
the cloud layer, and exposed surfaces dry after the shower. The same inputs
drive Editor preview, Play and a packaged game. The first delivery uses
authored regional schedules and bounded fields within a ground-based World.

## Existing facilities

| Capability | Ludus today |
| --- | --- |
| Sky and celestial lighting | The [clear-sky Atmosphere](renderer-systems.md#clear-sky-atmosphere-design) implements Earth scattering, sky lighting and aerial perspective on admitted profiles. [Celestial drivers](game-world.md#earth-celestial-transforms) sample the Sun and Moon from an explicit epoch and observer. [#1606](https://github.com/The-Ludus-Project/Ludus/issues/1606) owns the playable day–night clock and scrub controls. The [orbital limb tests](renderer-systems.md#celestial-background-and-orbital-views) cover grazing sunlight and planet shadow from space. |
| Render graph integration | The renderer reports [World feature support](../development/render-graph-authoring.md#world-features) from the selected graph. Raster clear-sky support and RT1 sky support have different transport contracts. |
| Clouds | [#1319](https://github.com/The-Ludus-Project/Ludus/issues/1319) tracks a bounded cloud transport prototype and its integration with aerial perspective. |
| Wind | [PhysicsFluid](fluid-field.md) owns numerical grid operations. [Sandbox](drift-game-integration.md) owns its wind policy. [Foliage #1697](https://github.com/The-Ludus-Project/Ludus/issues/1697) needs a reusable World provider and owns plant response. |
| Climate and precipitation | [Terrain fields](../modules/content/terrain.md) supply elevation-derived wetness and drainage for generation. [GameplayWeather](../modules/gameplay/weather.md) cooks regional climates and resolves deterministic weather, including precipitation rate, at World positions. Exposure-dependent surface wetness is proposed here. |

## Ownership and data flow

| Owner | Product |
| --- | --- |
| Content and cooker | Versioned climate regions, weather presets and schedules, cloud profiles and optional maps. Validate units, extents, resource references, bounds and compatible profile choices. Cook headlessly through the [resource model](content-resources.md#ownership-and-dependency-direction). |
| GameplayWorld and GameSession | Own environment region bindings, active transitions, authoritative tick timing, seed, overrides and weather events. Expose bounded queries at explicit World positions and ticks. Keep live state separate from the authored World. |
| World extraction | Resolve one immutable environmental input for each accepted World sample. Include exact profile revisions, region data, time and wind phase, and the existing celestial and Atmosphere references. Share the sample across all views and consumers for that presentation. |
| GraphicsRenderer and RHI | Resolve light and medium transport, prepare graph resources and histories, and retain exact resource leases through GPU completion. Cloud noise, particle detail and reprojection have presentation lifetimes. |
| Gameplay, materials, audio and FX | Sample weather and wind through their respective tick or presentation input. Each consumer defines its response. For example, foliage owns bend and culling bounds, and materials own wet appearance. |
| Editor | Author and inspect through World document transactions. Preview through the normal renderer, and run the same cooked configuration and World schedule in Play and packaged SDK fixtures. |

The environment is an optional World binding. Older Worlds preserve their
authored sky and lighting, with zero precipitation and additional fog and
calm wind. The existing Atmosphere component remains the physical air and
celestial lighting owner. Prepare profile changes as an atomic candidate with
existing World and resource leases. Failed or stale work preserves the last
drawable generation. Distinguish resource generation from tick identity so
continuous weather can advance with resident data and asynchronous texture
preparation can still finish.

## Authored climate and resolved weather

**Climate** describes persistent regional conditions. A climate asset binds
an ordered weather schedule, seasonal schedule selection and optional
long-term temperature and moisture fields. Start with a default climate
and bounded XZ regions with feather distances in world metres. Each region
has a stable ID and priority. Resolve its local schedule at the same tick
as other regions. For each region in descending priority, multiply its
feathered coverage by the weight still available. Subtract its contribution
from the remaining weight. Stable ID breaks priority ties. The default region
takes the remainder, so the weights sum to one. Use an authored
World coordinate domain, with checked grid addresses and an explicit outer
boundary. Sample regions at the receiver or medium position, so crossing an
interior threshold cannot erase fog or change weather outside its windows.

**Weather** controls short-term conditions. A preset supplies the following
values. A schedule specifies preset references, hold and blend durations in
ticks, and a declared looping boundary. Begin with deterministic authored
sequences. A future procedural scheduler can use
[addressed randomness](randomness.md) by World seed, region ID, calendar
period and decision ordinal. Rendering and query order do not consume
authoritative draws.

| Channel | Contract |
| --- | --- |
| Temperature and relative humidity | Degrees Celsius and fraction [0, 1] at the defined reference height. An optional profile supplies the altitude response. These are independent weather fields. |
| Ground and cloud-layer wind | Vectors in m/s toward the direction air moves. Altitude interpolation is defined by the wind provider. Component-space interpolation handles calm and opposing winds without a direction singularity. |
| Cloud coverage and density | Dimensionless coverage and type/shape weights per layer, plus a density multiplier. The renderer's cloud profile sets extinction and vertical support. |
| Fog | Excess extinction in inverse metres at the height reference plus scale height in metres. Optional bounded fog regions contribute local medium data. |
| Precipitation | Nonnegative liquid-water equivalent rate in mm/h. Begin with rain. Extend to mixed types using separately blended rates and declared temperature rules. |
| Surface state | A bounded accumulated wetness fraction, independently advanced from instantaneous precipitation. Terrain cook wetness retains its long-term terrain meaning. |

Validate finite values, ranges, sorted times, capacity and dependency closure
before publication. Interpolate bounded scalars and vectors. Interpolate numeric
cloud shape weights within an admitted layer profile. Profile or texture
changes require an explicit asset transition and checked peak residency. Use stable,
quantized channels for any gameplay threshold or authoritative event address
that has a cross-target determinism claim. Visual interpolation may use
floating point and declared tolerances.

Resolve schedule, regional weights and then game overrides. An override is an
explicit tick command with a stable ID, priority, footprint, affected channels
and blend interval. Apply higher-priority contributions first for each
channel, with the running schedule filling residual coverage. On interruption,
carry the resolved channel values at the transition boundary into the new
blend. Within a region this is per participating source before position-based
weighting, so the starting value does not depend on who triggered the command.
On expiry, start a return blend toward the running underlying schedule.
Define tie breaks by stable ID and re-evaluate all participating regional
weights at the sample position. Keep scheduled weather changes, override
decisions and threshold events in fixed-tick order. Authoritative events are
emitted once per accepted transition or threshold crossing, with stable
addresses for replay and deduplication.

## Clocks, positions and wind

Reuse the session's fixed ticks as the sole time authority for weather,
wind accumulation and gameplay. Pause freezes progression and Step advances
one tick. The same sample records the corresponding celestial instant. The
existing Earth date, time and location feed Sun and Moon. A visual sky scrub
can override the preview celestial instant without emitting weather events,
moving the player or changing stored state. A separate environment preview
scrub evaluates an authored schedule or preview checkpoint without writing
the save or replay. Reset returns to the session's authored baseline. Advancing
a compressed day changes the mapping to celestial time. Wind speed in m/s
integrates over simulation seconds.

Save tick, seed, exact climate and schedule identities, active transitions and
overrides, and any gameplay-authoritative accumulation or wind displacement
that cannot be derived from those inputs. Restore the same next tick and event
ordinal. Reject incompatible revisions with a named migration or reset path.
A future multiplayer host replicates validated environment state or ordered
commands and time anchors using the existing networking contract. GPUs
consume read-only visual samples. Frame rate, shadow quality and render
history cannot affect simulation outcomes.

Use the World convention +X east, +Y up and −Z north for the Earth anchor.
Atmosphere retains planet-relative radial height and binary64 intersections.
A weather region declares a local frame and altitude reference. Cloud layers
start as finite horizontal slabs in that frame, with explicit XZ bounds,
base and top in metres, and a smooth footprint fade. A curved shell or
orbital clouds require their own geometry and horizon acceptance. Evaluate
field addresses from stable World coordinates and use camera-relative
positions for rendering. Preserve phase across origin rebases, motion and
long sessions.

**Shared wind.** Start with a bounded analytic provider. Interpolate authored
mean velocity between ground and cloud heights, add coherent gusts with
world-anchored phase, and add directional or radial sources within authored
smooth falloff volumes. Bound overlaps and evaluate additive sources in stable
ID order, with zero contribution at an undefined radial direction at the
centre. Cap resultant speed after summation. Expose a gameplay tick query and
an immutable presentation representation. Verify both agree at matched samples
within their contract. Calm is exactly zero. Cloud advection integrates the
mean layer velocity sampled at its authored World anchor through fixed
ticks or an equivalent integral of authored curves. Use that displacement
for the whole first layer. Regional coverage can vary spatially while the
detail translates coherently. Carry displacement through transitions and
loop boundaries, using stable coordinate wrapping for tileable fields. A changing speed preserves
cloud position at the transition instant. Ground gusts and local sources can
drive foliage and particles without dragging the entire cloud map.

Qualify a spatially simulated wind provider only if named consumers need
obstacles, wakes or transport that analytic sources cannot represent. Begin
by comparing against PhysicsFluid where its grid semantics match. Treat
simulation ownership, resolution, memory and boundary behavior as independent
decisions. A view-centred GPU grid may supplement cosmetic winds within a
declared valid region. Authoritative queries retain their CPU provider or
receive a separately qualified simulation source.

## Atmospheric transport and renderer integration

Retain the shipped clear-sky path as the zero-cloud, zero-fog control.
Represent added media by extinction in inverse metres, scattering albedo,
normalized phase functions in inverse steradians and a dimensionless density.
Define the forward angle using the existing Atmosphere convention. For a
segment carry scene-linear premultiplied radiance `L` and RGB transmittance
`T`. Compose adjacent segments front-to-back as
`(L_front + T_front * L_back, T_front * T_back)`. Radiance after the farthest
segment includes the attenuated opaque or celestial boundary. A ray clips at
the first opaque depth, ground or atmosphere exit as applicable. A homogeneous
step of length `d`, extinction `sigma` and source radiance per metre `j`
uses `T = exp(-sigma * d)` and `L = j * (1 - T) / sigma` componentwise,
with the `j * d` limit at zero extinction. A vacuum step contributes
`(0, 1)`. Apply each medium's optical depth once.

- **Controlled cloud.** Use [#1319](https://github.com/The-Ludus-Project/Ludus/issues/1319)
  for the initial homogeneous layer, its nonuniform and phase controls, and
  ground, horizon and sunset tests. Reuse Atmosphere Sun/planet visibility.
  With overlapping media, accumulate extinction from all participants and
  source terms under the common view transmittance. A coarse cloud depth
  plus an aerial-perspective lookup is an approximation to compare against
  that shared integral. Applying a second full camera-to-background haze
  over an already composited cloud would count air twice.
- **Authored cloud volumes.** Resolve data from a world-space 2D coverage/type
  map, a vertical shape envelope and bounded tileable 3D detail. Start with
  one cloud layer and an explicit empty map. Keep coverage controls distinct
  from optical density. Evaluate the same density rule for camera rays,
  directional light rays and shadow projections. Use positive optical
  depth, exponential transmittance and a bounded normalized phase. Begin
  with a full-step, non-temporal comparison path. Add conservative empty
  skipping, fewer steps or lower resolution after measuring error and cost.
- **Lighting.** Resolve a bounded transmittance map along the admitted
  directional Sun, anchored and snapped in the light plane with a valid
  receiver footprint and altitude range. Receivers outside it use explicit
  unit transmittance or a qualified fallback. Apply cloud, geometry and
  atmospheric visibility once to the matched direct light. Visibility of
  the solar disk follows the view ray. Changes to diffuse sky and specular
  environment require a separate measured, low-frequency update from the
  same sky/cloud radiance and a coherent publication policy. Avoid hiding
  missing cloud effects by reducing the authored light globally. The
  first cloud profile declares its approximation for clear-air incident
  scattering under clouds.
- **Fog.** Begin with excess height fog and a bounded world-space local
  region. Accumulate overlapping air, cloud and fog coefficients under the
  shared transport rule. An analytic or bounded march establishes the
  reference image for raster qualification. Promote to a frustum volume
  for local lights only if measurements justify sampling, memory and temporal
  cost. Resolve shelter at positions in the scene. A camera-only fog toggle
  can remove exterior fog through a window.
- **Graph and history.** Add a renderer-owned scene-linear path with declared
  depth, color, lighting and volume accesses. Publish before exposure and
  output transforms. Reuse the graph's World-feature reporting for unsupported
  or approximate profiles. If a temporal path is qualified, own history per
  view. Reproject with camera change and cloud displacement, reject
  disocclusion, and invalidate on cuts, resize, incompatible profiles,
  scrub or unhandled origin changes. Pending assets and path disagreement
  surface through the same viewport diagnostic. Clear-sky fallback retains
  the authored environment and reports missing clouds or fog.

The initial media profile targets the default native clustered deferred graph.
Qualify a reduced WebGPU profile against the same analytic controls. WebGL 2
can ship the same weather state with an explicit clear-sky/ordinary background
fallback and bounded rain or wind consumers as supported. Report each tested
profile. Transparency, secondary views and reflections require explicit
transport and cost coverage before claiming support. Any reference transport
extension must provide matched frozen inputs and the same declared integral
under the [paired CPU/GPU contract](renderer-systems.md#cpu-and-gpu-reference-implementation-requirement).
Use raw scene-linear comparisons before temporal or display filtering.

## Rain, wetness and authoring

Sample a bounded world-space shelter mask at receivers. The initial fixture
has one authored overhang or volume and exposed terrain. The renderer emits
a bounded set of depth-tested rain particles near each view. Wind supplies
drift and the precipitation rate supplies particle intensity through a
calibrated visual mapping. Use the regional weather footprint to place
cloud coverage and precipitation consistently. Gameplay rain intensity is
resolved from authored inputs at the tick, independent of rendered cloud
density or view history. Rain exposure is a position-based input available
to gameplay and wetness without sampling the view's particles. Camera motion,
resize or particle budget changes leave outdoor precipitation unchanged.

Start with bounded regional accumulation and material-specific responses.
For example, given fractional wetness `w`, integrate exposure-scaled rain
gain against `(1 - w)` and drydown against `w` at a fixed simulation tick.
Define the gain per unit rain rate, drying in inverse seconds and the
interior/shelter policy before admitting the preset. Preserve a distinct
nonzero drying interval after rain ends. If gameplay needs persistent local
wetness, integrate exposure per addressed cell or actor in a separately
budgeted store. Particles, splashes and decals are presentation. Surface
friction, water accumulation, lightning damage, snow and audio behavior
can consume dedicated gameplay events or queries when a named application
requires them.

Expose an **Environment** World tool with climate and preset editors, a
region overlay, an explicit preview time and a selected position inspector.
The inspector shows the contributing regions, transition and override,
precipitation rate, wind vector, fog/cloud capability and the active
rendered generation. Keep sky scrub and weather preview independently
inspectable. Commit accepted edits through World undo. Report invalid
references, unsupported profiles and exceeded budgets in the acting panel
with repair guidance. Keep working defaults on a missing optional feature.
Exercise with Properties hidden, project save/reopen, Play and an installed
SDK fixture mounting the same cooked files.

## Delivery and qualification

Use a fixed camera/tick route through a two-region valley with a foreground
ridge, an overhang and window, directional Sun, a horizon view and a
clear → overcast → rain → clear sequence. Include a wind reversal, a failed
preset replacement, a rebase and an interrupted transition. Record revision
and tick with each sample. Compare analytic extinction, premultiplied
scene-linear radiance and intended direct shadow. Qualify clear baseline,
inside/above/below layer, grazing angles, occluders before and within media,
independent views and pause/scrub. Assess near-zero density in absolute units
and bright samples in relative units with profile-specific tolerances declared
before qualification.

As initial budgets, target 0.25 ms p95 CPU weather/extraction and 2.5 ms p95
incremental GPU environment work (cloud, fog, direct cloud lighting) in a
single 1080p view at 60 Hz on a named native reference GPU. These are proposed
qualification targets. Record CPU/GPU per-pass distributions, history and
peak resource bytes, step counts and rejection rates, worst-case horizon
coverage, cold preparation and steady-state cost. Hold output profile, camera
and quality fixed for comparisons. Run deterministic headless tests on
native and browser builds. Software GPU results qualify behavior. Measure
physical WebGPU hardware before making browser performance claims.

| Slice | Delivery gate |
| --- | --- |
| [Shared climate and weather](https://github.com/The-Ludus-Project/Ludus/issues/1704) | One default region and one feathered override. Compare per-tick golden samples/events across chunked stepping, pause, replay, save/load, iteration-order changes, and distinct view locations. Qualify Editor preview and SDK packaged play. The [runtime](../modules/gameplay/weather.md#determinism-evidence) and its [pause, step, preview and reset semantics](../modules/gameplay/weather.md#pause-step-preview-and-reset) are in place; Editor authoring, Play wiring and SDK play remain. |
| [Analytic World wind](https://github.com/The-Ludus-Project/Ludus/issues/1705) | Match sampled speed and direction at ground and cloud heights, calm, bounded falloff, and uninterrupted phase after clock restore and rebasing. Qualify two consumers using a cloud tracer marker and a ground receiver. |
| Cloud prototype | Reuse [#1319](https://github.com/The-Ludus-Project/Ludus/issues/1319) and its analytic and scene-linear controls. |
| [Weather-driven clouds](https://github.com/The-Ludus-Project/Ludus/issues/1706) | One authored coverage/type map, stable advection and accepted snapshots tied to World revisions. Verify clipping, horizon, day/night, clear recovery and profile fallback. |
| [Rain and surface response](https://github.com/The-Ludus-Project/Ludus/issues/1707) | Verify shelter, indoor/outdoor views, onset and drying independently of particle budget. Run the same route through Editor and packaged SDK play. |
| [Cloud direct lighting](https://github.com/The-Ludus-Project/Ludus/issues/1708) | Compare a moving footprint with independent light-ray transmittance. Verify low Sun, map coverage, exposure independence and no duplicate direct attenuation. |
| [Local fog](https://github.com/The-Ludus-Project/Ludus/issues/1709) | Check air/fog/cloud overlap against a segment integral and matched exterior rays viewed through a window. Measure the bounded march before scaling to a froxel profile. |
| [Conditional spatial wind](https://github.com/The-Ludus-Project/Ludus/issues/1710) | Compare an analytic provider and a bounded field on a named wake/obstacle scenario. Preserve useful fallback and report quality, stability, memory and total frame cost. |

## Reference decisions

The local `references/game-dev-gems-toc.md` surfaced the talks on regional
weather, cloud density authoring, coupled lighting and procedural winds.

- Thanks to **Emily Zhou and Colin Weick**, *Simulating Tropical Weather in
  Far Cry 6*, GDC 2022, slides 15–29 and 32–34. Reuse authored regional
  schedules, gameplay overrides and distinct accumulated wetness. Their
  indoor/window example motivates position-based weather and shelter queries
  instead of globally fading effects when the camera moves indoors.
  [Talk slides](https://media.gdcvault.com/GDC%2B2022/Speaker%2BSlides/SimulatingTropicalWeather_Weick_Colin_Zhou_Emily.pdf)
- Thanks to **Andrew Schneider and Nathan Vos**, *Nubis: Authoring Real-Time
  Volumetric Cloudscapes with the Decima Engine*, SIGGRAPH 2017, density and
  authoring sections. Separate horizontal coverage/type, vertical shape and
  procedural detail. The first Ludus producer uses an authored map and
  bounded advection. Simulated cloud maps need a dedicated content case.
  [Course](https://www.advances.realtimerendering.com/s2017/index.html)
- Thanks to **Sébastien Hillaire**, *Physically Based Sky, Atmosphere and
  Cloud Rendering in Frostbite*, SIGGRAPH 2016, §§5.6.3 and 5.9. Adapt
  finite-segment composition. Qualify a cloud-depth aerial-perspective
  approximation against controlled overlap before adopting it.
  [Course notes](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf)
- Thanks to **Fabian Bauer**, *Creating the Atmospheric World of Red Dead
  Redemption 2*, SIGGRAPH 2019, slides 14–20, 33–35, 42–49 and 69–71.
  The map/height split, cloud transmittance projection and near/far
  volume distinction motivate explicit pass products and coverage tests.
  Keep history and frustum volumes conditional on a measured budget.
  [Course](https://advances.realtimerendering.com/s2019/)
- Thanks to **Rupert Renard**, *Wind Simulation in God of War*, GDC 2019,
  the motors and advection sections. Separate bounded analytic wind sources
  from field resolution and receivers. Start with the direct analytic
  provider. Evaluate a grid when a named obstruction or wake justifies it.
  [Talk slides](https://media.gdcvault.com/gdc2019/presentations/Renard_Rupert_Wind_Simulation.pdf)
