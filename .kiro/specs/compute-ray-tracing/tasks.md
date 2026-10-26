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

Evidence is from `macos-clang-debug` on this Mac, with `QT_QPA_PLATFORM=offscreen`.

- [x] Launch size control and Automatic group size for scene compute nodes,
      in both Properties and the canvas node editor, with undo and Save.
      *`IsSceneComputeNode`/`UsesRayLaunch` (`render_graph_pipeline_node.cpp`)
      select the ray Launch size control and a Pipeline State section with the
      automatic 64×1×1 size, an Advanced override that writes only
      `[64, 1, 1]`, a warning for other saved sizes and **Use automatic
      settings**. The size comes from the builder's
      `render_graph_compute.GROUP_SIZE` at configure time. **Add Scene and View
      Inputs** (Properties button and canvas action) and adding a Scene row
      apply `state: {}` and `ColorTarget` through `ApplyComputeProfile`.
      Removing the Scene input restores the generic `[1, 1, 1]` fields. Both
      surfaces call `ReplaceNode`, so undo and Save are shared. The editor's
      ray-launch coverage check now also covers scene compute nodes. Generic
      `new_node` defaults are unchanged.*
- [x] Scene bundle connection, the C03 shader stub and the shader interface
      panel signature.
      *Scene compute nodes use the existing Scene bundle ports and Expand Scene
      Bundle. A new compute shader row on a scene compute node registers a
      `RayLaunch`/`Radiance` program with a stub that calls `TraceClosest`. When
      no ray technique exists, the same edit adds `RendererTriangles.RayTracing`
      with the `Triangles` recipe and `scene_interface`. Unchanged generated
      kernels switch between the generic and C03 stubs with the profile, and
      edited kernels keep their bytes. The Shader API panel lists the C03
      signature, scene types, query functions and the shared payload. The stub
      text, built into the `scene_compute` fixture, compiles for Metal with the
      pinned Slang and SPIRV-Cross through `ludus_tools.cli render-graph build`.*
- [x] Editor tests: node creation, launch-size edits, shader creation, and
      round-trip of old generic drafts.
      *`render_graph_pipeline_tests.cpp` `[scene-compute]` (five cases) and the
      scene compute coverage block in "pipeline texture contracts check samples
      and explicit ray coverage". `render_graph_canvas_tests.cpp` "scene compute
      nodes are authored on the canvas with Properties hidden" covers creation,
      conversion, launch size and group size through the context menu and Edit
      Node, plus undo, redo and Save. Full runs pass:
      `ludus_render_graph_pipeline_tests`, `ludus_render_graph_configuration_tests`,
      `ludus_shader_interface_tests`, `ludus_render_graph_live_build_tests` and
      `ludus_editor_tests` (16 existing skips).*
- [x] Docs: update the editor steps in the C2 section.
      *Replaced the "not available yet" paragraph with numbered editor steps.
      `check-docs`, the strict MkDocs build and `check-wiki
      --documentation-only` pass, except that a local `check-wiki` has no
      generated Doxygen API pages (`reference/index.html` → `../api/index.html`).*

## C4 Two-pass template

- [ ] Add sources in `modules/graphics/renderer/render_graphs/compute_ray_tracing/`:
  - [ ] `main.lurender.json` and `library.lurender.json`;
  - [ ] `main.lushader.json.in`;
  - [ ] `main_shaders/{primary,shade,hit_record}.slang`, with D5 encoding,
        validation and attribution.
- [ ] Tooling: add the `TEMPLATES` entry and the `cli.py` choice. Test it in
      `test_render_graph_templates.py`, including the native-ray parity check.
- [ ] Install and embed:
  - [ ] root `CMakeLists.txt` install list;
  - [ ] renderer GPU-test `foreach`;
  - [ ] editor qrc `foreach`.
- [ ] Catalog entries (graph and snippet) and
      `PrepareComputeRayTracingConfiguration`. Update the ordered identity
      lists in `render_graph_template_registry_tests.cpp` and add
      template-application tests.
- [ ] GPU acceptance in `render_graph_ray_tests.cpp`:
  - [ ] the template renders a lit Cornell-box-style fixture;
  - [ ] there are no magenta pixels;
  - [ ] the converged mean matches a single-pass `RayTracingPipeline`
        reference with the same direct-lighting policy, within a stated
        tolerance;
  - [ ] a resize stages zeroed hits without invalid-hit pixels.
- [ ] Docs: add "Use the compute ray-tracing template" next to the ReSTIR
      section, and update the Quick Add snippet row and the configuration
      template table.

## Readiness for each PR

- [ ] Warning-clean build and unit tests, with the pinned toolchain.
- [ ] ASan/UBSan build and tests.
- [ ] `./scripts/check --format` and `--tidy`.
- [ ] `source_policy.py --base <merge-base>`.
- [ ] Docs checks when docs change.
- [ ] Read the human PR reviews after the final push.
