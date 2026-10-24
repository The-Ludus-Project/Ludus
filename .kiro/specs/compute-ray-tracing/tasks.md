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

- [ ] Add `render_graph_compute.py`: lowering, C04 port checks shared with
      `render_graph_ray.py` through extracted helpers, and diagnostics with
      repairs.
- [ ] Route `_lower_pipeline`. Keep the `Capability` error for compute nodes
      without a Scene input, with updated repair text.
- [ ] `render_graph_pipeline.py`: `ColorTarget` dimensions, `state: {}`, the
      `[64, 1, 1]` group-size rule. Generic drafts round-trip byte-identically.
- [ ] `render_graph_build.py`:
  - [ ] allow the `compute` stage with the C03 signature;
  - [ ] collect `(compute_id,)` variants;
  - [ ] force the persistent profile for any sequence with a compute member;
  - [ ] reject `StructuredBuffer` on scene compute nodes.
- [ ] `persistent_wrapper`: raygen-only dispatch cases, and the closest/miss
      `default:` failure for compute pass indices.
- [ ] Update the plan summary and diagnostics to name the source node kind.
      Accumulate, ToneMap and Denoise accept compute producers.
- [ ] Python tests:
  - [ ] invert `test_compute_and_ray_programs_are_capability_gated`;
  - [ ] new `test_render_graph_compute.py` covering ports, launch size, group
        size, mixed sequences, the four-pass limit, StructuredBuffer rejection
        and post-processing acceptance;
  - [ ] keep `test_render_graph_selection.py` and the existing ray, buffer,
        execution and tone-mapping suites green unchanged.
- [ ] SDK fixture `tests/sdk_consumer/rg2/scene_compute/`: a single compute
      pass that writes `GetSceneInfo`-derived color. Cook it in
      `modules/graphics/renderer/CMakeLists.txt`.

## C2 Runtime evidence (same PR as C1 if no runtime change)

- [ ] `render_graph_ray_tests.cpp`:
  - [ ] the scene compute fixture renders the expected pixels;
  - [ ] a mixed compute + ray sequence works;
  - [ ] the Accumulate stop limit works;
  - [ ] the ToneMap exposure works.
  These tests skip only on `Unsupported` ray capability.
- [ ] Activation tests: a compute-only package is a ray package, and
      `StageSelected` falls back to raster without native rays.
- [ ] If D1 picked `SceneCompute`:
  - [ ] bump `plan.h`/`runtime_package.h` with history notes;
  - [ ] add `ValidatePlan` and decoder branches;
  - [ ] add old-runtime rejection tests;
  - [ ] update `test_neutral_format_limits_match_offline_lowering`.
- [ ] Docs: in `render-graph-authoring.md`, add "Author scene compute passes"
      under "Execute a triangle ray pipeline", and update "Author pipeline
      nodes" and the architecture doc's link list.
- [ ] Run `check-docs`, the strict MkDocs build and `check-wiki
      --documentation-only`.

## C3 Editor authoring of scene compute nodes

- [ ] Launch size control and Automatic group size for scene compute nodes,
      in both Properties and the canvas node editor, with undo and Save.
- [ ] Scene bundle connection, the C03 shader stub and the shader interface
      panel signature.
- [ ] Editor tests: node creation, launch-size edits, shader creation, and
      round-trip of old generic drafts.
- [ ] Docs: update the editor steps in the C2 section.

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
