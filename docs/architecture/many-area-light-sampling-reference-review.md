# Many-area-light resampling estimator review

Status: proposed design, reviewed October 27, 2026. Revised the same day to
reconcile it with the CPU and GPU reference requirement and with the
polygonal-light work's licensing, traversal and primitive decisions. The
FoundationMath primitives listed under [Ownership](#ownership) are implemented;
the estimator itself is not.
This review is the estimator review that
[hybrid and real-time transport](renderer-systems.md#hybrid-and-real-time-transport)
requires before resampling enters a Ludus estimator. It covers next-event
estimation (NEE) for emissive triangles, delivered in two stages:

1. A built-in render graph template, `ris-projected-solid-angle`: single-bounce
   direct lighting beside the existing
   [MIS](../development/render-graph-authoring.md#compare-light-and-bsdf-sampling-with-mis)
   and [ReSTIR DI](../development/render-graph-authoring.md#use-the-built-in-restir-di-template)
   templates.
2. An explicit second light-sampling profile for the RT1 progressive reference,
   delivered in the CPU oracle and the GPU-compute reference in the same change.

Spatiotemporal reservoir reuse remains a separate RT4 upgrade. The Peters 2021
polygonal-light work runs in parallel and shares two pieces with this design: the
projected-solid-angle primitives and the template shadow-ray traversal.

## Sources reviewed

| Source actually reviewed | Consequence for Ludus |
| --- | --- |
| Ishaan Shah, Aakash KT and P. J. Narayanan, [“Combining Resampled Importance and Projected Solid Angle Samplings for Many Area Light Rendering”](https://doi.org/10.1145/3610543.3626165), SIGGRAPH Asia 2023 Technical Communications; [author PDF](https://cdn.iiit.ac.in/cdn/cvit.iiit.ac.in/images/ConferencePapers/2023/ris_proj_sa.pdf) §§2–6 and Algorithm 1 read in full | Resample *lights*, not points. The candidate density is `p(A_j|x)`, the target is the unshadowed LTC shading of the whole light, `M = 32`, candidates stream through a weighted reservoir, and the selected light then receives one projected-solid-angle/LTC sample. The naive variant (their §3.1), which runs the expensive point sampler once per candidate, is rejected for the reason they give. Their renderer computes direct lighting only, with no BSDF-hit MIS and no reuse. Their §6 single-lobe limitation is addressed below rather than inherited. |
| Christoph Peters, [“BRDF Importance Sampling for Polygonal Lights”](https://doi.org/10.1145/3450626.3459672), ACM TOG 40(4), article 140, 2021; [author PDF](https://cg.ivd.kit.edu/publications/2021/polygonal_lights/peters2021-brdf_importance_sampling_for_polygonal_lights-paper.pdf) §§3.1, 3.7, 4.1–4.3, 6.1 and 6.5 | Projected-solid-angle sampling of a convex polygon clipped to `z ≥ 0` in the shading frame, with clockwise winding (flip `y` when the polygon is seen from behind), FP32 stability notes and a fixed two-iteration inversion. LTC sampling transforms the polygon by `M⁻¹`, clips only in LTC space, and disables the specular technique when that clip is empty. Mixed BRDFs take one sample per technique and combine them with clamped optimal MIS, `v = 1/2`; the paper recommends against the one-sample estimator (§4.2). Diffuse albedo in the MIS coefficients is clamped to at least 0.01 per channel so the diffuse technique always covers the polygon. |
| Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, [“Real-Time Polygonal-Light Shading with Linearly Transformed Cosines”](https://doi.org/10.1145/2897824.2925895), ACM TOG 35(4), article 41, 2016 (SIGGRAPH 2016); author preprint §§3.1–3.2, 4 and 5.1, reviewed for the LTC transform | The LTC density (Eq. 1) is the clamped cosine (Eq. 4) of `M⁻¹ω/‖M⁻¹ω‖` times the Jacobian `|M⁻¹|/‖M⁻¹ω‖³`. It is invariant to scaling `M`, its polygon integral is the clamped-cosine integral over `M⁻¹P` (Eq. 3), and it is sampled by mapping a cosine sample through `M` (Algorithm 1). Isotropic BRDF fits use the four-parameter form of Eq. 6 with the L3 error and store `M⁻¹` plus the BRDF norm over `(θ, √α)`. The fitter must reimplement this from the text, because the published utilities are not used. |
| Chris Wyman, “Weighted Reservoir Sampling,” *Ray Tracing Gems II*, chapter 22, via the existing [streaming selection review](randomness-gems-review.md#streaming-selection) | Sequential, with-replacement reservoir over an explicit candidate order, with zero totals handled explicitly. |

Cited through those sources but **not separately reviewed in this pass**. Review
the cited sections before implementing the corresponding piece, and add their
attribution beside the code:

- Stephen Hill and Eric Heitz, “Real-Time Area Lighting: a Journey from Research
  to Production,” SIGGRAPH 2016 Advances course: the approximate form-factor
  evaluation that Shah et al. use for speed.
- Benedikt Bitterli et al., “Spatiotemporal Reservoir Resampling for Real-Time Ray
  Tracing with Dynamic Direct Lighting,” ACM TOG 39(4), 2020, and Justin Talbot,
  David Cline and Parris Egbert, “Importance Resampling for Global Illumination,”
  EGSR 2005: the RIS contribution weight and its unbiasedness conditions.

**Licensing, as decided for the polygonal-light work.**
- Of Peters' published renderer code, only `polygon_sampling.glsl` is dual
  licensed BSD-3/GPLv3. It may be ported, keeping its notice.
- The clipping, LTC table and utility code, and shading passes are GPLv3-only.
  They are reimplemented from the paper's text (and, for LTCs, from Heitz et al.
  above), never copied.
- No third-party LTC tables are imported. Ludus fits its own.

## Current estimators

| Estimator | Emissive-triangle NEE today |
| --- | --- |
| RT1 reference: [reference_path.cpp](../../modules/graphics/renderer/src/reference_path.cpp), [renderer_reference.slang](../../modules/graphics/renderer/shaders/renderer_reference.slang), [renderer_gpu_preview.slang](../../modules/graphics/renderer/shaders/renderer_gpu_preview.slang) | Slot 0 picks a category uniformly from `N = Na + Ne + e`. If it lands on an emitter, slot 7 (`LUDUS_PATH_RANDOM_EMITTER_SLOT`) picks a triangle from the power-weighted integer CDF (luminance × area, quantized against 2²⁴, at least one unit per positive emitter). Slots 1–2 then sample uniformly by area. BSDF hits weight with the power heuristic using the same selection probability ([frozen profile](renderer-systems.md#reference-workflow-and-frozen-profile)). |
| `veach-guibas-mis` template | `SelectLight` is uniform over `GetLightCount`. Within the chosen emitter surface, the triangle is chosen in proportion to area and the point uniformly, then converted to solid angle. The template exports MIS, light-only and BSDF-only outputs with selectable heuristics. |
| `restir-di` template | RIS over *points* in area measure. `M = 8` by default, a distance-weighted emitter PMF and an unshadowed point target, with bounded temporal and spatial reuse. It is light-only and documented as not a full unbiased reference. |
| `tiled-vbuffer-nee` template | A per-tile emitter CDF with floors, combined with the BSDF by the power heuristic. |

None of these accounts for the cosine and BSDF when sampling a point on a
triangle, and only ReSTIR adapts light choice to the receiver. When this review
was written, `develop` had no LTC, projected-solid-angle or polygon-clipping code.
FoundationMath has since gained the projected-solid-angle primitives, the
sequential reservoir and the LTC polygon transform (see [Ownership](#ownership)).
The ReSTIR template still uses its private reservoir helpers.

## Proposed estimator

Notation: receiver `x` with outgoing direction `ωo`; emissive triangles `t`;
candidate density `p_src(t)`; `Y(c)` is BT.709 luminance of an RGB value. The
template and RT1 differ only where noted.

1. **Category selection is unchanged.** RT1 slot 0 and the template's
   `SelectLight` draw keep their uniform choice. Analytic lights and the
   environment keep their current estimators and MIS densities. A draw that lands
   on an emitter selects the *emissive set* with probability `P_E` (`Ne/N`), and
   its specific emitter is discarded. Only the emissive branch changes.
2. **Candidate density.** `p_src` is the existing power-weighted triangle CDF
   that RT1 already packs. Graph shaders read it through two additive
   scene-interface v1 functions: `SelectEmissiveTriangle` selects a triangle
   from an explicit dimension, and `EvaluateEmissiveTriangleSelectionPmf` returns
   a triangle's probability. Every positive emitter keeps a nonzero probability.
3. **Per-receiver LTC setup.** Look up the specular LTC once per receiver from
   roughness, `dot(n, ωo)` and `F0`. This gives `M₁⁻¹` and specular albedo `a₁`.
   The diffuse technique is the identity LTC `M₀ = I`, with albedo `a₀`
   approximating the Fresnel-weighted Lambert lobe and clamped to 0.01 per
   channel (Peters §4.2). These guide sampling and weights only; the BSDF
   (`EvaluateStandardBsdf` / L4 reference BSDF) is always evaluated exactly.
4. **Target.** For triangle `t`:

   ```text
   E₀(t) = projected solid angle / π of t clipped to z ≥ 0 in the shading frame
   E₁(t) = the same integral after transforming the vertices by M₁⁻¹
   p̂(t)  = Y(L̄_t) · (Y(a₀)·E₀(t) + Y(a₁)·E₁(t))
   ```

   - A one-sided emitter seen from behind has `p̂ = 0`.
   - `L̄_t` is the surface's constant emission factor. Emission maps are
     RGBA8 values in `[0, 1]` multiplied by that factor, so it bounds textured
     emission from above and needs no new cooked data.
   - Textured emission, alpha cutoff, finite range and visibility only make the
     true integrand smaller, which is allowed.
   - **Support invariant:** `p̂(t) > 0` whenever `t` has positive area above the
     shading horizon and `Y(L̄_t) > 0`. The clamped `a₀` and an exact clipped
     `E₀` guarantee it. Approximate evaluations need a new version that proves
     the same invariant.
5. **Resampled light selection.**
   - Draw `M` candidates `t_i ~ p_src` with replacement. The template's
     `RIS_CANDIDATE_COUNT` setting defaults to 32 with a maximum of 32; RT1
     fixes `M = 32`.
   - Weight each candidate `w_i = p̂(t_i) / p_src(t_i)`.
   - Stream candidates in index order: `w_sum += w_i`, and accept `t_i` when
     `u_i · w_sum < w_i`. Zero-weight candidates are never selected. If
     `w_sum = 0`, the branch contributes zero; this is a valid outcome, not an
     invalid sample.
   - The selected triangle `y` has contribution weight `W = w_sum / (M · p̂(y))`.
     For any `g` that is zero wherever `p̂` is zero, `E[W · g(Y)] = Σ_t g(t)`.
   - Float tickets quantize acceptance to `2⁻²⁴`, the same declared
     approximation as other uses of `SampleFloat01`.
6. **Projected-solid-angle and LTC sampling on `y`** (Peters §§3, 4.1, 4.3).
   - Take one diffuse sample `ω₀` with `p₀ = cos θ / (π · E₀)`.
   - Take one LTC sample `ω₁` with `p₁ = p_M₁(ω) / E₁`, unless the LTC-space clip
     is empty, in which case `p₁ ≡ 0` and only `ω₀` is traced.
   - Combine them with clamped optimal MIS, `v` defaulting to 1/2, using
     coefficients `c_t = Y(L̄_y) · Y(a_t) · E_t`.
   - Map each direction to barycentrics by intersecting the triangle's plane.
     Emission, alpha, sidedness and visibility come from that point
     (`GetSurfaceHit`, `EvaluateEmission`, `TestVisibility` in the template). A
     plane hit outside the triangle in FP32 contributes zero.
   - Emissive NEE therefore costs up to **two shadow rays**. The fixed
     two-iteration inversion has a small backward error, which Peters §3.6
     reports near `1e-5`. Tests bound it and the estimator declares it.
7. The contribution of light technique `s` is
   `W_L(ω_s, y) · w^c_s(ω_s) · F(ω_s) / p_s(ω_s) · W / P_E`. Here `F` is the
   shadowed BSDF·cosine·emission and `W_L` is the BSDF-hit split defined next.

## Decision: BSDF-hit MIS

A BSDF ray that hits an emitter needs the light strategy's density, and RIS has
no tractable density: whether `t` is selected depends on all `M` candidates. MIS
weights do not need true densities, however. Weights stay unbiased if they are
fixed functions of the sample, sum to one across strategies, and are zero where a
strategy cannot sample.

| Option | Verdict |
| --- | --- |
| Use the candidate density `P_E · p_src · p_area` as the light density | Rejected: it ignores resampling, so the BSDF wins weights it should lose. |
| Exact `p̂(t) / Σ p̂` | Rejected: `O(emitters)` target evaluations at every hit. |
| Reuse this reservoir's `w_sum` | Rejected: the weight would correlate with the candidates that chose `y`, which biases the estimate. |
| Light-only (`W_L = 1` wherever `p̂ · (p₀ + p₁) > 0`), the paper's estimator | Not the combined estimator. The template exports it as `lightSampling`. It misses sharp highlights from emitters that are rarely candidates. |
| **Surrogate with an independent normalization estimate** | **Adopted.** |

The adopted surrogate draws `K` further candidates `t'_k ~ p_src` from their own
dimensions. They are independent of the reservoir and of the BSDF sample. The
template's `RIS_NORMALIZATION_CANDIDATE_COUNT` defaults to 8 with a maximum of
16; RT1 fixes `K = 8`.

```text
Ẑ     = (1/K) · Σ_k p̂(t'_k) / p_src(t'_k)
cap   = min(1, M · p_src(t))               // ≈ chance that t is a candidate at all
P̃(t)  = P_E · min(p̂(t) / Ẑ, cap)           // p̂/Ẑ := +∞ when Ẑ = 0 and p̂ > 0; 0 when p̂ = 0
q     = P̃(t) · (p₀(ω) + p₁(ω))             // one sample per light technique
W_L   = q² / (q² + p_b²)                   // power heuristic; BSDF weight 1 − W_L
```

- **Why it is unbiased.** Conditioned on the normalization candidates, `W_L` is a
  fixed function of `x`, `ωo`, `t` and `ω`.
  - The light estimator integrates `W_L · F` without bias, and the BSDF
    estimator integrates `(1 − W_L) · F`.
  - Peters' `w^c_s` partition the light group.
  - Zero-density cases are consistent: `p_s = 0` gives `w^c_s = 0`, `q = 0` gives
    `W_L = 0`, and `p_b = 0` gives `W_L = 1`.
  - So every normalization draw sums to `∫F`.
- **Why the cap matters.** It hands rarely-included sharp highlights back to
  BSDF sampling.
- **Template cost.** Both strategies run in the same shade invocation, so `Ẑ` and
  the receiver frame stay in registers. A BSDF hit on an emissive triangle adds
  one target evaluation plus the projected-solid-angle and LTC density setup.
- **RT1 cost.** Addressed randomness makes `Ẑ` a pure function of
  `(pixel, sample, bounce)`, so it is computed lazily by one shared function:
  - at NEE, when the emissive branch is chosen;
  - at the next vertex's emissive hit, using the previous bounce's slots.

  Both sites must read bit-identical stored inputs: the previous receiver's
  position, shading frame, `M₁⁻¹`, `a₀`/`a₁` luminance and `P_E`. These join the
  path state wherever the emitter-hit MIS density is computed today.

## Template: `ris-projected-solid-angle`

The first deliverable is a built-in editable template, so the estimator ships
inside a legal, inspectable graph instead of a hidden renderer mode. Its shape
copies `veach-guibas-mis`, so the editor's compute-template path and the
existing evidence pattern apply unchanged.

```text
main:    Scene, Camera, RenderSize, color ──► [transport: Subgraph RisProjectedSolidAngle]
            color ─────────► Accumulate(4096) ─► ToneMap(1) ─► output
            lightSampling ─► Accumulate(4096) ─► ToneMap(1) ─► lightSamplingOutput
            bsdfSampling ──► Accumulate(4096) ─► ToneMap(1) ─► bsdfSamplingOutput

library: hits Buffer (32 B/px, RenderSize)
         primary ──► shade ──► shadeLight ──► shadeBsdf      (ComputePipeline ×4; hits Load/Store)
           │           │color      │lightSampling │bsdfSampling
```

- **Passes.** Four `ComputePipeline` passes, the current `MAX_RAY_PASSES`:
  - `primary` records hits;
  - `shade` runs the combined estimator above;
  - `shadeLight` runs RIS plus projected-solid-angle sampling alone (the paper's
    estimator);
  - `shadeBsdf` runs BSDF sampling alone.

  The technique is `RendererTriangles.RayTracing` with
  `scene_interface {version 1, surface_queries 1, visibility_queries 1}`, plus
  the polygon-light requirement below.
- **Traversal.** Shadow and BSDF rays use the renderer's GPU-compute BVH, the same
  decision as the polygonal-light templates, so the template does not require
  native ray tracing.
  - Today graph passes choose only `NativeQuery` or `TriangleLoop`, and
    `TriangleLoop` is a linear scan meant for small scenes. A compute-BVH strategy
    is therefore a prerequisite, added once at the shared
    [traversal boundary](../development/render-graph-authoring.md#reuse-ray-callbacks-in-compute-pipelines)
    rather than inside either template's callbacks.
  - A `NativeQuery` configuration can follow later as a separate configuration.
  - Browser execution stays outside the graph ray profile until that profile
    admits it.
- **Settings.** Finite, author-declared values:
  - `RIS_CANDIDATE_COUNT`: 32, range 1–32;
  - `RIS_NORMALIZATION_CANDIDATE_COUNT`: 8, range 1–16;
  - `PROJECTED_SOLID_ANGLE_MIS_VISIBILITY`: Peters' `v`, 0.5, range 0–1.

  Settings never change the random layout. Its ranges are sized for the maximums.
- **Baseline.** The `UniformArea` counterpart is the existing `veach-guibas-mis`
  template, not a duplicate. Shade dimensions 2–8 keep Veach's meanings, so
  analytic, environment and BSDF draws are paired across the two templates.

| Template dimension | Meaning |
| --- | --- |
| 0–1 | Primary jitter (`primary` pass) |
| 2 | `SelectLight` category |
| 3–5 | Analytic/environment `SampleIncidentLight` shape |
| 6–8 | BSDF sample |
| 9–10 | Diffuse projected-solid-angle sample `ω₀` |
| 11 | Reserved |
| 12–13 | LTC sample `ω₁` |
| 14–15 | Reserved |
| 16 + 2i, 17 + 2i, `i < 32` | Candidate `i`: power-CDF ticket, reservoir ticket |
| 80 + k, `k < 16` | Normalization candidate `k`: power-CDF ticket |

**Shader helpers.**
- The projected-solid-angle shader source from the polygonal-light work is
  reused, not duplicated. FoundationMath adds the sequential reservoir beside it.
- The renderer owns a generated LTC-table include, shared with the
  polygonal-light LTC templates.
- Stage them through the engine ray helpers (`HELPERS` in `render_graph_ray.py`
  and the installed shader directory).
- The wrapper includes them only when the technique declares a new
  `scene_interface` requirement, in the same way `visibility_buffer` adds its
  include today. Older SDKs reject that requirement explicitly.

**Registration** follows the existing template checklist:
- `TEMPLATES` and the CLI `--template` choices;
- the root CMake install list and the editor resource list and catalog (graph
  and snippet entries);
- the editor compute-template dispatch;
- the identity lists in the Python and editor template tests;
- the renderer GPU staging loop;
- a usage section in the authoring guide beside
  [Compare light and BSDF sampling with MIS](../development/render-graph-authoring.md#compare-light-and-bsdf-sampling-with-mis),
  added only once the template ships.

## RT1 profile split and identity

After the template proves the estimator, RT1 gains an explicit integrator setting
with two named modes. Under the
[CPU and GPU reference requirement](renderer-systems.md#cpu-and-gpu-reference-implementation-requirement),
the resampled mode lands in the CPU oracle
([reference_path.cpp](../../modules/graphics/renderer/src/reference_path.cpp))
and the GPU-compute reference
([renderer_reference.slang](../../modules/graphics/renderer/shaders/renderer_reference.slang))
in the same change, with parity evidence. A CPU-only first step is not a
deliverable.

| Mode | Contract |
| --- | --- |
| `UniformArea` | The current estimator, including the power-weighted emitter CDF. It stays the default and bit-identical to existing captures (`SamplerVersion = 1`, 8 dimensions per bounce), and remains the validation oracle. |
| `ResampledProjectedSolidAngle` | This review's estimator. `SamplerVersion = 2`, 128 dimensions per bounce. Requires the renderer's LTC table. |

- **API.** A proposed `ReferenceLightSampling` enum on
  `ReferenceCaptureDescription`, recorded in `ReferenceCaptureInfo`.
  - `CpuOracle` and `NativeQueries` share the CPU transport; `GpuCompute` runs
    the same estimator in its kernels.
  - The setting joins the integral/accumulation key, so changing it builds a
    replacement job and resets accumulation.
  - The separate GPU preview may adopt the mode later. That does not count toward
    the reference requirement, which the preview does not yet satisfy.
- **LUHDR.** The sampler field carries 2, and new flag bit `128` marks the
  resampled mode (bit 64 already means `gpu_compute`). Today
  `scripts/reference-hdr` rejects any sampler other than 1 and any flag above
  127, so old converters refuse these captures rather than mislabel them. Extend
  the converter and its manifest in the same change.
- **Versions.** `ProfileVersion` (`LIGHTING_PROFILE_VERSION`) is unchanged, because
  both modes estimate the same truncated RGB integral. Changes to packed words
  bump the reference kernel layout, and the preview's separate
  `LUDUS_GPU_PREVIEW_KERNEL_VERSION` if the preview adopts the mode.
- **Acceptance.** Both modes are unbiased for the same integral at matched depth,
  roulette and range, so acceptance is statistical agreement across seeds, not
  pixel equality.
  - CPU and GPU-compute resampled outputs must also agree with each other, on raw
    emission, direct, indirect and total radiance, with documented tolerances.
    Their discrete selections can diverge under FP32 differences.
  - Record the backend, device and toolchain with the evidence.
- **No fallback.** A device or package without the LTC table rejects the
  resampled mode explicitly.

### Random layout, sampler version 2

Slots 0–7 keep their version-1 meanings. New samples use unused dimensions,
aligned to Philox's four-lane blocks:

| Bounce slot | Meaning |
| --- | --- |
| 0 | Category selection, bounded over `N` (unchanged) |
| 1–2 | Environment sample, or the diffuse projected-solid-angle sample `ω₀` |
| 3–5 | BSDF continuation (unchanged) |
| 6 | Russian roulette (unchanged) |
| 7 | Power-CDF emitter ticket for the `UniformArea` branch; unused by resampling |
| 8–9 | LTC sample `ω₁` |
| 10–11 | Reserved |
| 12 + 2i, 13 + 2i, `i < 32` (12–75) | Candidate `i`: power-CDF ticket, reservoir ticket |
| 76 + k, `k < 8` (76–83) | Normalization candidate `k`: power-CDF ticket |
| 84–127 | Reserved; future meanings take unused slots and never renumber |

- Add a version-2 stride constant of 128 beside
  `LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE`, which keeps 8 for version 1.
- The largest dimension at `LUDUS_REFERENCE_MAX_DEPTH = 64` is
  `2 + 63 · 128 + 127 = 8193 ≤ 65535`. Extend the `static_assert` and
  `ReferenceBounceDimension` per sampler version.
- `M`, `K` and the stride are frozen constants of version 2.

## Ownership

| Piece | Owner | Notes |
| --- | --- | --- |
| Convex polygon horizon clipping (up to seven input vertices), clipped projected-solid-angle integral, Peters sampler and density | FoundationMath `projected_solid_angle.hpp` and `shaders/projected_solid_angle.slang`, being added by the polygonal-light work | The sampler is ported from the BSD-licensed `polygon_sampling.glsl` with its notice. Clipping is an independent reimplementation. This design consumes these primitives and adds none of its own. |
| Generic LTC polygon transform | FoundationMath, beside those primitives | Implemented as `linearly_transformed_cosine.hpp` and `shaders/linearly_transformed_cosine.slang` ([contract](../modules/foundation/math.md#linearly-transformed-cosines-over-polygons)): transform the vertices by `M⁻¹`, then reuse the same clipping, integral and sampler. Credits Heitz et al. 2016 (§3.1, Eq. 3, Algorithm 1) and Peters 2021 §4.1. |
| Sequential weighted reservoir | FoundationMath | Implemented as `weighted_reservoir.hpp` and `shaders/weighted_reservoir.slang` ([contract](randomness.md#sequential-weighted-reservoir)): explicit order, zero totals, and selection-frequency tests on exact small integers. The ReSTIR template may adopt it separately; this design does not change that template. |
| LTC table for Ludus GGX/correlated Smith/Schlick, its fitter and provenance | Renderer (generated include) plus an offline Ludus tool. **Owned by this RIS/many-area-light workstream** (decided October 27, 2026). The polygonal-light LTC templates reuse the same table and fitter rather than building their own. | Implemented as `tools/ltc-fit`, `shaders/ltc_table_data.inc` with its provenance, and the `ltc_table.slang` and CPU lookups ([fit, regeneration and accuracy](../development/ltc-table.md)). Reimplemented from Heitz et al. §4 and Peters §§4.1–4.2, not from GPLv3 utilities. It covers roughness down to 0.045, where every view up to 81° fits within 1% relative L1. `F0` stays exact in the tabulated albedo instead of forming a third axis. A poor fit costs variance, not correctness. |
| Compute-BVH graph traversal strategy | Renderer, at the shared traversal boundary | Shared with the polygonal-light templates; a prerequisite for both |
| Power-CDF scene-interface accessors | Renderer | Implemented as `SelectEmissiveTriangle` and `EvaluateEmissiveTriangleSelectionPmf` in `scene_shader.slang` ([usage](../development/render-graph-authoring.md#select-emissive-triangles-by-power)). They read the CDF that `PackReferenceEmitterSelection` packs for RT1, and the private C++ counterparts `SelectReferenceEmissiveTriangle` and `EvaluateReferenceEmissiveTriangleSelectionPmf` match them in the `[power-cdf]` GPU parity test |
| Diffuse albedo approximation, RT1 path state | Renderer | One packed CDF serves RT1 and graph shaders |

## Acceptance

1. **Primitives.** The projected-solid-angle primitives carry their own C++ and
   Slang parity tests from the polygonal-light work. This design adds parity tests
   for the reservoir and the LTC transform.
2. **LTC table.** Integral checks against Monte Carlo estimates of the exact BRDF.
3. **Template.** Python template tests modeled on `VeachGuibasMisTemplateTests`,
   including capability rejection for the selected traversal. GPU tests on
   Cornell, many-emitter, textured, two-sided, metallic and minimum-roughness
   fixtures:
   - the three outputs agree with each other and with `veach-guibas-mis`
     light-only within 2% of the mean at 256 spp;
   - MIS mean squared error against a 1024-spp image is below `veach-guibas-mis`
     MIS at equal spp on the many-emitter fixture;
   - an opt-in equal-time benchmark that reports shadow rays per pixel and kernel
     time.
4. **RT1 resampled profile.** The CPU oracle and the GPU-compute reference in one
   change.
   - Each agrees statistically with `UniformArea` across seeds and depths, and
     CPU and GPU agree with each other on raw outputs.
   - Edge cases are covered in both paths: `Ẑ = 0`, an empty LTC clip, all
     emitters behind the receiver, invalid contributions, and a unit test that the
     MIS weights sum to one.
5. **Evaluation.** Equal-sample and equal-time error against high-sample
   `UniformArea` references, reporting MAPE as in Shah et al. §5 plus RMSE, on
   scenes with thousands of emissive triangles.

Out of scope:
- reservoir reuse (RT4) and light-level reservoirs in `restir-di`, which would
  need their own review;
- a light tree;
- raster LTC area lighting;
- anisotropic roughness;
- polygons beyond triangles;
- Shah et al.'s naive per-candidate variant.

Open questions, to settle by measurement:
- `K`, and Peters' `v`;
- the diffuse albedo approximation;
- LTC table resolution and `F0` handling (the first table uses 64 × 64 cells and
  an exact `F0` albedo; see [its accuracy](../development/ltc-table.md#accuracy));
- whether two shadow rays beat Peters' one-sample variant for this workload.
