# Benchmark scenes and system test suites

Status: continuation roadmap, October 7, 2026. The scene catalog below identifies
candidate tests; entries become validated only after a reproducible Ludus run
and recorded evidence.

Continue testing Ludus with recognizable reference scenes and controlled system
setups. Use each scene to check observable correctness as well as performance.
Cornell box and Sponza anchor the rendering work; stacks and tumblers, audio
fixtures, navigation maps and simulation cases extend coverage across the engine.
Keep small diagnostic cases alongside larger integrated levels.

The [engineering quality architecture](../architecture/engineering-quality.md#scenario-coverage-and-reproducibility)
owns scenario coverage, failure semantics and reproducibility policy. This guide
owns the scene catalog, priorities and continuation workflow. Existing
[CI behavior](continuous-integration.md) remains the authority for enabled gates.

## Next session

Start with a Cornell box capability review against the current
[public rendering slice](fullscreen-rendering.md). Choose a bounded representation,
define the camera, light, geometry, material assumptions and expected observations,
then implement and run that case on an available declared backend. Record the
representation: an application-owned analytic fullscreen shader tests a different
path from imported meshes. General mesh, texture and depth support require
separate feature work when absent; a familiar image does not prove those paths.

Before choosing an implementation, inspect current code, tests and capability
evidence. Use a task-specific `codex/` branch and separate worktree from freshly
fetched `origin/main`; inspect the primary checkout read-only and preserve its
existing changes. Reuse existing examples and
test infrastructure where appropriate, and keep scene-specific policies in the
application. Follow AGENTS.md and the owning module's contracts for every change.

After the first case, continue through the initial priorities below. If a case
requires an unavailable subsystem, record the concrete prerequisite and select
another admissible case. Keep the deferred scene in the catalog. Each follow-up
should add or improve a runnable case and its evidence rather than repeatedly
rebuilding the catalog.

## Initial priorities

| Priority | Suite and first cases | Admission and purpose |
| --- | --- | --- |
| 1 | Rendering: Cornell box, then Sponza | Establish a controlled lighting case first; admit Sponza when the chosen geometry/material path supports it. |
| 2 | Physics: box stack and tumbler | Require a contact solver and rigid-body lifecycle; measure resting stability and sustained contact churn separately. |
| 3 | Character movement: stairs, slopes and moving platforms | Require the chosen controller and collision queries; define step/slope/platform behavior explicitly. |
| 4 | Audio: overlapping effects, looping and streamed music | Extend the existing audio-content sample; check lifetime, loop seams, streaming progress and callback deadlines before advanced room acoustics. |
| 5 | World lifecycle: bounded spawn/despawn arena | Extend the world reference with entity generation, stale-reference and allocation checks. |
| 6 | Streaming: scripted traversal, reversal and teleportation | Start with supported filesystem reads; distinguish file acquisition from future renderer resource residency. |

Relevant starting points are the [world example](../examples/world-demo.md),
[world validation](game-world-validation.md),
[audio example](../examples/audio-content.md),
[audio evidence](audio-content-evidence.md),
[filesystem measurements](filesystem-benchmark.md), and
[Drift physics design](../architecture/physics-fluid-simulation.md).
Drift's analytic drag/ripple model does not establish a general rigid-body or
fluid solver. Navigation, crowds, animation and specialized simulation enter
the runnable suite as their required capabilities become available.

## Reference scene catalog

The linked resources include asset collections, sample applications, research
scenarios and formal benchmarks. They are references for constructing Ludus
cases, with different assumptions and acceptance criteria. The systems and
measurements below are proposed Ludus coverage, not claims that an external
collection supplies every check.

| Suite | Representative cases | Systems and observations |
| --- | --- | --- |
| Lighting and materials | Cornell box, material spheres, glass interiors | Direct/indirect lighting, shadows, materials, reflections, transparency and output encoding. |
| Rendering scale | Sponza, San Miguel, dense instanced geometry | Visibility, culling, batching, geometry throughput, texture residency and CPU/GPU frame time. |
| Asset import | Khronos assets isolating format features | Attributes, indices, transforms, cameras, materials, textures and supported extensions. |
| Animation | Fox, Animated Morph Cube, repeated animated characters | Skinning, morph weights, interpolation and looping; add explicit blending and event fixtures. |
| Rigid bodies | Stacks, pyramids, tumblers, convex piles, objects on triangle meshes | Broad/narrow phase, friction, restitution, sleeping, islands, penetration and jitter. |
| Continuous collision | Fast projectile against a thin wall | Tunneling across speed, angle, shape and timestep boundaries. |
| Constraints | Pendulums, chains, bridges, ragdolls and motors | Joint limits, constraint error, convergence and mass-ratio sensitivity. |
| Character movement | Stairs, ramps, ledges, ceilings, elevators and rotating platforms | Sweeps, ground detection, wall sliding, traversal boundaries and platform-relative movement. |
| Vehicles | Suspension course, slopes, uneven terrain, braking and turning | Wheel contact, suspension, traction, steering and chassis interaction. |
| Cloth and deformables | Hanging cloth, cloth over a sphere, rods and deformable solids | Stretch/bend constraints, collision, self-intersection, volume error and stability. |
| Fluids | Dam break, emitters and fluid interacting with solids | Neighbor search, pressure solving, boundary handling, density/volume error and coupling. |
| Simulation accuracy | Taylor–Green vortex; free fall, spring and pendulum fixtures | Integration error, numerical dissipation, timestep sensitivity and convergence against an applicable reference. |
| Pathfinding | Moving AI game maps, mazes, rooms and random obstacles | Path validity/quality, expanded nodes, query cost and unreachable goals. |
| Navigation meshes | Multi-level layouts, narrow passages and tiled terrain | Walkability, agent clearance, path queries, tile updates and crowd integration. |
| Crowds | ORCA circle crossing and evacuation; doorway counterflow | Neighbor queries, local avoidance, congestion, oscillation and arrival time. |
| Audio playback | Sound gallery, overlapping impacts, looping and streamed music | Decoding, mixing, routing, voice policy, playback lifetime, underruns and loop seams. |
| Spatial audio | Orbiting mono source, approach/recede, pass-by and connected rooms | Coordinate conventions, attenuation, HRTF, Doppler, occlusion, reflections and reverb where supported. |
| Networked simulation | Fiedler's interacting cubes and coupled physical groups | Replication, interpolation, synchronization, correction error and bandwidth under injected network conditions. |
| UI interaction | Widget gallery, long lists and overlapping windows | Layout, clipping, focus, input routing, text editing and keyboard/controller navigation. |

Reference collections and attribution:

- [PBRT scene distribution](https://www.pbrt.org/scenes-v3) and
  [Benedikt Bitterli's Rendering Resources](https://benedikt-bitterli.me/resources/)
  provide rendering scenes. Preserve each asset's author and license notices.
- [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
  provide examples of asset and animation features, including Fox. The
  [Animated Morph Cube](https://github.com/KhronosGroup/glTF-Sample-Models/blob/main/2.0/AnimatedMorphCube/README.md)
  isolates animated morph weights.
- [Bullet benchmarks](https://github.com/bulletphysics/bullet3/blob/master/examples/ExampleBrowser/ExampleEntries.cpp),
  [Erin Catto's Box2D samples](https://box2d.org/documentation/samples.html) and
  [pyramid/tumbler measurements](https://box2d.org/posts/2023/10/simulation-islands/),
  and [Jorrit Rouwe's Jolt samples](https://github.com/jrouwe/JoltPhysics/tree/master/Samples/Tests)
  provide physics scenarios and diagnostic applications.
- [PositionBasedDynamics](https://github.com/InteractiveComputerGraphics/PositionBasedDynamics)
  and [SPlisHSPlasH](https://github.com/InteractiveComputerGraphics/SPlisHSPlasH),
  by Jan Bender and contributors, provide deformable/constraint and fluid examples.
  Use the [dam-break setup](https://github.com/InteractiveComputerGraphics/SPlisHSPlasH/blob/master/data/Scenes/DamBreakModel.json)
  as one candidate, preserving its numerical assumptions.
- The [Taylor–Green benchmark project](https://benchmark.coria-cfd.fr/index.php/Main_Page)
  distinguishes analytical verification in two dimensions from numerical
  reference comparisons in three dimensions. Select a case matching the solver.
- [Google DeepMind's MuJoCo Menagerie](https://github.com/google-deepmind/mujoco_menagerie)
  supplies articulated models for later robotics/control tests when relevant.
- [Nathan Sturtevant's Moving AI benchmarks](https://www.movingai.com/benchmarks/grids.html)
  supply maps and pathfinding problems; they are not complete playable levels.
  [Recast/Detour](https://github.com/recastnavigation/recastnavigation) provides
  navigation-mesh examples.
- [Optimal Reciprocal Collision Avoidance](https://gamma.cs.unc.edu/ORCA/), by
  Jur van den Berg, Stephen J. Guy, Jamie Snape, Ming C. Lin and Dinesh Manocha,
  provides crowd scenarios. The
  [Reciprocal n-body collision avoidance paper](https://gamma.cs.unc.edu/ORCA/publications/ORCA.pdf)
  includes circle crossing.
- [Audiokinetic's Wwise samples](https://www.audiokinetic.com/en/library/Launcher_2023.2.4.3909/?id=samples&source=InstallGuide)
  provide educational audio projects, including Cube, Wwise Adventure Game and
  Wwise Audio Lab. [OpenAL Soft's HRTF example](https://github.com/kcat/openal-soft/blob/master/examples/alhrtf.c)
  and [Valve's Steam Audio documentation](https://valvesoftware.github.io/steam-audio/doc/unity/index.html)
  inform spatial audio fixtures; their presence does not establish Ludus support.
- [Glenn Fiedler's Introduction to Networked Physics](https://www.gafferongames.com/post/introduction_to_networked_physics/)
  motivates interacting-cube scenarios, including tightly coupled motion.
- [Omar Cornut and contributors' Dear ImGui demo](https://github.com/ocornut/imgui/blob/master/imgui_demo.cpp)
  provides a reference for the widget-gallery approach.

These authors and organizations supply the reference assets and scenario ideas;
Ludus-specific adaptations should document meaningful departures near the code.

## Compare clustered forward and deferred raster

Use the offscreen Editor benchmark to test whether opaque material work or
G-buffer traffic warrants a visibility-buffer resolve experiment. Joshua Barczak's
[*Comments on Future Directions for Compute in Graphics*](https://www.joshbarczak.com/blog/?p=1317)
(2017, “Visibility Buffers and Shading”) motivates keeping a forward control
that uses the hardware raster-to-fragment path.

Build the native Editor with render preview and tests, and build
`ludus_editor_tests` and `ludus_renderer_render_tests`. The renderer test target
cooks both controls from the [default graph](render-graph-authoring.md#default-clustered-deferred-lighting)
with shared geometry and material callbacks and tone mapping disabled.
Run both packages against the same saved World and camera. Choose a small
control and a representative view with opaque overdraw or costly material
shading. Use opaque or alpha-masked meshes and shared analytic lighting without
shadows, atmosphere, or mesh-light illumination. The deferred-only lighting
work would confound the comparison.

```bash
build=out/build/macos-clang-development
graphs="$build/modules/graphics/renderer/ludus-graphs/ludus_renderer_render_tests"
mkdir -p out/raster-comparison
export QT_QPA_PLATFORM=offscreen
export LUDUS_RASTER_BENCHMARK_PROJECT=/path/to/project
export LUDUS_RASTER_BENCHMARK_WORLD=example.world.json
export LUDUS_RASTER_BENCHMARK_NAVIGATION=/path/to/saved-camera.json
for recipe in rg2_clustered_Default rg2_clustered_forward_control; do
    LUDUS_RASTER_BENCHMARK_RUNTIME="$graphs/$recipe/$recipe.lurgrt" \
    LUDUS_RASTER_BENCHMARK_OUTPUT="out/raster-comparison/$recipe.json" \
    LUDUS_RASTER_BENCHMARK_IMAGE="out/raster-comparison/$recipe.png" \
        "$build/apps/editor/ludus_editor_tests" '[.raster-mesh-light-benchmark]'
done
```

The navigation file is optional when the default framing gives the desired
view. Use new output paths for subsequent runs, and alternate package order
across processes. Compare the pass timings and full GPU frame durations at the
benchmark's fixed 1600×1080 extent. The JSON also records the selected profile,
logical deferred attachment bytes and CPU time inside `RenderGraph::Render`
before the completion wait. CPU frame-time tails during interaction or gameplay
need their own capture. The optional PNG is clamped linear RGBA8 and can flag
image mismatches between controls. A future visibility resolve still needs its
own derivative, coverage and raw linear output checks.
[Qualification issue #1409](https://github.com/The-Ludus-Project/Ludus/issues/1409).

## SMS Figure 17 rough reflector

`tools/sms-benchmark` reproduces Figure 17 of Tizian Zeltner, Iliyan Georgiev and
Wenzel Jakob, *Specular Manifold Sampling for Rendering High-Frequency Caustics
and Glints*, SIGGRAPH 2020, [doi:10.1145/3386569.3392408](https://doi.org/10.1145/3386569.3392408).
The [authors' code and scenes](https://github.com/tizian/specular-manifold-sampling)
are pinned to `1f0e40342a8760450d5aa6202ea096feaa70256a`. Acquisition is explicit;
the scene descriptions, Gaussian normal map, floor texture and reference optical
data are hash checked. Preserve the upstream license and texture README.

The original scene has a small rectangular area light, a normal-mapped gold
rectangle and a diffuse tiled floor. It uses isotropic **Beckmann** roughness
`0.005`, `0.02` and `0.1`, slope-space normal interpolation, Gaussian pixel
filtering and a maximum path depth of six. Gold optical constants use the
reference renderer's spectral-data-to-RGB conversion. Replacing this material
with GGX or interpolating RGB normals changes the experiment. The public engine
reference material path does not yet represent this exact combination.

Two executions have different, explicit purposes:

- `render_original.py` runs the actual authors' scalar RGB SMS and filtered path
  tracer. It preserves geometry, materials, lighting and pixel filtering. Image
  extent, seed, sample cap, timeout and CPU thread count are recorded overrides.
  The supplied upstream `render.py` currently requests one second, whereas the
  paper's Figure 17 reports five minutes; choose the budget explicitly.
- `render_connections.py` executes sampling, connection transport, shading and
  accumulation in Slang on a physical GPU. Its declared integral is only
  **camera → diffuse floor → rough gold → area light**, with pixel-centre rays
  and level-zero bilinear textures. Camera hits on the gold are outside this
  integral and remain black. It retains every floor pixel, including contact
  regions. This is a headless research harness using Ludus's pinned shader
  compiler and FoundationMath, not an imported World/editor viewport or a full
  reference path tracer. Its images must not be treated as full-integrator
  parity with the original Mitsuba renders.

### Set up the reference and assets

Use Python 3.12 and a separate environment. The optional requirements do not
enter installed Ludus host tooling. Provide the project's prepared CMake/Ninja
and Slang tools, or equivalent declared reference-build tools.

```bash
python3 -m venv out/sms-env
source out/sms-env/bin/activate
python -m pip install -r tools/sms-benchmark/requirements-reference.txt
python tools/sms-benchmark/setup_reference.py acquire --upstream out/sms-upstream
python tools/sms-benchmark/setup_reference.py build --upstream out/sms-upstream \
  --build-directory out/sms-build --output out/sms-build-record \
  --cmake out/host-tools/venv/bin/cmake
python tools/sms-benchmark/benchmark.py prepare --upstream out/sms-upstream --output out/sms-figure17
python tools/sms-benchmark/render_original.py --original out/sms-figure17/original \
  --mitsuba out/sms-build/dist/mitsuba --output out/sms-original \
  --extent 128 --samples 16 --seconds 600 --threads 4
```

The last command is a fixed-sample reproduction, not an equal-time comparison.
For timed runs, increase the sample cap and set the timeout explicitly. A very
short timeout can leave unvisited pixels nonfinite; the wrapper rejects such an
image and retains its EXR/log. Apple Silicon compatibility changes affect
libpng headers/SIMD selection, a build-time RGB table generator's rpath, legacy
Enoki diagnostics and the debugger trap. Transport sources remain unchanged;
the build record contains the commands, dependency revisions and patches.

### Validate and compare the portable proposals

The CPU oracle preserves the original slope interpolation, shading frame,
Beckmann distribution and Smith approximation. GPU checks compare raw
intersections, normals, nonzero narrow highlights, connection contributions and
analytic derivatives. Independent central differences check the CPU derivatives.

The experimental slice proposal uses a triangulated **surrogate of the inverse
sampling map**, conditioned on the fixed receiver and light centre. It enumerates
all regular inverse triangles, selects branches with inverse-determinant weights,
keeps null outcomes, and mixes ten percent uniform reflector-area sampling for
support. Conservative seed-space bins accelerate enumeration. The physical
integrand still evaluates the original continuous normal map and Beckmann BSDF.
A minimum guide alpha broadens only the proposal to tolerate interpolation error;
it does not change physical roughness. This is not an exact nonlinear BSDF-map
inversion, nor does finite-precision replay certify its mathematical density.

Portable SMS uses offset-normal sampling, a smoothed LEAN initialization,
analytic slope-map derivatives and independent re-hit trials. It estimates its
own solver's basin probabilities. Its constraints, tolerances and UV re-hit
criterion differ from the original angle-difference solver. CPU FP64 and GPU
FP32 may reach different valid branches; constraint checks and estimator/probe
checks are distinct requirements. Report the finite Bernoulli cap and every
capped sample rather than claiming an uncapped unbiased implementation.

```bash
python -m unittest discover -s tools/sms-benchmark -v
python tools/sms-benchmark/benchmark.py validate --upstream out/sms-upstream \
  --output out/sms-material-check --compiler out/shader-tools/slang/bin/slangc
python tools/sms-benchmark/validate_affine.py --upstream out/sms-upstream \
  --output out/sms-affine-check --compiler out/shader-tools/slang/bin/slangc
python tools/sms-benchmark/validate_transport.py --upstream out/sms-upstream \
  --output out/sms-transport-check --compiler out/shader-tools/slang/bin/slangc
python tools/sms-benchmark/render_connections.py prepare --upstream out/sms-upstream \
  --output out/sms-connections --extent 32 --resolution 64
python tools/sms-benchmark/quadrature_reference.py --prepared out/sms-connections \
  --output out/sms-connection-reference --samples 1048576 --replicates 16 \
  --compiler out/shader-tools/slang/bin/slangc
python tools/sms-benchmark/render_connections.py run --prepared out/sms-connections \
  --output out/sms-connection-comparison --seconds 15 --runs 3 \
  --reference-samples 33554432 --reference-directory out/sms-connection-reference \
  --compiler out/shader-tools/slang/bin/slangc
python tools/sms-benchmark/estimate_cpu.py --upstream out/sms-upstream --output out/sms-cpu-probes
python tools/sms-benchmark/check_integral.py --upstream out/sms-upstream --output out/sms-integral-check
python tools/sms-benchmark/estimate_cpu.py --upstream out/sms-upstream \
  --output out/sms-cpu-mis --methods connection-mis --samples 65536
python tools/sms-benchmark/analyze_results.py --runs out/sms-connection-comparison \
  --reference out/sms-connection-reference --output out/sms-analysis \
  --quadrature out/sms-integral-check --cpu-probes out/sms-cpu-probes --cpu-mis out/sms-cpu-mis
```

Each output directory must be new. GPU execution requires `wgpu`, a physical
adapter and timestamp-query support; software fallback is rejected. The uniform
layout and sampler policy constants have one owner in `layout.py`, which emits
the corresponding Slang declarations. Portable methods compile into separate
entry points, avoiding unused solver code in the inexpensive baseline.

The connection-MIS baseline combines uniform reflector-area and cosine-weighted
floor sampling. The recommended reference uses independently shifted Hammersley
points for both charts, uniform area-light endpoints and balance MIS. It uses
neither slice guides nor Newton roots. Each shift estimates a complete image;
uncertainty comes from variation across shifts, rather than treating correlated
points as independent. Increase reference samples when its empirical relative
standard error exceeds two percent. CPU tensor quadrature checks selected pixels
without either sampler and reports resolution convergence. Neither check is a
certified global error bound. The older slice/connection mixture reference remains
available for experiments, but its rare outliers made it unsuitable for this run.

Timing includes completed submission, parameter update and timestamp readback.
Compilation/warm-up and receiver-dependent table preparation are separate and
must be included when discussing cold start or motion. Methods rotate order
across independent seeds; timing-only extensions match completed wall budgets
and retain the initial arrays. Report actual time mismatch, GPU time, sample
counts, nulls, caps, memory, raw linear error and reference uncertainty. At this
small image size, per-band submission/readback overhead and GPU occupancy affect
rankings. The reference parallelizes over sample blocks; its throughput must not
be presented as a matched implementation comparison with the serial per-pixel
samplers. No denoising, outlier clamping or temporal reuse enters these measurements.

This experiment does not establish superiority over present-day SOTA. The
released [PSMS-ReSTIR](https://graphics.cs.utah.edu/research/projects/psms-restir/)
ReflectivePlane uses zero roughness and a finite-roughness port needs its own
matching-integral validation.

### Coupled two-rough-reflection sampler

Run this asset-free analytic fixture with the benchmark dependencies above.
Select a new output directory for each run.

```bash
python tools/sms-benchmark/benchmark_rough_pair.py \
  --compiler out/shader-tools/slang/bin/slangc --output out/rough-pair-gpu \
  --budget gpu --seconds 0.25 --runs 3
python tools/sms-benchmark/benchmark_rough_pair.py \
  --compiler out/shader-tools/slang/bin/slangc --output out/rough-pair-wall \
  --budget wall --seconds 0.50 --runs 3
```

The fixture isolates the transport from a fixed receiver through two rough
reflections to a fixed emitter. Two square patches face each other at a unit
separation. The endpoint-to-patch normal distances are also one unit. Every
estimator uses the same scalar, scene-linear path integral over the two
patches, including all three geometry factors and both BSDFs. The endpoint
positions index separate receiver integrals. All three segments are
unobstructed and both lobes have finite roughness.

Thanks to Shlomi Steinberg, *Slice Sampling: Importance sampling with an
auxiliary map* (University of Waterloo draft), sections 4, 6.5 and 7, for the
fresh proposal and same-seed shift. The benchmark uses a reciprocal rough
lobe: a Gaussian of the sum of the two warped slopes, with componentwise
warp `w(t) = t (1 + k |t|)`. The frozen conditional samples by subtracting the
known warped slope from a Gaussian residual and applying the inverse warp.
The conditional PDF includes the sampled slope's warp derivative. The BSDF
itself uses the symmetric Gaussian factor. Its hemispherical reflectance is
bounded by one because the warp derivative is at least one and the
projected-solid-angle measure per unit slope area is at most one.

- **Fresh slice.** Each axis couples the two patch coordinates through
  their shared segment. Eliminating the coordinates leaves a strictly
  increasing scalar equation bracketed by zero and the uncoupled separation.
  Each full-plane seed therefore has exactly one root. The finite patches
  restrict the chart, and roots outside either patch count as zero-valued
  draws. The Gaussian seed density times the full path-to-seed determinant is
  the proposal subdensity in product area measure. An unresolved numerical
  solve fails the benchmark.
- **BDPT with MIS.** The fixed-endpoint baseline uses the three ways to split
  the two internal vertices between the subpaths. It samples the two
  endpoint directions independently, or extends one cosine-sampled endpoint
  direction by one BSDF sample from either side before connecting to the
  opposite endpoint. It chooses one split uniformly per draw and evaluates
  all three PDFs in the same area measure for one-sample balance MIS. See
  [Veach, Chapters 9 and 10](https://graphics.stanford.edu/papers/veach_thesis/).
- **Same-seed shift.** Draw a source path at a receiver offset by 0.4 units
  and replay its two Gaussian residuals at the target receiver. The matched
  chart is unique. The forward area Jacobian is the ratio of source to
  target path-to-seed determinants. The shifted PDF is the source PDF
  divided by that Jacobian. Mix shifted and fresh target draws equally.
  Reverse mapping determines where the shifted proposal contributes to the
  mixture PDF. The fresh half covers paths whose reverse root lies off the
  source patch. The experiment generates source paths on demand and times
  that work. See [Lin et al., GRIS](https://research.nvidia.com/labs/rtr/publication/lin2022generalized/)
  for the support contract for shift mappings.

The float64 CPU oracle uses bracketed bisection, and the Slang GPU kernel
uses safeguarded Newton iteration. The parity probe compares 4,096 matched
draws per method, including patch membership, positions, proposal densities
and raw contributions. It checks GPU shift round trips against the original
source positions and checks the Jacobian against a CPU finite difference.
The benchmark integrates the physical contribution directly by Gauss–Legendre
quadrature at orders 30 and 34, splitting at slope sign changes. Each
receiver estimate must agree with the refined integral within six estimated standard errors plus an absolute
tolerance of `1e-5`. `--probe-only` runs the paired checks without timing or
quadrature. The timed comparison uses physical GPU timestamps or completed
wall time, with independent seeds and rotated estimator order. Cold shader
compilation, warm-up and final output readback are outside both budgets.
Read `report.json` for actual budgets, counts, null rates and RMSE, and
`linear.npz` for raw per-receiver values and errors.

On Apple M5 Metal with Slang 2026.1.2 and wgpu 0.32.0, the two quadrature
orders differed by at most `3.22e-10` over the 8 × 8 receiver grid. The paired
probe checked 1,382 accepted shifts, with a maximum round-trip coordinate
error of `4.72e-6` world units and a maximum absolute Jacobian discrepancy of
`6.99e-6` against the CPU oracle. The probe also exercised 282 fresh draws
outside the shifted support.

| Estimator | Median RMSE at 0.25 s GPU | Median RMSE at 0.50 s wall | Approximate null rate |
| --- | ---: | ---: | ---: |
| Fresh slice | 6.93e-6 | 7.53e-6 | 13.2% |
| Fixed-endpoint BDPT with MIS | 3.78e-4 | 3.67e-4 | 90.5% |
| Same-seed shift and fresh mixture | 7.75e-6 | 7.74e-6 | 19.5% |

The medians are over three trials with seeds 1000–1002. Actual timed budgets
were 0.2501–0.2508 GPU seconds and 0.5000–0.5027 wall seconds. Fresh slice
reduced median wall-time RMSE by about 49× relative to BDPT with MIS on this
fixture. Generating source paths on demand gave the shift mixture slightly
higher median RMSE than fresh slice in both budgets.

The [rough pair render graph template](render-graph-authoring.md#preview-two-rough-reflections-with-slice-sampling)
shares this sampler. The equal-time measurements below use the benchmark's
workgroup cache, while the authored graph runs two passes through a graph Buffer.

#### Reuse cached paths across neighboring receivers

The optional `cached-neighbors` estimator pairs consecutive receivers
horizontally, keeping the last receiver of an odd-width row fresh. Each
receiver draws one fresh path per round. A GPU workgroup stores the solved
positions, source density and failure status in shared memory so the paired
receiver can replay that path's seed. Alternating cache slots let each round
publish with one workgroup barrier. The cache is rebuilt from independent
draws every round. The two receivers in a pair have correlated estimates.

For a target path, the shifted proposal has the fresh target density wherever
the reverse root belongs to the source patches. The same seed has the same
Gaussian density on either chart, and the area Jacobian converts between the
two. Each round sums the fresh and shifted contributions with multi-sample
balance MIS. The fresh draw gets half weight where both proposals cover it
and full weight elsewhere. An accepted shifted draw gets half weight after
division by its shifted PDF. A null source or off-patch shift contributes
zero. The GPU checks reverse support by bounding the monotone separation
equation with the four patch edges. Near a boundary it uses the ordinary
inverse solve. See [Veach, Chapter 9](https://graphics.stanford.edu/papers/veach_thesis/)
for the multi-sample balance heuristic.

```bash
python tools/sms-benchmark/benchmark_rough_pair.py \
  --compiler out/shader-tools/slang/bin/slangc --output out/rough-cache-gpu \
  --methods slice cached-neighbors --budget gpu --seconds 0.25 --runs 7
python tools/sms-benchmark/benchmark_rough_pair.py \
  --compiler out/shader-tools/slang/bin/slangc --output out/rough-cache-wall \
  --methods slice cached-neighbors --budget wall --seconds 1.00 --runs 7
```

Both budgets include fresh path generation, cache publication, shifting,
MIS and accumulation. The wall budget also includes dispatch and timestamp
readback. The report's sample counts, null rates and variance refer to whole
rounds, each of which includes one fresh draw and, where paired, one shifted
draw. The CPU cache oracle uses bisection for reverse support. The GPU parity
probe uses four consecutive rounds per lane on an odd-width grid to check
cache slot turnover, singleton receivers, null source paths, off-target
shifts, fresh-only coverage, PDFs, Jacobians and weighted contributions.

On the same Apple M5 Metal setup, the cache passed 10,368 paired CPU/GPU
rounds, including 8,470 accepted shifts and 110 draws whose support came
only from the fresh proposal. The largest absolute discrepancy was
`2.54e-6` in weighted linear radiance and `7.22e-6` in the Jacobian.
Both timed estimators agreed with independent quadrature across the 8 × 8 grid.

| Estimator | Median RMSE at 0.25 s GPU | Median RMSE at 1.00 s wall | Mean variance per round |
| --- | ---: | ---: | ---: |
| Fresh slice | 9.72e-6 | 4.97e-6 | 6.26e-5 |
| Cached neighbors | 1.13e-5 | 4.74e-6 | 4.99e-5 |

Each budget used seeds 1000–1006, rotating estimator order. Completed times
were 0.2500–0.2519 GPU seconds and 1.0001–1.0031 wall seconds. A shorter
wall-time pilot had a host scheduling overrun, so the table uses the
one-second rerun. The cache cut mean variance per round by about 20%, but
completed about 22% fewer rounds at equal GPU time. Its median RMSE was
about 17% higher at equal GPU time and 5% lower at equal wall time, where
it had lower error in five of the seven trials. These measurements give
mixed evidence for an equal-time advantage.

The benchmark covers one regular chart with a reciprocal analytic lobe.
Curved geometry and real materials still require root enumeration and a
comparison against a full scene BDPT integrator. Spatial and temporal
reservoirs would require their own reuse estimator and validation.

## Ludus integration suites

| Suite | Controlled setup | Systems and observations |
| --- | --- | --- |
| World lifecycle | Repeated bounded spawning and destruction | Entity generations, component mutation, stale handles, events and allocation behavior. |
| Transform hierarchy | Deep chains, wide trees, moving parents and reparenting | Propagation, dirty tracking, hierarchy integrity and update cost. |
| Streaming and filesystem | Traversal, reversal and teleportation | Background reads, cancellation, cache behavior and stalls; resource residency when supported. |
| Save and replay | Checkpoint, mutate, restore and repeat inputs | Serialization, restored references, version handling, RNG state and scoped reproducibility. |
| Jobs and scheduling | Independent tasks, dependency chains and uneven work | Dependency correctness, synchronization, cancellation, shutdown and load balancing. |
| Time and pacing | Pause, single-step, slow motion and injected stalls | Fixed-tick accumulation, interpolation, timers, catch-up and dropped-time policy. |
| Platform and input | Resize, focus changes, fullscreen and device reconnects | Window lifecycle, DPI, input cancellation, device state and presentation recovery. |
| Editor and content | Import, edit, undo, save, reload, build and run | Document state, dependencies, persistence, setup repair and SDK integration. |

## Adding and running a case

1. Select one observable contract and inspect its current implementation and
   evidence. State the required backend, devices and engine features.
2. Pin the source revision and asset variant, verify license/attribution, and
   record hashes, units, coordinate conventions and import/conversion choices.
   Keep external acquisitions explicit and repeat runs independent of downloads.
3. Define initial state, ordered inputs, camera/listener trajectory, seed, tick
   policy, workload size, warm-up, measured duration and reset/termination rules.
4. Define expected observations and correctness tolerances before accepting a
   baseline. Use image comparisons, physical invariants, signal analysis or
   state checks appropriate to the tested contract. Listening/playtesting can
   supplement automated observations.
5. Build a small case using existing module boundaries and test helpers. Support
   interactive inspection and scripted execution; use headless execution where
   the contract allows it. For standalone SDK consumers, perform the project
   checks in [project setup](project-sdk-workflow.md).
6. Run supported correctness checks first, then repeated performance measurements
   with recorded build, hardware, backend and workload identity. Keep sanitizer
   measurements separate from ordinary performance baselines. Report distributions
   and workload-specific metrics rather than FPS alone.
7. Preserve failure artifacts and write an evidence record. Link it from this
   guide and the relevant implementation owner; update the case's status and
   name the next runnable case or concrete missing prerequisite.

Every evidence record should include the case/source/content identities, exact
commands, environment, expected and observed checkpoints, invariant results,
measurements, artifact paths and remaining limitations. Use ignored `out/` for
generated captures/reports; keep authored instructions and concise evidence in
`docs/`. Treat missing capabilities as unsupported/skipped, incomplete runs as
incomplete, and failed invariants as failed. Preserve the first failure before
retries. Calibrate performance thresholds on a declared runner before making
them required gates. See the owning engineering quality policy for details.

## Continuation status

| Item | Status | Next action |
| --- | --- | --- |
| Scene catalog and initial priorities | Recorded | Use this guide when selecting the next case. |
| Cornell box | Candidate | Review the rendering capability and define the representation and expected observations. |
| Sponza and San Miguel | Deferred | Admit a version when geometry/material import and rendering support the intended measurements. |
| Physics stack, tumbler and character course | Candidate | Confirm subsystem availability before adding runnable cases. |
| Audio and world fixtures | Candidate | Reuse the existing examples and add one controlled case at a time. |
| Other catalog entries | Backlog | Select according to implemented capabilities and a concrete validation need. |

Update this table with links to evidence as cases are implemented and executed.
An external sample running in its original engine is reference behavior; a
Ludus result requires the corresponding Ludus execution and observations.

## Controlled directional luminaire room

`tools/luminaire-benchmark/benchmark.py` generates a two-metre, Y-up room with
matte floor, three walls and a raised receiver. The ceiling panel faces down;
its centre is `(0, 0.97, 0)` metres and its side is 0.7 metres. The open front
admits a camera at `(0, 0, -3.2)` looking towards `(0, -0.1, 0)`. Receivers have
unit diffuse reflectance. No other lights or environment illuminate surfaces.
This is an analytic-panel diagnostic, with no internal fixture optics.

The generator emits the same receiver vertices as PBRT triangle meshes for
Ludus import and OBJ meshes referenced by a Mitsuba XML scene. It copies the
existing [slice-sampling graph](render-graph-authoring.md#sample-a-directional-luminaire-with-slice-sampling)
without changing its shader implementation. The scene files contain receiver
geometry only; in Ludus the World's luminaire supplies the panel. The XML alone
therefore renders black until a luminaire estimator is attached. The panel is not
camera-visible.

Requires Python 3.10 or newer and NumPy in a separate benchmark environment;
this optional research tool adds no dependency to installed Ludus host tooling.
From the checkout root:

```bash
python tools/luminaire-benchmark/benchmark.py prepare --output out/luminaire-room
ludus-cook pbrt out/luminaire-room/scene-v4.pbrt out/luminaire-cooked
python tools/luminaire-benchmark/benchmark.py run --output out/luminaire-cpu --samples 131072
python -m unittest discover -s tools/luminaire-benchmark -v
```

Use a fresh output directory for each run; existing data is never overwritten.
To inspect the room in an existing configured Ludus project, import
`scene-v4.pbrt`, add the generated `graph` package, and route its
`SliceSamplingLuminaire` configuration to the viewport. The configuration exposes
`color` (defensive slice sampling), `sliceSampling`, and `uniformArea` outputs.
Add the room's panel with **Add World Preset → Luminaire** and set its
Transform to position `(0, 0.97, 0)` and rotation `(-90, 0, 0)`, keeping the
default half extent `0.35` and beam profile; the headless kernels in
`tools/luminaire-benchmark/shaders` use the same panel. Follow the linked graph guide for
native compilation, output selection and raw linear frame capture. Camera and
visibility rays execute in the existing GPU compute-BVH implementation.

### Shared radiance and CPU diagnostic

The CPU evaluator reads the GPU-owned steering, width, strength and tint tables
from `luminaire.slang`; it implements that file's affine Gaussian radiance with
a new World luminaire's default profile. Both CPU proposal paths call this one evaluator.
The fixture fingerprint covers the tables, profile, placement and radiance scale.
A checkpoint from another profile is rejected rather than silently compared.

`run` evaluates eight fixed receivers, including overlapping beams and far tails.
It reports RGB means, sample variance, standard errors, null rate and wall time.
Independent tensor-product Gauss-Legendre integration on each tile supplies the
reference; orders 128 and 256 must agree within the declared tolerance before
measurements are written. The CPU point diagnostic is **unoccluded**; it does not
replace room image comparisons or visibility tests. The generated GPU graph
traces the room's actual shadows. The Python reference does not add a new engine
transport mode; the existing production GPU proposal is retained unchanged.

Null draws count in the requested sample budget. The defensive mixture retains
10% uniform-area sampling. The normalization regression integrates a constant
through the actual mixture, including null outcomes. Area-to-solid-angle
conversion is checked independently against the geometric cosine/distance law.

### PureLight receiver-flow comparison

Thanks to Pedro Figueiredo, Zixuan Li, Beibei Wang, Miloš Hašan and Nima Khademi
Kalantari, *PureLight: Learning Complex Luminaires with Light Tracing*, SIGGRAPH
Asia 2026, [section 3.4](https://arxiv.org/abs/2606.04319), for the released
conditional receiver-flow architecture and spherical-cap maps. Thanks to Shlomi
Steinberg, *Slice Sampling: Importance sampling with an auxiliary map* (draft),
sections 6.2 and 6.4, for the analytic panel and branch construction. The GPU
source preserves its original attribution.

The optional `purelight_adapter.py` imports the **official implementation** at
commit `d0969834eef28f1453056508ba39eaef0471a5a2` from a separate checkout. Set up
its documented CUDA, PyTorch and tiny-cuda-nn environment first. Nothing downloads
or installs implicitly when a benchmark runs.

```bash
git clone https://github.com/pedrovfigueiredo/purelight.git out/purelight-upstream
git -C out/purelight-upstream checkout d0969834eef28f1453056508ba39eaef0471a5a2
python tools/luminaire-benchmark/purelight_adapter.py --upstream out/purelight-upstream --output out/luminaire-training --steps 10000
python tools/luminaire-benchmark/benchmark.py run --output out/luminaire-comparison --upstream out/purelight-upstream --purelight out/luminaire-training/receiver-flow.pt --samples 131072
```

The adapter uses the released TCNN model dimensions and encodings (four coupling
layers, five hidden layers of width 128, 32 bins), and the released concentric-disk
spherical-cap mapping. It retrains a grayscale radiance guide on the analytic
panel's eight receivers with forward KL. Exact panel radiance and the defensive
proposal supply importance-weighted training examples. Both comparison estimators
then evaluate **the same analytic RGB radiance**, receiver cosine and Lambertian
BSDF. PureLight proposal directions missing the panel remain null draws. Its
PDF is converted from cap coordinates to solid angle, then to panel area.

This is a controlled adaptation of the receiver sampler, not reproduction of the
full PureLight system, its original training distribution/schedule, or its Fast
radiance estimator. Training on fixed probes establishes no unseen-position
performance. Training duration is saved separately. CPU reports include NumPy
physical evaluation and GPU-to-CPU adapter transfers; they are not a fair native
GPU speed comparison. Repeat seeds and use matched implementations/hardware for
equal-time claims. Missing CUDA or dependencies is an explicit failure; no
histogram is labelled PureLight and no absent result is reported as measured.

### CUDA-free portable compute implementation

CUDA is a dependency of the released implementation, not of the sampling
construction. `portable_flow.py` trains the receiver flow on the CPU and
`export_compute.py` emits a Ludus package whose Slang compute shader executes the
encodings, MLPs, inverse splines, spherical-cap mapping and PDF on the GPU.
Both proposals retain the original analytic panel radiance. The generated shader
specializes the shared receiver BSDF to unit Lambertian, matching this room and
its CPU oracle. Use this package only with the declared matte benchmark geometry.

The portable model keeps four coupling layers, five hidden layers of width 128,
32 spline bins, periodic quartic OneBlob encoding and eight smoothstep dense-grid
levels with eight features each. It uses explicit MLP biases and zero-initialized
grid nodes, stores only trained grid vertices, and treats absent vertices as zero.
These training/implementation differences must be reported when discussing
PureLight; this is a portable adaptation rather than an official checkpoint port.

The exporter stores weights as per-layer signed bytes and evaluates in float32.
It also exports the exact decoded CPU model as `portable-model.npz`, so CPU/GPU
parity compares the same quantized function. This changes the learned proposal;
it does not quantize physical radiance or replace the actual proposal PDF.
Weight quantization is a benchmark variable, not evidence of equivalence to the
released CUDA implementation. A float32 parameter-buffer implementation remains
useful future work for a stronger baseline. Emission settings are frozen in the
exported package; retrain and re-export after profile edits.

For an isolated local environment, use Python 3.12 and the recorded dependencies:

```bash
python3.12 -m venv out/luminaire-env
out/luminaire-env/bin/python -m pip install -r tools/luminaire-benchmark/requirements-portable.txt
out/luminaire-env/bin/python tools/luminaire-benchmark/portable_flow.py --upstream out/purelight-upstream --output out/portable-training --steps 1000
out/luminaire-env/bin/python tools/luminaire-benchmark/export_compute.py --model out/portable-training/flow.npz --room out/luminaire-room --output out/portable-room-graph
out/luminaire-env/bin/python tools/luminaire-benchmark/benchmark.py run --room out/luminaire-room --output out/portable-cpu-comparison --portable out/portable-room-graph/portable-model.npz --samples 131072
```

Acquire the pinned PureLight checkout as above. Only its differentiable spline
reference is imported by the CPU trainer; CUDA, tiny-cuda-nn and Mitsuba are not
needed for this route. The initial training uses eight fixed receivers and a
finite frozen training dataset; it establishes no unseen-position performance.
Use a fresh output directory for every command that writes artifacts.

Build the exported `main.lushader.json` through the same Ludus graph workflow.
Its outputs are `color` (defensive slice), `sliceSampling` (raw slice), and
`portablePureLight` (portable learned proposal); the root exports the latter as
`portablePureLightOutput`. The original room package retains the uniform-area
control. Generated Metal indentation is compacted at the shader-helper boundary
without changing tokens, literals, comments, continuations, or line numbers.
This keeps the combined shader inside the existing artifact budget; no source,
package or artifact capacity is raised.

For raw GPU parity, with the pinned Slang compiler selected:

```bash
out/luminaire-env/bin/python tools/luminaire-benchmark/validate_compute.py --graph out/portable-room-graph --compiler /path/to/pinned/slangc --output out/portable-parity
```

This uses wgpu solely as a verification driver for the same exported Slang code.
A physical GPU is required; software adapters are rejected. It saves raw inputs,
CPU expectations, GPU outputs, tolerances and model/shader identities. The final
Metal run on Apple M5 checked 256 draws across eight receivers and passed all five
records: transformed coordinates/PDF, inverse replay, cap direction/PDF, and
physical radiance weights and incoming light directions. The endpoint-derived
incoming direction avoids a pinned Slang WGSL code-generation error for unary
negation of a sum of vector products; checking radiance alone misses that error.
The exported graph also compiled to Metal and WGSL
and cooked into a native runtime package. This is execution evidence for compute
inference, not a complete Ludus room image benchmark or an equal-time claim.

### Headless equal-time room images

`render_comparison.py` runs the generated room triangles and both exported
sampling shaders on a physical GPU through wgpu. Both methods execute primary
intersection, direct lighting, receiver shading, shadow intersection and
accumulation on the GPU, with the same analytic radiance and unit Lambertian
BSDF. This is a standalone research driver with linear triangle traversal;
its timings do not measure the Ludus editor or the native compute-BVH renderer.
The CPU oracle independently implements geometry, visibility and tile-area
quadrature. This harness does not introduce a new native reference transport mode.

```bash
out/luminaire-env/bin/python tools/luminaire-benchmark/render_comparison.py --graph out/portable-room-graph --room out/luminaire-room --compiler /path/to/pinned/slangc --output out/room-equal-time --extent 128 --seconds 25 --runs 3 --reference-samples 65536
out/luminaire-env/bin/python tools/luminaire-benchmark/check_room_reference.py --room out/luminaire-room --results out/room-equal-time
python tools/luminaire-benchmark/show_comparison.py out/room-equal-time
```

The last command additionally requires Pillow. Both estimators use fixed pixel
centres without antialiasing, indirect illumination, denoising or temporal reuse.
Null draws count in samples per pixel. Each dispatch shades one sample over an
eight-row band; band order is shuffled per sweep. Completed rendering wall time
includes uniform updates, command encoding/submission, GPU completion and
timestamp readback. Compilation, warmup, training, final image readback and
reference construction are excluded and reported separately. Host synchronization
can dominate slice timings; GPU timestamp totals are recorded separately.

Trials alternate method order and share independent seeds. The budget is at
least twice the slower full-frame warmup. The faster method is extended to match
the slower method's actual completed time, without consulting image error; a
remaining mismatch above 1% is rejected. Samples per pixel can differ by one
between bands. Raw linear RGB, sample counts, source/model/geometry hashes and
per-trial metrics are retained. The reference averages two independent defensive
slice runs, each at least the requested sample count and eight times the largest
measured per-pixel count. Their difference estimates reference noise.

The independent CPU check compares eight selected lit pixels against per-tile
Gauss-Legendre quadrature with shadows, split at the horizontal rectangular
occluder's projected silhouette: orders 128/256 must change by at most
0.1%, and the GPU reference must agree within 3%. Failed checks preserve evidence
and block preview generation. This spot check complements the raw direction/PDF
parity test; it does not prove every pixel or the full native renderer correct.
The preview uses one exposure for all images, the middle trial by run order,
and a common logarithmic error scale. MSE and NRMSE are computed before display
conversion across the whole image; NRMSE is the ratio of RGB error and reference
L2 norms, including black pixels.

The November 2, 2026 Apple M5/Metal run used 128 by 128 pixels, three paired
seeds and two independent 65,536-sample-per-pixel references. Actual paired wall
times differed by at most 0.053%. Medians (ranges across three trials) were:

| Measurement | Defensive slice | Portable receiver flow |
| --- | --- | --- |
| Completed rendering wall time | 25.467 s (25.155–25.601) | 25.456 s (25.155–25.588) |
| Mean samples per pixel | 184.06 (162.44–258.00) | 2.50 (2.50–2.56) |
| Pixel samples per wall second | 119,885 (104,501–165,113) | 1,628 (1,609–1,641) |
| Linear RGB MSE | 0.00003816 (0.00003015–0.00004781) | 1.3724 (0.6702–4.0874) |
| Linear RGB NRMSE | 4.82% (4.29–5.40%) | 914.32% (638.93–1577.91%) |
| GPU timestamp total | 2.173 s (1.324–2.298) | 23.840 s (23.043–23.915) |

The reference's estimated NRMSE was 0.182%; eight CPU quadrature spot checks
agreed within 1.125%, with quadrature orders converged. The 256-draw GPU/CPU
inference and incoming-direction check passed. The flow used 1,000 CPU training
steps on eight fixed receivers (450.6 seconds), int8 weights and float32
evaluation. Training and compilation were excluded from the matched budgets.
The learned proposal produced sparse bright outliers at these sample counts.
This records the current portable implementation, including substantial host
synchronization overhead for slice sampling and an unoptimized neural shader.
It does not measure the official PureLight model or establish a SOTA advantage.

### Qualification still required

The setup does not establish a SOTA advantage. The released rough-flower receiver
checkpoints represent a different luminaire and are rejected for this panel
experiment. A complex-fixture comparison additionally needs its invertible
emission teacher, radiance evaluator, compatible geometry and complete inverse
branch handling. The release documents pretrained distilled appearance and
receiver proposals, but its teacher must be acquired or trained separately.

Matched native image captures and timings, convergence/training-budget sweeps,
quantization ablations, comparison against the official trained receiver model,
Fast-system bias comparisons, occlusion, distance sweeps, edits and CPU/GPU
statistical image parity remain unqualified. Report the portable adaptation's
limitations and all training costs rather than calling its measurements a
reproduction of PureLight's published performance.
