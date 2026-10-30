# Point and line direct lighting implementation tasks

All boxes start unchecked. Read [requirements](requirements.md) and
[design](design.md). Each phase is one reviewable pull request against
`develop` from its own `codex/` branch and worktree. Update evidence before
checking a task. Mocks, skipped checks or optionalized failures do not count as
acceptance. The GPU tests here use `TriangleLoop` and must not skip for a
missing native ray profile; report any other skip as a skip, with the device.

## PL0 Feasibility (no product change)

Record results in [design section 4](design.md#4-pl0-evidence). Evidence is from
an Apple M5 on Metal (macOS 26.6). Vulkan was not run: no Linux or Vulkan host
was available.

- [x] **Prototype query.** Write the D2/D3 linear-scan query as a throwaway
      kernel in a local fixture (not committed as product code). Run it on
      Metal against hand-computed intervals for one occluder, two overlapping
      occluders and a coplanar occluder.
      *A `TriangleLoop` scene compute kernel matched hand-computed intervals
      within 9e-8 for one occluder, two overlapping occluders plus a disjoint
      one, a coplanar occluder, an abutting quad, and occluders behind the
      receiver and beyond the segment (design 4.1).*
- [ ] **Capacity.** Count visible intervals on the PL13 fixtures plus a
      dense-blinds fixture. Measure register use and occupancy for capacities
      8, 16 and 32 on Metal. Record the chosen
      `LUDUS_SCENE_LINE_INTERVAL_CAPACITY`.
      *Counts, occupancy and the choice are recorded: single-occluder fixtures
      reach at most 2 visible intervals (3 running) and blinds with `K` slats
      about `K`. Every capacity keeps `maxTotalThreadsPerThreadgroup` at 1024,
      and time does not order by capacity. Chosen capacity: 32 (design 4.2).
      Left unchecked because register use was not measured: Metal exposes no
      register count, and Xcode's GPU profiler is not installed here. Vulkan
      was not run.*
- [x] **Merge tolerance.** Find the smallest tolerance with no visible crack
      along shared edges of a tessellated sphere and a two-triangle quad
      occluder, and record the visible length it drops. Record the chosen
      `LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE`.
      *Intersecting each edge from its lexicographically smaller endpoint
      leaves no crack at any tolerance, including 0. In triangle winding order,
      cracks persist up to 1e-3, so D2 step 2 now requires canonical order.
      Chosen tolerance: 1e-6, which drops at most 2.1e-7 m (design 4.3).*
- [x] **Packed nodes.** Confirm whether render-graph ray passes pack
      `ReferenceBvhNode` data under `NativeQuery` and `TriangleLoop`. If a
      strategy omits them, record what PL3 must add.
      *Both strategies pack the nodes, with identical counts, for example 2047
      nodes for 3980 triangles. In a plan with a `NativeQuery` pass, however,
      `Order` is opacity-partitioned, so PL3 needs an unpartitioned leaf order
      for scenes with masked triangles (design 4.4, D5).*
- [x] **Random dimensions.** Confirm `RANDOM_MAX_DIMENSION` admits dimension 14
      (design D8). If not, revise the D8 layout before PL2.
      *`RANDOM_MAX_DIMENSION = 0xffff`, so the D8b layout is unchanged
      (design 4.5).*
- [x] **Cost.** Time the prototype query per pixel against `TestVisibility` with
      `TriangleLoop` on the same scene. Record it as context, not as an
      acceptance threshold.
      *On 3980 triangles a line query costs 0.90-1.02× a `TriangleLoop` shadow
      ray, about 175 ns per pixel at 640×480. On 14-44 triangles it costs
      1.4-8.2×, with up to 2× noise between runs (design 4.6).*

## PL1 Line visibility query and line-sampling template

Evidence is from an Apple M5 on Metal (macOS 26.6), `macos-clang-development`.
Vulkan was not run: no Linux or Vulkan host was available.

- [x] `scene_shader_layout.h`: `LUDUS_SCENE_LINE_INTERVAL_CAPACITY` and
      `LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE` with their units and purpose.
      Mirror them in `scene_shader.hpp` with Doxygen comments, and in
      `scene_shader.slang`.
      *32 intervals and 1e-6 of the segment, as `SCENE_LINE_INTERVAL_CAPACITY`
      and `SCENE_LINE_INTERVAL_MERGE_TOLERANCE` in C++ and Slang. The installed
      SDK consumer `static_assert`s the C++ capacity against the macro.*
- [x] `ray_line_visibility.slang`: the D2 clip, with canonical edge order (D2
      step 2), and the D3 interval set over a linear scan, with alpha-tested
      candidates reported, not clipped (D4).
      Attribution comments for Billen and Dutré (2016) and Salesin and Jarosz
      (2019), with the departures in design section 2.
      *The occluded set lives in `LineVisibility.Intervals` and is complemented
      in place. A separate occluded array lost its stores on Metal in the
      template's merged kernel at capacity 32 (design D3, "PL1 storage").*
- [x] `scene_shader.slang`: `LineSegment`, `LineVisibility` and
      `FindVisibleLineIntervals`. Budget accounting, the PL03 offsets and index
      exclusions, failure latching, zeroed output on failure.
      *Also `ClearLineVisibility`, a field-by-field reset that replaces
      `(LineVisibility)0` for the same Metal problem. `ray_triangle_loop.slang`
      gained `#pragma once` so the query can use
      `TRIANGLE_PARALLEL_COSINE_TOLERANCE` in `NativeQuery`-only passes.*
- [x] `RayQueryContext`: `LineQueries` and `MaximumLineQueries`.
- [x] Builder (D9):
  - [x] optional `line_queries` in `scene_interface`, with
        `MIN_LINE_QUERIES`/`MAX_LINE_QUERIES` in `SCENE_QUERY_LIMITS`;
  - [x] wrapper substitution of `MAX_LINE_QUERIES`;
  - [x] Python tests: an absent field means 0, the bounds are rejected with
        repairs, and a negative or nonintegral value is rejected.
  *`test_line_query_budget_is_optional_bounded_and_integral` rejects -1, 9,
  1.5, `true`, `"1"` and `null` with a `Capacity` diagnostic naming `[0,8]`.
  `test_line_query_budget_reaches_the_wrapper_only_when_nonzero` shows that an
  absent or zero budget gives identical wrapper bytes with no assignment. The
  render-graph Python suites ran 400 tests: 399 pass.
  `test_real_target_entries_layouts_determinism_and_generated_cpp` fails the
  same way on unmodified `develop` with the local tools (a compiler diagnostic
  now reports a column).*
- [x] Compatibility (PL07). These build byte-identical plans, wrappers and
      Metal artifacts before and after:
  - fixtures under `tests/sdk_consumer/rg2/`: `ordered_ray`,
    `visibility_sequence`, `compute_split`, `compute_mixed`, `scene_compute`,
    `veach_guibas_reference`;
  - templates under `render_graphs/`: `compute_ray_tracing`,
    `simple_compute_ray_tracing`, `veach_guibas_mis`, `restir_di`,
    `visibility_secondary_rays`, `tiled_vbuffer_nee`.

  Plan `VERSION` and `PACKAGE_VERSION` are unchanged. If either must change,
  stop and record why in design D9.
  *Cooked with `develop` `52ed0f97` and with this branch: all 12 give identical
  plans, wrappers, Metal artifacts, stage descriptors and generated headers.
  Each `.lurgrt` has the same size and differs only in its 32-byte generation
  identity (bytes 73-104), a hash over the helper sources' provenance, which any
  `scene_shader.slang` change moves. Slang drops the unused line fields of
  `RayQueryContext` from the emitted MSL. No version changed.*
- [x] GPU tests in `render_graph_ray_tests.cpp`, through a `TriangleLoop`
      fixture under `tests/sdk_consumer/rg2/line_visibility/`. Each case
      compares intervals with analytic values within a stated tolerance:
  - [x] an unoccluded segment returns `SCENE_QUERY_MISS` and one interval
        `[0, 1]`;
  - [x] one occluder, and two overlapping occluders that merge;
  - [x] an occluder mesh with abutting triangles leaves no crack;
  - [x] a coplanar occluder does not occlude;
  - [x] occluders behind the receiver and beyond the segment do not occlude;
  - [x] the receiver and carrying triangles are excluded;
  - [ ] a masked occluder sets `HasMaskedOccluders` and removes no length, and
        an opaque scene never sets it;
  - [x] a fully occluded segment returns `SCENE_QUERY_SURFACE` with zero
        intervals;
  - [x] capacity overflow, budget exhaustion and a degenerate segment latch
        failure and produce the magenta pixel.
  *"Line visibility queries match analytic intervals through a triangle-loop
  scene compute pass": tolerance 1e-6 of the segment, largest measured error
  9.6e-8. Also a full-capacity case (31 slats, 32 visible intervals) passes and
  32 slats overflow; a receiver in the segment's line fails too. The masked box
  stays open: render-graph scenes cannot hold alpha-tested triangles
  (`SceneShaderMaterial` has no alpha mode and non-opaque items are rejected),
  so only "an opaque scene never sets it" is tested (design D4, PL1 finding).*
- [x] Run the same fixture under a `NativeQuery` pass on Metal. It must give the
      same intervals, since line traversal is software in both (PL06).
      *Every case is byte-identical between the `Loop` and `Native`
      configurations.*
- [x] SDK consumer: cook the fixture in the installed renderer consumer and pass
      against a local `macos-clang-development` SDK install.
      *`ludus_sdk_scene_shader_contract_consumer` cooks
      `rg2_line_visibility_loop` through the installed helper (which now ships
      `ray_line_visibility.slang`) and checks the installed constants; it passed
      against a local `macos-clang-development` SDK install.*
- [x] `line-sampling` template (design D8a):
  - [x] `render_graphs/line_sampling/` graphs, manifest and `main_shaders/`,
        with `primary`, `shade` and `shadeVisibleFraction`, all `TriangleLoop`;
  - [x] `main_shaders/line_sampling.slang`: the D6 strategy for one direction.
        PL2 extends this file rather than copying it, if the template layout
        allows sharing; otherwise PL2 copies it and records why;
        *Its chord, proposal and estimate functions take their random
        dimensions as arguments, so PL2 can add direction `V` without changing
        PL1's layout. Template directories import only their own include
        closure, so PL2 will likely need a copy.*
  - [x] registration (design D8c): `TEMPLATES`, CLI choices, graph and
        snippet registry entries, embedding list,
        `PrepareLineSamplingConfiguration`, installed SDK template files;
        *`PrepareLineSamplingConfiguration` is in
        `render_graph_line_sampling_template.cpp` and shares
        `PrepareSceneComputeTemplateConfiguration` with the compute templates.
        Imports raise `line_queries` only when the template declares it.*
  - [x] registry requirement `compute-ray-tracing` with its label and registry
        tests (design D8c);
        *Label "Metal or Vulkan compute". `TEMPLATES` now records each
        template's registry requirement, and the CLI repair text follows it.
        `simple-compute-ray-tracing` still declares `native-ray-tracing`; that
        correction is a separate change.*
  - [x] Python tests: the template builds, declares the D8a budgets and is a
        `ComputeRayTracing` package; `render-graph new --template
        line-sampling` succeeds for `metal` and `spirv`, and for `wgsl` alone
        fails with a `Capability` diagnostic naming Metal or Vulkan compute
        (design D8c);
  - [x] editor tests: New Render Graph → Template, Quick Add and Apply
        Configuration Template create the configuration;
        *In `ludus_render_graph_configuration_tests`: the New Render Graph
        picker and the Quick Add snippet each run with Properties hidden, with
        undo, redo, save and validation; a document-level import shows that
        `line_queries` rises to 1 and keeps a larger authored value. The
        registry tests cover the new entries and the requirement label.*
  - [x] GPU tests: `color` converges within 2% at 256 samples per pixel to
        `veach-guibas-mis` `lightSampling` on the same scene;
        `visibleFraction` is 1 in a lit region and 0 in umbra.
        *Compared with the Veach and Guibas template cooked with every pass on
        the triangle loop (`rg2_veach_guibas_mis_triangle_loop`), so no native
        ray profile is needed. Means: line sampling 0.20376, 0.20380, 0.18602;
        light sampling 0.20372, 0.20375, 0.18599 (within 0.03%). The lit floor
        decodes to 1.01 (one ToneMap byte step) and the umbra pixels to
        exactly 0. Before the fix recorded in design D3 ("PL1 storage"), Metal
        dropped the intervals and this check caught a 2.6% bias.*
- [x] Documentation: the guide's "Author scene-aware ray shaders" section gains
      the line query, bounds, alpha policy and budget. A new "Sample lights
      along lines" section documents the `line-sampling` template like the MIS
      template section, including that `visibleFraction` is not radiance.
      *"Find the visible parts of a segment" and "Sample lights along lines"
      in `docs/development/render-graph-authoring.md`, with the
      `ClearLineVisibility` note.*
- [ ] Gates: build warning-clean, unit tests, ASan/UBSan,
      `./scripts/check --format` and `--tidy`, `source_policy.py --base` with
      the real merge base and `--clang-format`, `check-docs`, strict MkDocs and
      `check-wiki --documentation-only`.
      *Run on the branch merged with `develop` `4011adfc`:
      `macos-clang-development` builds with no compiler warnings; `--format`,
      `--tidy-changed` (base `4011adfc`) and `source_policy.py --base 4011adfc
      --clang-format` pass. The full targeted `--tidy` fails in 188 translation
      units with 301 findings, all on lines this change does not touch except one
      `bugprone-easily-swappable-parameters` on a new test helper, now
      suppressed with a reason as in the neighboring ray tests; `check-docs`, strict MkDocs and the
      `test_docs.py`/`test_wiki.py` suites pass. `macos-clang-asan-ubsan`
      (tests on, editor off) passes 107/107 tests, but that preset runs neither
      the GPU ray-pixel suite nor the editor. Left open because three
      `macos-clang-development` suites fail, in code this change does not touch,
      after `develop`'s native Metal ray-stage merge: `ludus_rhi_ray_queries`
      (expects `RayPipelines` false on Metal), one legacy-package case in
      `ludus_render_graph_ray_pixels` (`storageGeneration.Decode`), and five
      `renderer_workspace_tests` cases in `ludus_editor_tests` whose fake SDK
      lacks `compile_metal_ray.py`. `scripts/build-api --bootstrap` (pinned
      Doxygen, baseline `4011adfc`) reports no new gaps, and
      `check-wiki --documentation-only` then passes.
      Ruff was not run (not installed locally). Vulkan was not run.*

## PL2 Point-and-line MIS template

Evidence is from an Apple M5 on Metal (macOS 26.6), `macos-clang-development`,
on the branch merged with `develop` `221f7621` (rechecked on `ba249bff`).
Vulkan was not run: no Linux or Vulkan host was available, so the llvmpipe run
is left to batch CI.

- [x] `render_graphs/salesin_jarosz_lines/`: library and main graphs, manifest
      and `main_shaders/` (D8b), all passes `TriangleLoop`, Graph settings
      `POINT_LINE_MIS_HEURISTIC` and `POINT_LINE_MIS_POWER_EXPONENT`.
      *Library configuration `PointLineMis` with `primary`, `shade`,
      `shadeLines` and `shadePoints` over one 32-byte hit Buffer; outputs
      `color`, `lineSampling` and `pointSampling`, each with its own Accumulate,
      ToneMap and Export. `POINT_LINE_MIS_HEURISTIC` defaults to `0` (balance) with range
      `0`-`1`; `POINT_LINE_MIS_POWER_EXPONENT` defaults to 2.*
- [x] `main_shaders/line_sampling.slang`: PL1's single-direction strategy
      extended to both directions, plus the D7 density evaluation at a given
      point.
      *A copy of PL1's file, because a template directory imports only its own
      include closure. `ProposeLine` takes `LINE_DIRECTION_U` or
      `LINE_DIRECTION_V`; `EvaluateLineDensity` is D7. Both run their query in
      a `[noinline]` function that copies `RayQueryContext` in and out: inlined,
      the ten line queries made the merged Metal stage 2.10 MB, over the 2 MiB
      artifact limit, and Slang inlines `[noinline]` functions with `inout`
      parameters (design D8b, "PL2 notes"). The stage is now 1.09 MB.*
- [x] `main_shaders/multiple_importance.slang`: the three-strategy weight for
      balance and power, with the Veach and Guibas attribution.
      *`ComputeMisWeight(sampledDensity, otherDensity0, otherDensity1)`, with
      the Veach and Guibas and the Salesin and Jarosz attributions. A zero
      other density drops that strategy, and `pow` is skipped for it because
      Metal compiles with fast math.*
- [x] Shading passes `shade`, `shadeLines` and `shadePoints` with the D8 random
      layout and budgets.
      *Dimensions 2-6 line `U`, 7-11 line `V`, 12-14 BSDF; `shadePoints` draws
      its point from 2-5. The scene interface declares surface 1, visibility 2
      and line 6. A non-mesh light keeps the point proposal in both line
      strategies, which then share it equally.*
- [x] Registration (design D8c): `TEMPLATES`, CLI choices, graph and snippet
      registry entries declaring `compute-ray-tracing`, embedding list,
      `PreparePointLineConfiguration`, installed SDK template files.
      *`PreparePointLineConfiguration` is in
      `render_graph_point_line_template.cpp` and imports `PointLine`-prefixed
      programs through `PrepareSceneComputeTemplateConfiguration`. The
      registry tests list both new entries and their requirement.*
- [x] Python tests: the template builds, has four passes, declares the D8b
      budgets, rejects a heuristic outside `0`-`1`, and is a
      `ComputeRayTracing` package; `render-graph new --template
      salesin-jarosz-lines` succeeds for `metal` and `spirv`.
      *`SalesinJaroszLinesTemplateTests` (7 tests) also checks the random
      layout, the attributions, a `wgsl`-only `Capability` diagnostic and the
      relocated SDK copy. It rejects heuristic values -1, 2, 3, 0.5, `true` and
      `null`. The render-graph Python suites, with the pinned Slang, helper and
      SPIRV-Cross set so that the compiler acceptance tests run, ran 429 tests:
      428 pass. `test_real_target_entries_layouts_determinism_and_generated_cpp`
      fails as PL1 recorded on unmodified `develop` (a compiler diagnostic now
      reports a column); this change does not touch its code.*
- [x] GPU tests (`TriangleLoop`, Metal; Vulkan through llvmpipe in batch CI):
  - [x] three-strategy weights sum to one within float tolerance over sampled
        densities, including zero and delta cases;
        *`point_line_densities` fixture, which compiles the template's own
        `line_sampling.slang` and `multiple_importance.slang`, under both
        heuristics. Over every triple of 0, 1e-20, 1e-6, 1/32, 1/2, 1, 37 and
        1e12 the largest error is 1.2e-7 (tolerance 2e-6). The delta case
        `ComputeMisWeight(p, p, 0)` gives 1/2, and the single weights 1, 0 and
        1/3 are exact within 3e-8.*
  - [x] with no occluder, a line strategy's density equals
        `EvaluateLightPdfSolidAngle` for the same point;
        *1305 receivers per direction in the room without its block: largest
        relative difference 9.2e-7 (`U`) and 9.6e-7 (`V`), tolerance 1e-5.
        Pixel-center rays first landed on floor-wall seams, where the query
        clips the grazing wall (design D8b, "PL2 seam finding"). The fixture
        now traces off-center rays.*
  - [x] `color`, `lineSampling` and `pointSampling` converge to one mean within
        2% at 256 samples per pixel, in a Cornell-style room with an occluder
        casting a penumbra from a two-triangle quad light, under both
        heuristics;
        *Red channel means at 256 samples. Balance: `color` 0.20378,
        `lineSampling` 0.20376, `pointSampling` 0.20373. Power: 0.20376,
        0.20376, 0.20371. The Veach and Guibas triangle-loop light-sampling
        image gives 0.20372. All channels agree within 0.03%.*
  - [x] after 16 samples per pixel, `color` has lower mean squared error than
        `pointSampling` in the penumbra region, against an independently seeded
        1024-sample reference. Timing is reported beside it;
        *The penumbra is the 215 pixels below eye height whose mean visible
        chord fraction from the line-sampling template lies in (0.1, 0.9). MSE:
        `color` 3.05e-4, `lineSampling` 3.06e-4, `pointSampling` 6.93e-4. Mean
        GPU time per frame at 96×64 over 16 frames: `primary` 0.05 ms, `shade`
        0.77 ms, `shadeLines` 0.58 ms, `shadePoints` 0.12 ms, on a heavily
        loaded machine. This is an equal-sample comparison and supports no speed
        claim. Pixels at the light's edge are excluded: they mix emission 12
        with the dark ceiling, and one tone-mapped byte there decodes to a
        large radiance.*
  - [ ] a masked occluder scene converges to the `pointSampling` mean;
        *Not run: render-graph scenes still cannot hold alpha-tested
        triangles (`SceneShaderMaterial` has no alpha mode and non-opaque items
        are rejected), as PL1 found (design D4).*
  - [x] an analytic point light and a constant environment converge to the
        same mean in all three outputs (non-mesh fallback, D6).
        *The room with the ceiling light's emission removed, a 2 cd point light
        and a 0.5 sky. Red channel means, balance: 0.38617, 0.38620, 0.38615;
        power: 0.38616, 0.38620, 0.38619; Veach and Guibas light sampling
        0.38612.*
- [x] Editor tests: New Render Graph → Template, Quick Add and Apply
      Configuration Template create the configuration, and the Graph settings
      edit and save through the canvas's **Edit Graph Settings...** action with
      Properties hidden, as well as through Properties.
      *Four tests in `render_graph_template_tests.cpp`, all passing. With
      Properties hidden, the picker creates the graph and the canvas
      **Edit Graph Settings...** dialog sets `POINT_LINE_MIS_HEURISTIC` to 1, with undo,
      redo, save, validation and a reload from disk. Through Properties,
      `POINT_LINE_MIS_HEURISTIC` and `POINT_LINE_MIS_POWER_EXPONENT` are edited and saved. Quick Add
      and the canvas **Configurations → Apply Configuration Template...**
      import `PointLineMis` with Properties hidden.
      `ludus_render_graph_configuration_tests` passes all 214 test cases.*
- [x] SDK consumer fixture for the template.
      *`ludus_sdk_scene_shader_contract_consumer` copies the installed
      `templates/salesin_jarosz_lines`, cooks it through the installed helper
      and checks that its four passes form a ray-tracing plan that needs no
      hardware ray tracing. It passed against a local `macos-clang-development`
      SDK install. `scripts/install-sdk` runs only the base consumer, so the
      renderer consumer was configured against the install with the pinned
      Slang and SPIRV-Cross.*
- [x] Documentation: a "Combine point and line samples" section after "Compare
      light and BSDF sampling with MIS", with the passes, outputs, settings,
      evidence, scope limits and attribution.
      *Placed after "Sample lights along lines", which it builds on and which
      itself follows the MIS section. PL1's scope paragraph now links to it.*
- [ ] Gates as in PL1.
      *Run on the branch merged with `develop` `221f7621`:
      `macos-clang-development` builds with no compiler warnings (only the
      linker's notes that Homebrew Qt targets macOS 14.4). `--format`,
      `--tidy-changed` (base `221f7621`) and `source_policy.py --base 221f7621
      --clang-format` pass. Targeted `--tidy` on the five changed translation
      units reports findings only on lines this change does not touch, after
      one new file-scope `QString` in the editor tests became a function.
      `check-docs`, strict MkDocs, `scripts/build-api --bootstrap` (no new
      gaps), `check-wiki --documentation-only` and the
      `test_docs.py`/`test_wiki.py` suites pass. `macos-clang-asan-ubsan`
      passes 120 of 122 tests with no sanitizer report. That includes
      `ludus_render_graph_ray_pixels`, which runs the PL2 GPU tests.
      Left open because `develop` fails in code this change does not touch.
      `macos-clang-development` passes 138 of 142:
      `ludus_rhi_ray_queries` expects `RayPipelines` false on Metal (as in
      PL1, also under ASan); `ludus_renderer_pixels` fails
      `projected_solid_angle_parity_tests.cpp:542`, the
      `ris-projected-solid-angle` work from #723 (also under ASan);
      `ludus_editor_tests` fails five cases (asynchronous activation failure,
      ray path capture without native rays, Build & Preview of an edited
      default, New Graph cooking with the project SDK, and the shell inspector's
      horizontal scroll); and `ludus_render_graph_configuration_tests` fails
      the Project Settings resolution case. Those six editor cases fail the same
      way in a build of plain `develop` `221f7621`. A seventh,
      the activity chip's rate limit, failed only under load and passes alone.
      After a final merge of `develop` `ba249bff` the tree builds with no
      compiler warnings and the PL1 and PL2 ray tests and editor tests pass
      again. #725 fixed the inspector case. #727 changed the built-in default
      graph's program count, and three budget cases in
      `ludus_render_graph_configuration_tests` now fail on `develop`'s own
      expectations (`needs 8 shader programs`, 6 and 11 programs). This change
      does not touch those lines. The full suites above were not rerun on that
      merge. Ruff was not run (not installed locally). Vulkan was not run.*

## PL3 BVH line traversal

Design D5 was revised before implementation (2026-10-29): the line walk shares
`ComputeBvh`'s node reader, leaf order and stack bound (#676), and
`scene_interface.line_traversal` selects the BVH (default) or the loop.
Evidence is from an Apple M5 on Metal (macOS 26.6), `macos-clang-development`,
on `develop` `c08ffec7`. Vulkan was not run: no Linux or Vulkan host was
available, so llvmpipe is left to batch CI.

- [x] `ray_bvh_nodes.slang`: `LoadBvhNode`, `GetBvhLeafTriangle` (order word Z),
      `BVH_NODE_WORDS` and `BVH_MAXIMUM_PENDING`, used by `TraceBvh` and the
      line walk. Register it with the helper sources and the SDK install. The
      `ComputeBvh` and RT1 parity tests still pass.
      *Registered in `render_graph_ray.py` `HELPERS`, the `LudusRenderGraph.cmake`
      helper dependencies and the renderer's shader install list. `TraceBvh`
      changed only in how it reads nodes. "Compute BVH traversal reports the same
      hits and visibility as the other strategies", the ComputeBvh template test
      and the GPU-compute RT1 reference tests pass (the last in
      `ludus_renderer_pixels`, see the gates below).*
- [x] `ray_line_visibility.slang`: the BVH walk with the Akenine-Möller
      separating-axis node test and its conservative margin (D5), with
      attribution; stack exhaustion and a pack without nodes fail; both
      traversals stop at full occlusion and clear `HasMaskedOccluders` when no
      length is visible.
      *`TraceLineVisibility` replaces `TraceLineTriangleLoop`. The scan and the
      walk share one candidate body (`ClipLineCandidate`, then the PL1 insertion)
      and one in-place complement, so the interval array is still the only one
      (D3, "PL1 storage"). The attribution cites JGT 6(1), pp. 29-33, DOI
      10.1080/10867651.2001.10487535. Stack exhaustion has no GPU test: the
      builder's 48-level depth limit keeps every pack the public API can produce
      within the 64-entry stack (D5).*
- [ ] Leaf order. Leaves address order word Z, which keeps the BVH order in
      plans with a `NativeQuery` pass (no packing change, D5). Add a
      masked-scene parity test under both strategies once scenes can hold
      alpha-tested triangles (D4).
      *Word Z is read through `GetBvhLeafTriangle`, as `TraceBvh` reads it, and
      the `NativeQuery` parity below passes on opaque scenes. Left open because
      the masked-scene test still cannot be written: render-graph scenes cannot
      hold alpha-tested triangles (D4, PL1 finding).*
- [x] Builder (D9): optional `line_traversal`, `"ComputeBvh"` or
      `"TriangleLoop"`, with Python tests for the default, the rejection and
      its repair, and identical wrapper bytes for an absent field and
      `"ComputeBvh"`.
      *`test_line_traversal_is_optional_and_names_its_choices` rejects
      `"NativeQuery"`, `"Custom"`, `"computebvh"`, `""`, `1`, `true` and `null`
      with a `Schema` diagnostic naming the field and both values.
      `test_line_traversal_defines_the_scan_only_for_triangle_loop` shows that an
      absent field and `"ComputeBvh"` give identical wrappers and that
      `"TriangleLoop"` adds only the one define, ahead of the helper includes.*
- [x] Parity: the PL1 analytic fixture gives byte-identical interval readouts
      with the loop and the BVH, in `TriangleLoop` and `NativeQuery` passes.
      *"Line visibility through the packed BVH equals the linear scan in
      triangle-loop and native passes" cooks the fixture again with
      `line_traversal` `"TriangleLoop"` and compares four packages. Every set
      first passes the PL1 analytic checks; then all eight cases, including 31
      and 32 slats (62 and 64 occluder triangles in a multi-level tree) and the
      overflow, read back identically. The readout is the PL1 one: status, count
      and flag bytes and 24-bit fixed-point bounds.*
- [x] Both templates give byte-identical accumulated images on their PL1 and PL2
      GPU fixtures with `line_traversal` `"TriangleLoop"` and the default BVH,
      since the BVH changes only which triangles are visited. Their merged
      kernels still pass the PL1 and PL2 convergence tests with the BVH stack
      added (D3, "PL1 storage").
      *Two `[line-bvh]` template tests compare the Cornell room's tone-mapped
      RGBA8 running means after 1 and 64 samples: both line-sampling outputs and
      all three point-and-line outputs, including the BSDF and point samples, are
      identical. With the BVH as default, the PL1 and PL2 convergence, density
      and fallback tests pass (9 test cases), and the PL2 penumbra errors repeat
      PL2's to the printed digits: 3.04899e-4, 3.06474e-4 and 6.93375e-4. Each
      BVH kernel now holds a 64-entry `uint` stack beside the 32 `float2`
      intervals. The merged point-and-line Metal stage is 1,123,179 bytes, against
      1,099,515 with the scan (2 MiB limit); line sampling's is 583,264 against
      547,084.*
- [x] Measure line query time against the loop on a larger reference scene,
      such as Sponza, and record it with the device, with an interval parity
      check on that scene. Make no speed claim beyond the measurement.
      *Hidden test `[.line-traversal-performance]`. It reads a scene exported to
      a raw position and index file, named by `LUDUS_LINE_TRAVERSAL_SCENE`, and
      adds a 2 m square light facing down (emission 50). The line-sampling
      template runs with every ray pass on `ComputeBvh`, so the two runs differ
      only in `line_traversal`. Scene: the first 500,000 triangles of Intel's
      `NewSponza_Main_glTF_003.gltf` (whole primitives in node order; 274,307
      vertices), exported by
      `modules/graphics/renderer/tests/export_line_traversal_scene.py
      --maximum-triangles 500000`. The full 3,747,018 triangles, and exports capped at 2,000,000
      and 1,000,000, fail scene preparation with `CapacityExceeded`; the cause
      was not traced. 96×64 pixels, 8 frames, two runs. Primary pass 0.24 ms
      either way. `shade`, one line query per pixel: BVH 2.24 and 2.24 ms, scan
      452.3 and 426.6 ms. `shadeVisibleFraction`: BVH 3.59 and 3.56 ms, scan
      327.6 and 324.5 ms. Both outputs are byte-identical between the
      traversals, with no failure pixels; 1,239 of 6,144 `color` pixels and 3,894
      `visibleFraction` pixels are nonzero. On the 14-triangle Cornell room the
      BVH `shade` pass was 28% and 13% slower than the scan in two runs
      (1.02 against 0.80 ms, 0.29 against 0.26 ms).*
- [x] Re-run PL2's penumbra comparison of `color` against `pointSampling` at
      equal GPU time rather than equal samples, and record it as a measurement.
      *Hidden test `[.point-line-performance]`, same room, mask and reference as
      PL2. Each estimator alone needs the primary pass and its own shading pass
      per sample. Over 64 timed frames a `color` sample cost 6.62 and 6.37 times
      a `pointSampling` sample in two runs (GPU clocks differed 3.5× between
      them), so point sampling got 106 and 102 samples against 16. Penumbra MSE:
      `color` at 16 samples 3.05e-4; `pointSampling` at 106 and 102 samples
      9.93e-5 and 9.91e-5 (6.93e-4 at 16). At equal GPU time on this 14-triangle
      room, point sampling has about a third of the error. This measures one small
      scene and supports no general claim either way.*
- [ ] Documentation and gates as in PL1.
      *Documentation: the guide's "Find the visible parts of a segment" now
      describes the BVH walk, `line_traversal`, the attribution and the
      measurements. The manifest record, both template sections and the
      `ComputeBvh` section were updated too.
      Gates, on `develop` `c08ffec7` (no later `develop` commits): `macos-clang-development`
      builds all targets with no compiler warnings (only the linker's notes that
      Homebrew Qt targets macOS 14.4). `--format`, `--tidy-changed` (base
      `c08ffec7`) and `source_policy.py --base c08ffec7 --clang-format` pass; the
      targeted `--tidy` on the changed test file reports findings only on lines
      this change does not touch. The render-graph Python suites pass 434 tests
      (19 compiler-acceptance tests skipped without the tool variables). With
      `LUDUS_RG1_SLANG`, `LUDUS_RG1_HELPER` and `LUDUS_RG1_CROSS` set to the
      pinned tools, about twenty acceptance tests error with "vertex inputs
      differ from schema" in this checkout and identically in a `c08ffec7`
      snapshot, so they are not this change's; the CMake cooks with the same
      Slang build every fixture and template. `check-docs`, strict MkDocs,
      `scripts/build-api --bootstrap` (no new gaps), `check-wiki
      --documentation-only` and the `test_docs.py`/`test_wiki.py` suites pass.
      `install-sdk` passes, and the renderer consumer, configured by hand against
      the install with the pinned Slang and SPIRV-Cross, cooks
      `rg2_line_visibility_loop` and `salesin_jarosz_lines` through the installed
      helper (which ships `ray_bvh_nodes.slang`) and its contract executable
      passes. `macos-clang-asan-ubsan` (tests on, editor off, configured with the
      shader tools) passes 121 of 122 with no sanitizer report, including
      `ludus_render_graph_ray_pixels` and `ludus_renderer_pixels`.
      Left open because `develop` fails in code this change does not touch.
      `ludus_rhi_ray_queries` expects `RayPipelines` false on Metal (both presets,
      as in PL1 and PL2). `macos-clang-development` CTest passes 139 of 142.
      `ludus_render_graph_configuration_tests` fails 4 of 222 cases: those on
      `develop`'s own program counts after #727 ("needs 8 shader programs", 7
      against 6, 12 against 11) and the Project Settings resolution case. `ludus_editor_tests`
      fails 13 cases (preview presentation timeouts, engine default,
      Build & Preview, New Graph and project-SDK cooking, the workspace's fake
      SDK, ray path capture without native rays). The same 13 fail with this
      change's product files reverted to `c08ffec7` and rebuilt, with the machine
      idle. No open pull request fixes them. Ruff was not run (not installed
      locally). Vulkan was not run.*

## Large-scene equal-time measurement (after PL3)

PL3's equal-time comparison ran only on the 14-triangle room. Record results in
[design section 5](design.md#5-large-scene-equal-time-measurement). Evidence is
from an Apple M5 on Metal (macOS 26.6), `macos-clang-development`, measured on
`develop` `d00cd9eb`. Vulkan was not run: no Linux or Vulkan host was available.

- [x] Measure `salesin-jarosz-lines`' `color` against `pointSampling` at equal
      GPU time on at least 500,000 triangles with real occluders and a large
      area light. Use PL2's penumbra mask and an independently seeded reference
      whose noise is negligible. Record measurements only.
      *Hidden test `[.point-line-large-scene-performance]`. It runs on the
      500,000-triangle Sponza export named by `LUDUS_LINE_TRAVERSAL_SCENE`, lit
      by a 6 m square light, with both line templates cooked with every ray pass
      on `ComputeBvh` and a 2 GiB device budget. Penumbra: 1,278 pixels with
      line-sampling visible fraction in (0.1, 0.9) below eye height. Outside:
      1,794 other lower-half pixels. Reference: 8 independently seeded `color`
      images of 4,096 samples each (32,768 per pixel, since accumulation stops at
      4,096). Its noise is 1.45e-6 in the penumbra and 2.52e-7 outside, 3-5% of
      the smallest MSE. Two runs on `d00cd9eb`: a `color` sample cost 15.63 and
      17.17 ms, a `pointSampling` sample 0.525 and 0.755 ms (ratio 29.8 and
      22.7). Point sampling therefore got 477 and 364 samples against 16, and
      1,906 and 1,455 against 64. Penumbra MSE: `color` 2.73e-3 at 16 samples and 6.62e-4 at 64.
      `pointSampling` at equal time: 1.94e-4 and 2.41e-4, then 4.42e-5 and
      5.62e-5 (ratios 0.067-0.088). Outside: ratios 0.066-0.104. A third run
      after merging `develop` `f3bdd943` (#745) gave ratio 28.6 and point/`color`
      MSE ratios 0.068-0.088, with the reference and `color` errors bit-identical. At equal samples
      `color` has 0.55-0.58 times `pointSampling`'s penumbra MSE. Across eight
      timed runs that day, the primary pass varied 2.7× and the cost ratio ranged
      from 17.7 to 30.2. Line queries failed on 12 `color` and 12 `lineSampling`
      pixels, summed over the eight 4,096-sample references; the cause was not
      traced. Those pixels leave the masks. One scene on one device; no general
      claim.*
- [x] Gates for a change to a test, its CMake staging and documentation.
      *On `develop` `f3bdd943` merged: `macos-clang-development` and
      `macos-clang-asan-ubsan` build `ludus_render_graph_ray_tests` with no
      compiler warnings. The `[point-line]`, `[line-bvh]`, `[line-sampling]` and
      `[scene-capacity]` cases (9 cases, 14,397 assertions) pass under both, with
      no sanitizer report. `--format`, `source_policy.py --base f3bdd943
      --clang-format` and `--tidy-changed --base f3bdd943` pass. `check-docs`,
      strict MkDocs, `check-wiki --documentation-only` and the
      `test_docs.py`/`test_wiki.py` suites pass. The full CTest suites,
      `install-sdk` and Ruff were not run: no product, SDK or shader source
      changed, and Ruff is not installed locally. Vulkan was not run.*

## PL4 Deferred (not scheduled)

Each needs its own spec revision before work starts:

- [ ] Discontinuity-smoothing balance heuristic (Salesin and Jarosz section
      4.3), after a stratified scene sampler exists.
- [ ] Solid-angle line sampling for parallelogram emitters (Ureña et al. 2013).
- [ ] Chords across coplanar emitter triangles.
- [ ] Line queries in ray path capture.
- [ ] Line sampling in the RT1 reference path tracer, in the CPU oracle and the
      GPU-compute reference together, with parity evidence (AGENTS.md hard rule).
