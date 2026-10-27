# Point and line direct lighting implementation tasks

All boxes start unchecked. Read [requirements](requirements.md) and
[design](design.md). Each phase is one reviewable pull request against
`develop` from its own `codex/` branch and worktree. Update evidence before
checking a task. Mocks, skipped checks or optionalized failures do not count as
acceptance. The GPU tests here use `TriangleLoop` and must not skip for a
missing native ray profile; report any other skip as a skip, with the device.

## PL0 Feasibility (no product change)

Record results in [design section 4](design.md#4-pl0-evidence-to-be-recorded).

- [ ] **Prototype query.** Write the D2/D3 linear-scan query as a throwaway
      kernel in a local fixture (not committed as product code). Run it on
      Metal against hand-computed intervals for one occluder, two overlapping
      occluders and a coplanar occluder.
- [ ] **Capacity.** Count visible intervals on the PL13 fixtures plus a
      dense-blinds fixture. Measure register use and occupancy for capacities
      8, 16 and 32 on Metal. Record the chosen
      `LUDUS_SCENE_LINE_INTERVAL_CAPACITY`.
- [ ] **Merge tolerance.** Find the smallest tolerance with no visible crack
      along shared edges of a tessellated sphere and a two-triangle quad
      occluder, and record the visible length it drops. Record the chosen
      `LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE`.
- [ ] **Packed nodes.** Confirm whether render-graph ray passes pack
      `ReferenceBvhNode` data under `NativeQuery` and `TriangleLoop`. If a
      strategy omits them, record what PL3 must add.
- [ ] **Random dimensions.** Confirm `RANDOM_MAX_DIMENSION` admits dimension 14
      (design D8). If not, revise the D8 layout before PL2.
- [ ] **Cost.** Time the prototype query per pixel against `TestVisibility` with
      `TriangleLoop` on the same scene. Record it as context, not as an
      acceptance threshold.

## PL1 Line visibility query and line-sampling template

- [ ] `scene_shader_layout.h`: `LUDUS_SCENE_LINE_INTERVAL_CAPACITY` and
      `LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE` with their units and purpose.
      Mirror them in `scene_shader.hpp` with Doxygen comments, and in
      `scene_shader.slang`.
- [ ] `ray_line_visibility.slang`: the D2 clip and the D3 interval set over a
      linear scan, with alpha-tested candidates reported, not clipped (D4).
      Attribution comments for Billen and Dutré (2016) and Salesin and Jarosz
      (2019), with the departures in design section 2.
- [ ] `scene_shader.slang`: `LineSegment`, `LineVisibility` and
      `FindVisibleLineIntervals`. Budget accounting, the PL03 offsets and index
      exclusions, failure latching, zeroed output on failure.
- [ ] `RayQueryContext`: `LineQueries` and `MaximumLineQueries`.
- [ ] Builder (D9):
  - [ ] optional `line_queries` in `scene_interface`, with
        `MIN_LINE_QUERIES`/`MAX_LINE_QUERIES` in `SCENE_QUERY_LIMITS`;
  - [ ] wrapper substitution of `MAX_LINE_QUERIES`;
  - [ ] Python tests: an absent field means 0, the bounds are rejected with
        repairs, and a negative or nonintegral value is rejected.
- [ ] Compatibility (PL07). These build byte-identical plans, wrappers and
      Metal artifacts before and after:
  - fixtures under `tests/sdk_consumer/rg2/`: `ordered_ray`,
    `visibility_sequence`, `compute_split`, `compute_mixed`, `scene_compute`,
    `veach_guibas_reference`;
  - templates under `render_graphs/`: `compute_ray_tracing`,
    `simple_compute_ray_tracing`, `veach_guibas_mis`, `restir_di`,
    `visibility_secondary_rays`, `tiled_vbuffer_nee`.

  Plan `VERSION` and `PACKAGE_VERSION` are unchanged. If either must change,
  stop and record why in design D9.
- [ ] GPU tests in `render_graph_ray_tests.cpp`, through a `TriangleLoop`
      fixture under `tests/sdk_consumer/rg2/line_visibility/`. Each case
      compares intervals with analytic values within a stated tolerance:
  - [ ] an unoccluded segment returns `SCENE_QUERY_MISS` and one interval
        `[0, 1]`;
  - [ ] one occluder, and two overlapping occluders that merge;
  - [ ] an occluder mesh with abutting triangles leaves no crack;
  - [ ] a coplanar occluder does not occlude;
  - [ ] occluders behind the receiver and beyond the segment do not occlude;
  - [ ] the receiver and carrying triangles are excluded;
  - [ ] a masked occluder sets `HasMaskedOccluders` and removes no length, and
        an opaque scene never sets it;
  - [ ] a fully occluded segment returns `SCENE_QUERY_SURFACE` with zero
        intervals;
  - [ ] capacity overflow, budget exhaustion and a degenerate segment latch
        failure and produce the magenta pixel.
- [ ] Run the same fixture under a `NativeQuery` pass on Metal. It must give the
      same intervals, since line traversal is software in both (PL06).
- [ ] SDK consumer: cook the fixture in the installed renderer consumer and pass
      against a local `macos-clang-development` SDK install.
- [ ] `line-sampling` template (design D8a):
  - [ ] `render_graphs/line_sampling/` graphs, manifest and `main_shaders/`,
        with `primary`, `shade` and `shadeVisibleFraction`, all `TriangleLoop`;
  - [ ] `main_shaders/line_sampling.slang`: the D6 strategy for one direction.
        PL2 extends this file rather than copying it, if the template layout
        allows sharing; otherwise PL2 copies it and records why;
  - [ ] registration (design D8c): `TEMPLATES`, CLI choices, graph and
        snippet registry entries, embedding list,
        `PrepareLineSamplingConfiguration`, installed SDK template files;
  - [ ] registry requirement `compute-ray-tracing` with its label and registry
        tests (design D8c);
  - [ ] Python tests: the template builds, declares the D8a budgets and is a
        `ComputeRayTracing` package; `render-graph new --template
        line-sampling` succeeds for `metal` and `spirv`, and for `wgsl` alone
        fails with a `Capability` diagnostic naming Metal or Vulkan compute
        (design D8c);
  - [ ] editor tests: New Render Graph → Template, Quick Add and Apply
        Configuration Template create the configuration;
  - [ ] GPU tests: `color` converges within 2% at 256 samples per pixel to
        `veach-guibas-mis` `lightSampling` on the same scene;
        `visibleFraction` is 1 in a lit region and 0 in umbra.
- [ ] Documentation: the guide's "Author scene-aware ray shaders" section gains
      the line query, bounds, alpha policy and budget. A new "Sample lights
      along lines" section documents the `line-sampling` template like the MIS
      template section, including that `visibleFraction` is not radiance.
- [ ] Gates: build warning-clean, unit tests, ASan/UBSan,
      `./scripts/check --format` and `--tidy`, `source_policy.py --base` with
      the real merge base and `--clang-format`, `check-docs`, strict MkDocs and
      `check-wiki --documentation-only`.

## PL2 Point-and-line MIS template

- [ ] `render_graphs/salesin_jarosz_lines/`: library and main graphs, manifest
      and `main_shaders/` (D8b), all passes `TriangleLoop`, Graph settings
      `MIS_HEURISTIC` and `MIS_POWER_EXPONENT`.
- [ ] `main_shaders/line_sampling.slang`: PL1's single-direction strategy
      extended to both directions, plus the D7 density evaluation at a given
      point.
- [ ] `main_shaders/multiple_importance.slang`: the three-strategy weight for
      balance and power, with the Veach and Guibas attribution.
- [ ] Shading passes `shade`, `shadeLines` and `shadePoints` with the D8 random
      layout and budgets.
- [ ] Registration (design D8c): `TEMPLATES`, CLI choices, graph and snippet
      registry entries declaring `compute-ray-tracing`, embedding list,
      `PreparePointLineConfiguration`, installed SDK template files.
- [ ] Python tests: the template builds, has four passes, declares the D8b
      budgets, rejects a heuristic outside `0`-`1`, and is a
      `ComputeRayTracing` package; `render-graph new --template
      salesin-jarosz-lines` succeeds for `metal` and `spirv`.
- [ ] GPU tests (`TriangleLoop`, Metal; Vulkan through llvmpipe in batch CI):
  - [ ] three-strategy weights sum to one within float tolerance over sampled
        densities, including zero and delta cases;
  - [ ] with no occluder, a line strategy's density equals
        `EvaluateLightPdfSolidAngle` for the same point;
  - [ ] `color`, `lineSampling` and `pointSampling` converge to one mean within
        2% at 256 samples per pixel, in a Cornell-style room with an occluder
        casting a penumbra from a two-triangle quad light, under both
        heuristics;
  - [ ] after 16 samples per pixel, `color` has lower mean squared error than
        `pointSampling` in the penumbra region, against an independently seeded
        1024-sample reference. Timing is reported beside it;
  - [ ] a masked occluder scene converges to the `pointSampling` mean;
  - [ ] an analytic point light and a constant environment converge to the
        same mean in all three outputs (non-mesh fallback, D6).
- [ ] Editor tests: New Render Graph → Template, Quick Add and Apply
      Configuration Template create the configuration, and the Graph settings
      edit and save through the canvas's **Edit Graph Settings...** action with
      Properties hidden, as well as through Properties.
- [ ] SDK consumer fixture for the template.
- [ ] Documentation: a "Combine point and line samples" section after "Compare
      light and BSDF sampling with MIS", with the passes, outputs, settings,
      evidence, scope limits and attribution.
- [ ] Gates as in PL1.

## PL3 BVH line traversal

- [ ] Traverse the packed `ReferenceBvhNode` array with a bounded stack and the
      Akenine-Möller triangle-box test (D5), with attribution. Stack exhaustion
      fails.
- [ ] Add packed nodes to any strategy PL0 found without them.
- [ ] Parity: every PL1 and PL2 non-overflow fixture gives identical intervals
      and byte-identical accumulated images with the loop and the BVH.
- [ ] Both templates keep byte-identical accumulated images on their PL1 and
      PL2 GPU fixtures, since the BVH changes only which triangles are visited.
- [ ] Measure line query time against the loop on a larger reference scene,
      such as Sponza, and record it with the device. Make no speed claim beyond
      the measurement.
- [ ] Documentation and gates as in PL1.

## PL4 Deferred (not scheduled)

Each needs its own spec revision before work starts:

- [ ] Discontinuity-smoothing balance heuristic (Salesin and Jarosz section
      4.3), after a stratified scene sampler exists.
- [ ] Solid-angle line sampling for parallelogram emitters (Ureña et al. 2013).
- [ ] Chords across coplanar emitter triangles.
- [ ] Line queries in ray path capture.
- [ ] Line sampling in the RT1 reference path tracer, in the CPU oracle and the
      GPU-compute reference together, with parity evidence (AGENTS.md hard rule).
