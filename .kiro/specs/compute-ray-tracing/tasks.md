# Compute ray tracing implementation tasks

All boxes start unchecked. Read [requirements](requirements.md) and
[design](design.md). Each phase is one reviewable pull request against
`develop` from its own `codex/` branch and worktree. Update evidence before
checking a task. Mocks, skipped checks or optionalized failures do not count as
acceptance. Native ray pixel tests need a qualified GPU; report a skip as a
skip.

## C0 Feasibility (no product change)

Evidence: [design section 3](design.md#3-phase-c0-evidence). Metal only; this
Mac has no Linux host.

- [x] **Triangle packing.** Confirm that two `GpuPreviewRecord` jobs built from
      one extraction pack triangles in the same order (`gpu_preview_ray.cpp`,
      `Order`). Record the evidence. If the order is not guaranteed, switch D5
      to instance + primitive identity before C3.
      *Guaranteed by construction. Kept the packed triangle index. Regression:
      `scene_shader_tests.cpp` "Ray passes built from one extraction pack
      triangles in identical order".*
- [x] **Random seeding.** Confirm per-pass seeding (`Sampling.xy`,
      `NextRandomDimension`, `RandomSampleIndex`) and choose the D4 option.
      *Identical in every pass, measured on Metal. Chose explicit per-pass
      dimension ranges through the `RayLaunch` address overloads; rejected the
      wrapper salt.*
- [ ] **Prototype.** Hand-edit a cooked ordered-ray fixture so one pass's
      raygen calls `TraceClosest` directly and writes a hit buffer. Run it on
      Metal and Vulkan to show the wrapper/runtime path needs no runtime change.
  - [x] Metal: `tests/sdk_consumer/rg2/compute_split/` and
        `render_graph_ray_tests.cpp` "A raygen-only pass hands packed triangle
        hits to a later pass" pass with no runtime change. A negative control
        fails.
  - [ ] Vulkan: the same test in the batch CI native suite on Linux. Not run
        yet. Check this box only after a batch run reports it passed rather
        than skipped.
- [x] **Decide D1.** Pick between no format change and `Technique::SceneCompute`
      from that evidence, and record the choice in design.md.
      *No format change.*

## C1 Scene compute execution profile (builder)

- [x] Add `render_graph_compute.py`: lowering, C04 port checks shared with
      `render_graph_ray.py` through extracted helpers, and diagnostics with
      repairs.
      *`render_graph_ray.lower_ports` is the shared helper; ray diagnostics keep
      their wording.*
- [x] Route `_lower_pipeline`. Keep the `Capability` error for compute nodes
      without a Scene input, with updated repair text.
- [x] `render_graph_pipeline.py`: `ColorTarget` dimensions, `state: {}`, the
      `[64, 1, 1]` group-size rule. Generic drafts round-trip byte-identically.
      *`is_scene_compute` selects the scene validation. The group-size rule is
      enforced at lowering, with a repair that clears it, so saved drafts with
      another size still load.*
- [x] `render_graph_build.py`:
  - [x] allow the `compute` stage with the C03 signature;
  - [x] collect `(compute_id,)` variants;
  - [x] force the persistent profile for any sequence with a compute member;
  - [x] reject `StructuredBuffer` on scene compute nodes.
  *The variant collection and persistent profile live in `render_graph_ray`
  (`program_tuple`, `group_profiles`). The StructuredBuffer rejection is in the
  plan compiler, so it names the node and port before any build. The ray
  technique's three default callbacks are now optional as a group, so a
  compute-only manifest names no unused programs.*
- [x] `persistent_wrapper`: raygen-only dispatch cases, and the closest/miss
      `default:` failure for compute pass indices.
- [x] Update the plan summary and diagnostics to name the source node kind.
      Accumulate, ToneMap and Denoise accept compute producers.
- [x] Python tests:
  - [x] invert `test_compute_and_ray_programs_are_capability_gated`
        (now `test_generic_compute_is_capability_gated_and_scene_compute_lowers`);
  - [x] new `test_render_graph_compute.py` covering ports, launch size, group
        size, mixed sequences, the four-pass limit, StructuredBuffer rejection
        and post-processing acceptance;
  - [x] keep `test_render_graph_selection.py` and the existing ray, buffer,
        execution and tone-mapping suites green unchanged.
  *Ran 299 render-graph Python tests on macOS, with the pinned native shader
  tools for the generated-wrapper checks. Seven existing ray fixtures and
  templates (ordered_ray, visibility_sequence, compute_split, raytracing,
  scene_ray, restir_di, visibility_secondary_rays) build byte-identical plans,
  wrappers and Metal artifacts with the `develop` and the new tooling (C08).*
- [x] SDK fixture `tests/sdk_consumer/rg2/scene_compute/`: a single compute
      pass that writes `GetSceneInfo`-derived color. Cook it in
      `modules/graphics/renderer/CMakeLists.txt`.
      *Also cooked by the installed renderer consumer
      (`ludus_sdk_scene_shader_contract_consumer`), which passed against a local
      `macos-clang-development` SDK install.*

## C2 Runtime evidence (same PR as C1 if no runtime change)

No runtime change was needed. Evidence is from Metal on this Mac
(`macos-clang-debug`). Vulkan evidence for the same tests comes only from the
batch CI native suite on Linux.

- [x] `render_graph_ray_tests.cpp`:
  - [x] the scene compute fixture renders the expected pixels;
  - [x] a mixed compute + ray sequence works;
  - [x] the Accumulate stop limit works;
  - [x] the ToneMap exposure works.
  These tests skip only on `Unsupported` ray capability.
  *"A scene compute pass traces the World and feeds Accumulate and ToneMap"
  (exposures 1, 0 and 2, and the `TraceAndShade` error pixel) and "A scene
  compute pass hands packed triangle hits to a later ray pass"
  (`tests/sdk_consumer/rg2/compute_mixed/`). Both pass on Metal.*
- [ ] Activation tests: a compute-only package is a ray package, and
      `StageSelected` falls back to raster without native rays.
  - [x] Ray package: "Scene compute packages are ray packages for renderer
        selection" passes on Metal. `RequiresHardwareRayTracing` is true for
        the cooked package, and `StageSelected` stages it as the hardware ray
        candidate, or raster under Force raster.
  - [ ] Raster fallback without native rays: the same test takes that branch
        on a device that does not enable ray queries. Metal enables them on
        every device here, so this Mac cannot run it. The reference-backend
        test "Unsupported native rays select raster without preparing the ray
        package" covers the identical numeric `RayTracing` record (D1). Check
        this box after a run on a device without native rays.
- [ ] If D1 picked `SceneCompute`: *not applicable, because D1 chose no format
      change.*
  - [ ] bump `plan.h`/`runtime_package.h` with history notes;
  - [ ] add `ValidatePlan` and decoder branches;
  - [ ] add old-runtime rejection tests;
  - [ ] update `test_neutral_format_limits_match_offline_lowering`.
- [x] Docs: in `render-graph-authoring.md`, add "Author scene compute passes"
      under "Execute a triangle ray pipeline", and update "Author pipeline
      nodes" and the architecture doc's link list.
- [x] Run `check-docs`, the strict MkDocs build and `check-wiki
      --documentation-only`.
      *All pass, except that a local `check-wiki` has no generated Doxygen API
      pages (`reference/index.html` → `../api/index.html`). This change touches
      no public API.*

## C3 Editor authoring of scene compute nodes

- [ ] Launch size control and Automatic group size for scene compute nodes,
      in both Properties and the canvas node editor, with undo and Save.
- [ ] Scene bundle connection, the C03 shader stub and the shader interface
      panel signature.
- [ ] Editor tests: node creation, launch-size edits, shader creation, and
      round-trip of old generic drafts.
- [ ] Docs: update the editor steps in the C2 section.

## C4 Two-pass template

Evidence is from Metal on this Mac (`macos-clang-debug`). Vulkan evidence for the
same GPU test comes only from the batch CI native suite on Linux.

- [x] Add sources in `modules/graphics/renderer/render_graphs/compute_ray_tracing/`:
  - [x] `main.lurender.json` and `library.lurender.json`;
  - [x] `main.lushader.json.in`;
  - [x] `main_shaders/{primary,shade,hit_record}.slang`, with D5 encoding,
        validation and attribution.
  *The raster fallback programs (`geometry`, `color`, `depth`) are byte copies of
  ReSTIR's, because a ray package needs `DefaultStaticOpaque.ColorAndDepth`. The
  shade pass uses one visibility query, not two: it rebuilds the surface from the
  record and traces only the shadow ray. Word 1 `.w` bit 1 records a failed
  primary query, so the shade pass shows the error color for it.*
- [x] Tooling: add the `TEMPLATES` entry and the `cli.py` choice. Test it in
      `test_render_graph_templates.py`, including the native-ray parity check.
      *`ComputeRayTracingTemplateTests`; the catalog parity tests cover the new
      graph and snippet entries; `test_render_graph_tone_mapping.py` includes the
      template. 357 render-graph Python tests pass; with the pinned native shader
      tools, one unrelated existing check fails (`test_real_target_entries_...`
      expects a diagnostic location without `column`/`length`).*
- [x] Install and embed:
  - [x] root `CMakeLists.txt` install list;
  - [x] renderer GPU-test `foreach`;
  - [x] editor qrc `foreach`.
- [x] Catalog entries (graph and snippet) and
      `PrepareComputeRayTracingConfiguration`. Update the ordered identity
      lists in `render_graph_template_registry_tests.cpp` and add
      template-application tests.
      *`apps/editor/src/render_graph_compute_template.cpp`. Tests: "Compute
      ray-tracing template is created from the editor picker", "... configuration
      inserts on the root canvas and validates" and "... import extends a shared ray
      technique and preserves conflicting edits", plus the Quick Add search.*
- [ ] GPU acceptance in `render_graph_ray_tests.cpp`:
  - [x] the template renders a lit Cornell-box-style fixture;
  - [x] there are no magenta pixels;
  - [x] the converged mean matches a single-pass `RayTracingPipeline`
        reference with the same direct-lighting policy, within a stated
        tolerance;
  - [x] a resize stages zeroed hits without invalid-hit pixels.
  - [ ] Vulkan: the same test in the batch CI native suite on Linux. Not run yet.
  *"The compute ray-tracing template matches a single-pass direct-lighting
  reference" passes on Metal. The reference is
  `tests/sdk_consumer/rg2/direct_lighting_reference/`; it draws the same values
  from its automatic stream, so after 64 samples the two images are byte-identical
  on Metal. The stated tolerances are 2% of the mean radiance per channel and at
  most one pixel in a thousand differing by more than two levels. A negative control
  that swapped the decoded barycentrics failed the test. After a resize to twice the
  size, four frames show no error pixels.*
- [x] Docs: add "Use the compute ray-tracing template" next to the ReSTIR
      section, and update the Quick Add snippet row and the configuration
      template table.

## Readiness for each PR

- [ ] Warning-clean build and unit tests, with the pinned toolchain.
- [ ] ASan/UBSan build and tests.
- [ ] `./scripts/check --format` and `--tidy`.
- [ ] `source_policy.py --base <merge-base>`.
- [ ] Docs checks when docs change.
- [ ] Read the human PR reviews after the final push.
