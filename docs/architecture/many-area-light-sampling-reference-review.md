# Many-area-light resampling estimator review

Status: reviewed October 27, 2026. Revised the same day to reconcile it with the
CPU and GPU reference requirement and with the polygonal-light work's licensing,
traversal and primitive decisions. The primitives listed under
[Ownership](#ownership) are implemented. The `ris-projected-solid-angle` template
(stage 1) is implemented as of October 28, 2026, with the departures listed under
[Implementation notes](#implementation-notes). The RT1 profile (stage 2) is
implemented as of October 29, 2026, in the CPU oracle and the GPU-compute
reference, with the choices listed under [RT1 implementation notes](#rt1-implementation-notes).
Acceptance item 5 was measured the same day; see
[RT1 equal-time evaluation](#rt1-equal-time-evaluation). Later that day both
stages gained Hill and Heitz's [approximate target](#approximate-target), which
lowers the per-candidate cost behind those equal-time results.
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
projected-solid-angle primitives and the template shadow-ray traversal. Its
comparison template, `polygonal-light-sampling`, is implemented as of October 28,
2026: it shades every emissive triangle, without light selection, with clamped
optimal MIS, the balance heuristic and each technique alone, and has a CPU oracle
with GPU parity tests. On October 29, 2026 it gained Peters' §6.1 clip of the
shading normal to the view's hemisphere, in the template and the oracle, and an
occluded-penumbra fixture after his Fig. 10, where the GPU shows clamped optimal
MIS beating the weighted balance heuristic. Its limit of eight emissive triangles
stays: lifting it needs a change to the scene-interface contract. Its usage,
departures, evidence and those options are in the
[authoring guide](../development/render-graph-authoring.md#compare-brdf-importance-sampling-for-polygonal-lights).

## Sources reviewed

| Source actually reviewed | Consequence for Ludus |
| --- | --- |
| Ishaan Shah, Aakash KT and P. J. Narayanan, [“Combining Resampled Importance and Projected Solid Angle Samplings for Many Area Light Rendering”](https://doi.org/10.1145/3610543.3626165), SIGGRAPH Asia 2023 Technical Communications; [author PDF](https://cdn.iiit.ac.in/cdn/cvit.iiit.ac.in/images/ConferencePapers/2023/ris_proj_sa.pdf) §§2–6 and Algorithm 1 read in full | Resample *lights*, not points. The candidate density is `p(A_j|x)`, the target is the unshadowed LTC shading of the whole light, `M = 32`, candidates stream through a weighted reservoir, and the selected light then receives one projected-solid-angle/LTC sample. The naive variant (their §3.1), which runs the expensive point sampler once per candidate, is rejected for the reason they give. Their renderer computes direct lighting only, with no BSDF-hit MIS and no reuse. Their §6 single-lobe limitation is addressed below rather than inherited. |
| Christoph Peters, [“BRDF Importance Sampling for Polygonal Lights”](https://doi.org/10.1145/3450626.3459672), ACM TOG 40(4), article 140, 2021; [author PDF](https://cg.ivd.kit.edu/publications/2021/polygonal_lights/peters2021-brdf_importance_sampling_for_polygonal_lights-paper.pdf) §§3.1, 3.7, 4.1–4.3, 6.1 and 6.5 | Projected-solid-angle sampling of a convex polygon clipped to `z ≥ 0` in the shading frame, with clockwise winding (flip `y` when the polygon is seen from behind), FP32 stability notes and a fixed two-iteration inversion. LTC sampling transforms the polygon by `M⁻¹`, clips only in LTC space, and disables the specular technique when that clip is empty. Mixed BRDFs take one sample per technique and combine them with clamped optimal MIS, `v = 1/2`; the paper recommends against the one-sample estimator (§4.2). Diffuse albedo in the MIS coefficients is clamped to at least 0.01 per channel so the diffuse technique always covers the polygon. |
| Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, [“Real-Time Polygonal-Light Shading with Linearly Transformed Cosines”](https://doi.org/10.1145/2897824.2925895), ACM TOG 35(4), article 41, 2016 (SIGGRAPH 2016); author preprint §§3.1–3.2, 4 and 5.1, reviewed for the LTC transform | The LTC density (Eq. 1) is the clamped cosine (Eq. 4) of `M⁻¹ω/‖M⁻¹ω‖` times the Jacobian `|M⁻¹|/‖M⁻¹ω‖³`. It is invariant to scaling `M`, its polygon integral is the clamped-cosine integral over `M⁻¹P` (Eq. 3), and it is sampled by mapping a cosine sample through `M` (Algorithm 1). Isotropic BRDF fits use the four-parameter form of Eq. 6 with the L3 error and store `M⁻¹` plus the BRDF norm over `(θ, √α)`. The fitter must reimplement this from the text, because the published utilities are not used. |
| Chris Wyman, “Weighted Reservoir Sampling,” *Ray Tracing Gems II*, chapter 22, via the existing [streaming selection review](randomness-gems-review.md#streaming-selection) | Sequential, with-replacement reservoir over an explicit candidate order, with zero totals handled explicitly. |
| Stephen Hill and Eric Heitz, [“Real-Time Area Lighting: a Journey from Research to Production”](https://advances.realtimerendering.com/s2016/), SIGGRAPH 2016 Advances in Real-Time Rendering course; slides and speaker notes 63–77 (edge integrals, θ/sin θ fits) and 87–102 (horizon clipping, vector form factor, proxy sphere), reviewed October 29, 2026 | The [approximate target](#approximate-target). Without its projection onto the normal, the sum of edge integrals is the polygon's vector form factor `F`, whose length is the form factor in the direction of `F` (slides 90–93). A sphere with the same `F` has angular extent `asin(√|F|)` (slides 94–96). Its horizon-clipped form factor is tabulated from Snyder's closed form (slide 97), or approximated by `max((|F|² + F.z) / (|F| + 1), 0)` without a table (slide 102). θ/sin θ uses a quadratic fit for diffuse (slide 77) and a cubic rational fit where accuracy matters (slides 72–74). For the LTC term, the course transforms the polygon by `M⁻¹` before integrating (slide 63). Ludus adopts the table-free sphere approximation and the quadratic fit for both terms, because a target needs only a positive weight that tracks the integral, not shading accuracy. |

Cited through those sources but **not separately reviewed in this pass**. Review
the cited sections before implementing the corresponding piece, and add their
attribution beside the code:

- John Snyder, “Area Light Sources for Real-Time Graphics,” Microsoft Research
  technical report MSR-TR-96-11, 1996: the exact horizon-clipped sphere form
  factor that Hill and Heitz tabulate. Ludus uses only their table-free
  approximation.
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
     the same invariant; the [approximate target](#approximate-target) is one.
5. **Resampled light selection.**
   - Draw `M` candidates `t_i ~ p_src` with replacement. The template's
     `RIS_CANDIDATE_COUNT` setting has a maximum of 32; RT1's sampler version 2
     fixes `M = 32`. With the [approximate target](#approximate-target), the
     template defaults to `M = 8` and RT1's version 3 fixes its own `M`.
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
template's `RIS_NORMALIZATION_CANDIDATE_COUNT` has a maximum of 16 and defaults to
4 with the [approximate target](#approximate-target); RT1's sampler version 2
fixes `K = 8`, and version 3 fixes its own `K`.

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
  - Graph passes select it with `"traversal": "ComputeBvh"`, the strategy added
    once at the shared
    [traversal boundary](../development/render-graph-authoring.md#reuse-ray-callbacks-in-compute-pipelines)
    rather than inside either template's callbacks. `TriangleLoop` remains a
    linear scan meant for small scenes.
  - A `NativeQuery` configuration can follow later as a separate configuration.
  - Browser execution stays outside the graph ray profile until that profile
    admits it.
- **Settings.** Finite, author-declared values:
  - `RIS_CANDIDATE_COUNT`: 8, range 1–32 (32 before the approximate target);
  - `RIS_NORMALIZATION_CANDIDATE_COUNT`: 4, range 1–16 (8 before it);
  - `RIS_TARGET`: 1, the [approximate target](#approximate-target); 0 evaluates
    the exact clipped integrals;
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
- The renderer owns the LTC table, shared with the polygonal-light LTC
  templates. Its 64 × 64 × 51 transforms (3.4 MB) are too large for shader
  constants, so shaders read the renderer's GPU copy from a read-only storage
  buffer through `ltc_table.slang`.
- Stage them through the engine ray helpers (`POLYGONAL_LIGHT_HELPERS` in
  `render_graph_ray.py` and the installed shader directory).
- The technique declares `scene_interface.polygonal_light_sampling`, in the same
  way `visibility_buffer` adds its include and textures. The wrapper then includes
  the helpers, and the renderer binds the table buffer at slot 13, which every
  backend leaves free, when the linked kernel's reflection keeps it. Version 25
  runtime packages record that binding. Older SDKs reject the key explicitly. The
  passes must be compute-invoked: native ray stages cannot bind the table
  ([use the table](../development/ltc-table.md#use-the-table)).

### Implementation notes

The template ships as
[`render_graphs/ris_projected_solid_angle`](../../modules/graphics/renderer/render_graphs/ris_projected_solid_angle/main.lurender.json).
Its usage is in the
[authoring guide](../development/render-graph-authoring.md#resample-many-area-lights-with-projected-solid-angle-sampling).
It follows the estimator, BSDF-hit decision, settings and random table above, with
these departures and choices:

- **Visibility budget.** The technique declares `visibility_queries: 2`, not 1,
  because the selected triangle's diffuse and LTC samples each trace a shadow ray.
- **Helper requirement.** The requirement is `scene_interface.polygonal_light_sampling: true`.
  It stages and includes the projected-solid-angle, LTC and reservoir primitives and
  the LTC table lookup, and binds the renderer's table buffer (see the shader
  helpers above). Packages without it keep their snapshots. The builder rejects a
  non-Boolean value, and older SDKs reject the unknown field.
- **`F0` slice.** `MakeReceiverLightingFrame` looks up the specular LTC with the
  receiver's `F0`, `0.04 · (1 − metallic) + baseColor · metallic`, so the
  table's Peters slice matches the albedo it scales.
- **Emitter alpha.** Light samples do not apply the emitter's own alpha cutoff,
  because the scene interface has no alpha-acceptance query. `veach-guibas-mis`
  light sampling behaves the same way. Emission maps, sidedness and occlusion are
  evaluated at the sampled point.
- **Diffuse albedo.** `a₀ = baseColor · (1 − metallic) · (1 − F₀)` per channel,
  Schlick's reflectance at normal incidence, clamped to 0.01. This settles the
  open question for the template only; RT1 may measure another.
- **Other MIS pairs.** Analytic and environment light samples and the BSDF sample
  use the same power heuristic as `W_L`, and delta lights keep weight one.
- **Consistent densities.** Both MIS sites evaluate `P̃` with
  `EvaluateEmissiveTriangleSelectionPmf`, the same target function and the
  densities at the normalized world direction, so `W_L` is one function of the
  direction. `Ẑ` is computed at most once per invocation, on first use.
- **Comparison baseline.** GPU tests compare against `veach-guibas-mis` cooked with
  every pass on the triangle loop, so neither template needs native ray tracing.
  - The two-sided, minimum-roughness fixture is compared with that template's MIS
    output instead of its light-only output. Area-uniform light sampling of a light
    2 cm below the ceiling has unbounded variance, and its 256-sample mean reads
    about 4% low. Its MIS and BSDF outputs agree with all three of this template's.
  - The MSE check measures error on the tone-mapped image, against that template's
    1024-sample MIS output with an independent seed. On decoded linear radiance, the
    pixels that see emitters directly dominate both errors: 0.079 against 0.080,
    compared with 0.0011 against 0.0025 tone-mapped on Apple M5.

Evidence and remaining acceptance work, against [Acceptance](#acceptance) item 3:

- **Tested.** The GPU tests on Metal cover the glossy Cornell room, the
  many-emitter room, a textured emitter and two-sided emitters over
  minimum-roughness metal. The weight unit test covers the MIS partition of unity,
  `Ẑ = 0` and the empty LTC clip.
  - The textured fixture puts a 4 × 4 linear emission map on the Cornell light,
    over rough dielectric surfaces. The map is white, warm, cool and black in
    diagonal stripes, so it is zero on a quarter of the light. The power CDF and the
    RIS target see only the material's constant `Emission`, so a light sample that
    ignored the map would bias the image.
  - At 256 samples per pixel on Apple M5, the three outputs' image means are within
    0.2% of the MIS template's light-sampling output. Without the map, the image mean
    rises from 0.116 to 0.204, so the map does reach every estimate.
- **Equal-time benchmark.** Measured; see below.
- **Not run.** Vulkan first runs in batch CI.

#### Equal-time benchmark

The opt-in Catch2 tag `[.ris-projected-solid-angle-benchmark]` in
`ludus_render_graph_ray_tests` measures Shah et al.'s claim: lower error at equal
time with many lights. Its
[usage](../development/render-graph-authoring.md#resample-many-area-lights-with-projected-solid-angle-sampling)
is in the authoring guide.

- **Scene.** The Cornell room has its light replaced by a 40 × 40 field of one-sided
  emitters 3 cm wide, 3200 emissive triangles in all. Four power levels spanning a
  factor of 64 and three tints vary between neighbors.
  - The diffuse variant has white rough dielectric surfaces (roughness 1).
  - The specular variant has white metal at roughness 0.2, which mirrors the emitter
    field. The two variants follow the paper's diffuse and glossy scenes.
- **Packages.** Both templates are cooked with every pass on `ComputeBvh`, so both
  use one traversal.
  - The BSDF-only program of each is replaced by a test program
    (`tests/render_graph_query_count.slang`). It runs the template's own
    `ShadeMultipleImportance` unchanged and outputs the number of visibility and
    surface queries it issued, which gives rays per pixel without instrumenting the
    template.
  - The MIS output is each template's own.
- **Timing.**
  - The image is 288 × 192: one RGBA8 readback slice holds at most 256 KiB, and
    the next size up of this aspect ratio does not fit.
  - Each package renders 8 warmup and 64 timed frames with per-pass GPU timing, in
    the order RIS, Veach–Guibas, Veach–Guibas, RIS. Its cost per sample is the mean
    of its two runs' median kernel times, primary plus MIS shade.
  - Accumulate and ToneMap are identical in both templates and are left out.
- **Error.**
  - The reference is the Veach–Guibas MIS output at 4096 samples per pixel, with an
    independent seed. RIS is read at 4, 16 and 64 samples. Veach–Guibas is read at
    the same samples, and at the same kernel time.
  - MAPE is `|x − r| / (r + 0.01)`. MAPE is the metric Shah et al. report; the 0.01
    guard against black reference channels is this benchmark's own. It is averaged
    with linear RMSE over the RGB channels of pixels whose decoded reference stays
    below 4.
  - Brighter pixels see emitters directly, and the 8-bit readback cannot resolve
    them; they are about 1.5% of the image. Tone-mapped MSE covers every pixel.
- **Failure pixels.** In the recorded run, both reference images showed three
  failure pixels, which every metric leaves out. They came from a `ComputeBvh`
  traversal defect, not from either estimator, and are now fixed; see the paragraph
  after the results.

Results on Apple M5 (Metal) with Slang and Clang 23 from `config/tool_versions.json`,
October 29, 2026. The machine was shared with other builds, at a load average of 70–95.

| Variant | Template | Primary + MIS kernel per sample | Shadow rays per pixel | BSDF rays per pixel |
| --- | --- | --- | --- | --- |
| Diffuse | `ris-projected-solid-angle` | 0.155 + 12.16 ms | 0.913 | 0.473 |
| Diffuse | `veach-guibas-mis` | 0.411 + 2.28 ms | 0.463 | 0.473 |
| Specular | `ris-projected-solid-angle` | 0.145 + 11.51 ms | 0.911 | 0.534 |
| Specular | `veach-guibas-mis` | 0.411 + 2.08 ms | 0.463 | 0.534 |

Rays are averaged over all pixels. About half the image is shaded surface: the BSDF
ray count equals the shaded fraction, so each shaded pixel traces about 1.9 shadow
rays with RIS and 1.0 with Veach–Guibas. One RIS sample costs 4.6 times as much
kernel time as a Veach–Guibas sample (diffuse), or 4.7 times (specular). In an
earlier run under heavier GPU contention, the diffuse ratio was about 9, so the
equal-time sample counts below are specific to this run.

Each cell gives MAPE / linear RMSE / tone-mapped MSE:

| Variant | RIS samples | RIS | Veach–Guibas, equal samples | Veach–Guibas, equal time (samples) |
| --- | --- | --- | --- | --- |
| Diffuse | 4 | 0.136 / 0.288 / 0.0088 | 0.357 / 0.339 / 0.0191 | 0.181 / 0.163 / 0.0044 (18) |
| Diffuse | 16 | 0.072 / 0.145 / 0.0021 | 0.192 / 0.172 / 0.0051 | 0.091 / 0.081 / 0.0010 (73) |
| Diffuse | 64 | 0.039 / 0.073 / 0.00046 | 0.098 / 0.087 / 0.0012 | 0.047 / 0.041 / 0.00025 (293) |
| Specular | 4 | 0.119 / 0.300 / 0.0086 | 0.274 / 0.349 / 0.0153 | 0.139 / 0.165 / 0.0033 (19) |
| Specular | 16 | 0.064 / 0.151 / 0.0020 | 0.151 / 0.178 / 0.0040 | 0.071 / 0.083 / 0.00076 (75) |
| Specular | 64 | 0.034 / 0.077 / 0.00044 | 0.077 / 0.090 / 0.00092 | 0.037 / 0.042 / 0.00019 (300) |

What the results show:

- **Equal samples.** RIS has lower error on every metric, in both variants. MAPE
  falls by a factor of 2.2–2.7 and tone-mapped MSE by 1.8–2.6. This repeats, at 3200
  emitters, the CTest MSE result for sixteen emitters.
- **Equal time.**
  - RIS keeps a lower MAPE, by 8–25%.
  - Veach–Guibas has the lower linear RMSE, by a factor of about 1.8, and the lower
    tone-mapped MSE, by 1.8–2.6.
  - MAPE weights relative error, which is dominated by dim regions that uniform
    emitter selection serves poorly. RMSE is dominated by bright pixels, where the
    extra Veach–Guibas samples pay off.
  - So, at equal time, this implementation reproduces the paper's claim only for
    MAPE.
- **Why RIS is expensive.** Its cost is the per-candidate target evaluation. Each of
  the 32 candidates clips and integrates two polygons, diffuse and LTC. Shah et al.
  evaluate an approximate form factor instead (Hill and Heitz). The
  [approximate target](#approximate-target) now does the same, and its
  [measurements](#approximate-target-measurements) include an `M` and `K` sweep.

The three failure pixels also appeared with the stock `ris-projected-solid-angle`
package, which showed one by sample 256 at another pixel with this seed.
- **Location.** They were on the ceiling, where the emitter quads are. Every output
  failed at the same pixel and sample, including the BSDF-only output, so the shared
  surface load failed, not a sampling strategy.
- **Not reproduced on the triangle loop.** With the same seed, the triangle-loop
  Veach–Guibas package showed no failure through sample 1920, while the ComputeBvh
  one failed by then.
- **Not a stack overflow.** `REFERENCE_BVH_MAX_DEPTH` bounds the traversal stack.
- **Cause.** `IntersectBvhTriangle` divided the edge functions separately. Near the
  edge opposite a triangle's first corner, the two float32 quotients could round to
  a sum above one. For these tiles that is an outer tile edge: their index order
  puts the shared diagonal through the first corner. `GetSurfaceHit` rejects that
  sum, so the pass latched a failure.
- **Fix.** The traversal now clamps both barycentrics to [0, 1] and bounds the
  third by one minus the second, so the float32 sum never exceeds one. The
  `ComputeBvh` regression in `render_graph_ray_tests.cpp` traces 6144 rays down a
  shared diagonal opposite both triangles' first corners. Before the fix, 118 of
  those hits failed the reload on Apple M5 (Metal). The
  [GPU-compute reference](renderer-systems.md#gpu-compute-reference) records this
  float32 departure.

**Registration** follows the existing template checklist:
- `TEMPLATES` and the CLI `--template` choices;
- the root CMake install list and the editor resource list and catalog (graph
  and snippet entries);
- the editor compute-template dispatch;
- the identity lists in the Python and editor template tests;
- the renderer GPU staging loop;
- a usage section in the authoring guide beside
  [Compare light and BSDF sampling with MIS](../development/render-graph-authoring.md#compare-light-and-bsdf-sampling-with-mis),
  added with the template.

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
| `ResampledFormFactor` | The same estimator with the [approximate target](#approximate-target). `SamplerVersion = 3`, version 2's random layout and its own frozen `M` and `K`. Requires the LTC table. |

- **API.** The `ReferenceLightSampling` enum on
  `ReferenceCaptureDescription`, recorded in `ReferenceCaptureInfo`.
  - `CpuOracle` and `NativeQueries` share the CPU transport; `GpuCompute` runs
    the same estimator in its kernels.
  - The setting joins the integral/accumulation key, so changing it builds a
    replacement job and resets accumulation.
  - The separate GPU preview may adopt the mode later. That does not count toward
    the reference requirement, which the preview does not yet satisfy.
- **LUHDR.** The sampler field carries 2 (3 for `ResampledFormFactor`), and new
  flag bit `128` marks a resampled mode (bit 64 already means `gpu_compute`). Today
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
- Sampler version 3 uses this table unchanged. Its own frozen `M` and `K` may only
  be smaller, so it reads a prefix of the candidate and normalization slots.

### RT1 implementation notes

Usage and evidence are in
[Light-sampling modes](renderer-systems.md#light-sampling-modes) and the
[GPU-compute reference](renderer-systems.md#gpu-compute-reference). The profile
follows the estimator, BSDF-hit decision and random table above, with these
choices:

- **Owners.** The CPU estimator is
  [`reference_light_sampling.cpp`](../../modules/graphics/renderer/src/reference_light_sampling.cpp),
  called from `reference_path.cpp`. `renderer_reference.slang` ports it, with the
  scene-independent pieces in
  [`reference_light_sampling.slang`](../../modules/graphics/renderer/shaders/reference_light_sampling.slang).
  The slot table and the frozen `M`, `K` are `renderer_reference_layout.h`
  constants. The template keeps its own copy of the scene-independent pieces, so
  either can change without renumbering the other; sharing one source is
  possible follow-up work.
- **Diffuse albedo and `v`.** As in the template: `baseColor · (1 − metallic) · (1 − F₀)`
  clamped to 0.01, and `v = 1/2`. Both remain open questions.
- **Emitter alpha.** Unlike the template, light samples apply the emitter's alpha
  cutoff at the sampled point, because RT1's `UniformArea` already does. The
  emitter's sidedness and the secondary range are applied there too.
- **One kernel.** The GPU-compute trace kernel serves every mode; a parameter flag
  selects sampler version 2, and a second flag (`128`) version 3's approximate
  target. The LTC table is the kernel's sixth binding, uploaded once per
  `EnableReferenceRendering` and charged to its GPU budget. Kernels without the
  binding are rejected there, so no capture falls back.
  `LUDUS_REFERENCE_KERNEL_VERSION` is 4 since version 3.
- **Shadow rays.** The CPU path queues up to two shadow rays per vertex, so
  `NativeQueries` tiles need three queries per path. The GPU kernel traces each
  technique's shadow ray as soon as it is sampled; the order of the sums matches
  the CPU's.
- **Probes.** Static-lighting diffuse probes reject the resampled mode, because
  their first receiver is virtual and has no stored frame.

Evidence against [Acceptance](#acceptance) item 4, on Apple M5 Metal:
- **Tested.** Each mode agrees with `UniformArea` across six seeds at depths 1, 3,
  4 and 6. CPU and GPU raw AOVs agree within the `UniformArea` parity tolerances.
  Both implementations cover `Ẑ = 0`, an empty LTC clip, emitters behind every
  receiver, finite contributions and weights that partition unity, in the CPU unit
  tests, the shader parity kernel and the capture tests.
- **`UniformArea`.** Captures stay bit-identical in both implementations.
- **Textured emitters.** Only the item 5 benchmark below has them. There the image
  means of both modes agree within 0.1%, but no test asserts that.
- **Not run.** Vulkan, WebGPU and `NativeQueries` captures of the resampled mode,
  and sanitizer builds, first run in batch CI.

#### RT1 equal-time evaluation

Evidence against [Acceptance](#acceptance) item 5. The opt-in Catch2 tag
`[.reference-light-sampling-benchmark]` in `ludus_renderer_render_tests`
([source](../../modules/graphics/renderer/tests/reference_light_sampling_benchmark.cpp))
renders RT1 in both modes. Its usage is under
[Light-sampling modes](renderer-systems.md#light-sampling-modes).

- **Scene.** The emitter field of the [template benchmark](#equal-time-benchmark),
  rebuilt as renderer meshes: the Cornell room lit by 3200 one-sided emissive
  triangles with power spanning a factor of 64.
  - One tint's emitters, a third of the field, multiply their emission by a 4 × 4
    checker of 1 and 0.25. The power CDF and the target use the constant emission
    factor, which bounds the textured emission from above.
  - The surface finishes match the template's diffuse and specular variants. Each
    finish runs with direct lighting only (`MaxDepth` 1) and at depth 4.
- **Timing.**
  - Each mode renders 32 samples per pixel of the 288 × 192 image twice, in the
    order `UniformArea`, resampled, resampled, `UniformArea`.
  - Its cost per sample is the mean of the two runs' median device times of the
    GPU-compute transport pass. Each pass traces one sample of every pixel.
  - Shadow and closest rays per pixel come from `ReferenceCaptureInfo`'s visibility
    and closest query counts.
- **Error.**
  - The reference is a GPU-compute `UniformArea` image at 8192 samples per pixel,
    with an independent seed.
  - The resampled mode is read at 4, 16 and 64 samples. `UniformArea` is read at the
    same samples, and at the same transport kernel time.
  - Metrics cover the linear `Direct + Indirect` AOVs of every pixel and RGB channel:
    MAPE `|x − r| / (r + 0.01)`, with the template's guard, and RMSE. Float AOVs need
    no tone-mapped readback or saturation cut.
  - Primary emission is left out: both modes evaluate it bit-identically from
    unchanged random dimensions.
- **CPU oracle.** The diffuse room at 96 × 64 renders 4 and 16 samples per pixel in
  both modes, at depths 1 and 4, against a GPU-compute `UniformArea` reference at
  8192 samples. Larger extents and the equal-time comparison are GPU-only.

Results on Apple M5 (Metal, macOS 26.6), with the `macos-clang-development` build
(Clang 23.1.3, Slang 2026.1.2), October 29, 2026. The machine was shared, at a load
average of 5–23. The tag ran three times, taking 4–7 minutes each. Runs 1 and 2
built `develop` at `da35d03e`; run 3 built `a34fccf1`, the base of this change. All
three runs produced bit-identical references and equal-sample images.

| Case | `UniformArea` kernel per sample | Resampled kernel per sample | Cost ratio | Shadow rays per pixel, `UniformArea` / resampled |
| --- | --- | --- | --- | --- |
| Diffuse, direct | 1.13–1.15 ms | 20.5–21.0 ms | 17.9–18.3 | 0.458 / 0.916 |
| Specular, direct | 0.88–1.29 ms | 20.6–22.1 ms | 16.7–25.2 | 0.458 / 0.913 |
| Diffuse, depth 4 | 2.75–4.14 ms | 85.0–90.1 ms | 20.5–32.7 | 0.992 / 1.981 |
| Specular, depth 4 | 2.29–4.00 ms | 81.6–86.0 ms | 20.4–37.6 | 1.055 / 2.099 |

Closest rays per pixel are equal in both modes: 1.40–1.44 with direct lighting only
and 1.97–2.12 at depth 4. As in the template benchmark, rays are averaged over all
pixels.

Each error cell gives MAPE / RMSE. The two equal-time columns are runs 1 and 2,
whose cost ratios differ. Run 3 has the same ordering in every cell.

| Case | Samples | Resampled | `UniformArea`, equal samples | `UniformArea`, equal time, run 1 (samples) | Run 2 (samples) |
| --- | --- | --- | --- | --- | --- |
| Diffuse, direct | 4 | 0.083 / 0.064 | 0.164 / 0.140 | 0.043 / 0.033 (73) | 0.043 / 0.033 (73) |
| Diffuse, direct | 16 | 0.042 / 0.031 | 0.088 / 0.071 | 0.022 / 0.017 (292) | 0.022 / 0.017 (293) |
| Diffuse, direct | 64 | 0.022 / 0.017 | 0.045 / 0.036 | 0.011 / 0.0091 (1166) | 0.011 / 0.0091 (1171) |
| Specular, direct | 4 | 0.062 / 0.116 | 0.137 / 0.191 | 0.030 / 0.040 (101) | 0.037 / 0.048 (67) |
| Specular, direct | 16 | 0.032 / 0.057 | 0.073 / 0.096 | 0.016 / 0.020 (403) | 0.019 / 0.024 (268) |
| Specular, direct | 64 | 0.017 / 0.029 | 0.038 / 0.049 | 0.0084 / 0.011 (1610) | 0.010 / 0.013 (1070) |
| Diffuse, depth 4 | 4 | 0.186 / 0.093 | 0.262 / 0.180 | 0.067 / 0.037 (92) | 0.057 / 0.031 (131) |
| Diffuse, depth 4 | 16 | 0.098 / 0.047 | 0.147 / 0.090 | 0.036 / 0.019 (367) | 0.030 / 0.016 (524) |
| Diffuse, depth 4 | 64 | 0.051 / 0.025 | 0.079 / 0.045 | 0.019 / 0.010 (1467) | 0.017 / 0.0087 (2095) |
| Specular, depth 4 | 4 | 0.286 / 0.141 | 0.398 / 0.235 | 0.125 / 0.038 (150) | 0.159 / 0.052 (82) |
| Specular, depth 4 | 16 | 0.183 / 0.071 | 0.274 / 0.116 | 0.069 / 0.019 (602) | 0.090 / 0.026 (327) |
| Specular, depth 4 | 64 | 0.108 / 0.036 | 0.174 / 0.058 | 0.040 / 0.011 (2406) | 0.050 / 0.014 (1307) |

On the CPU oracle at 96 × 64, with the same field and the diffuse finish:

| Depth | Samples | `UniformArea` | Resampled |
| --- | --- | --- | --- |
| 1 | 4 | 0.184 / 0.141 | 0.104 / 0.064 |
| 1 | 16 | 0.098 / 0.061 | 0.054 / 0.036 |
| 4 | 4 | 0.279 / 0.168 | 0.207 / 0.095 |
| 4 | 16 | 0.159 / 0.081 | 0.110 / 0.048 |

In run 1, the oracle needed 137 ms of wall time for 16 `UniformArea` samples per
pixel at depth 1 and 1516 ms for 16 resampled samples. At depth 4 it needed 103 ms
and 1110 ms. These times include setup and are not equal-time measurements.

What the results show:

- **Equal samples.** The resampled mode has lower error in every case and in both
  implementations.
  - With direct lighting only, MAPE falls by 2.0–2.3× and RMSE by 1.6–2.3×.
  - At depth 4, MAPE falls by 1.4–1.6× and RMSE by 1.6–1.9×. Indirect bounces gain
    less than direct light.
- **Equal time.** `UniformArea` has lower error in every case and in all three runs.
  - Its MAPE is 1.7–2.1× lower with direct lighting only, and 1.8–3.3× lower at
    depth 4. Its RMSE is 1.8–2.9× and 2.3–3.7× lower.
  - Equal error per unit time needs a cost ratio near the square of the
    equal-sample error ratio: about 3–5 with direct lighting only, and 2–4 at depth
    4. The measured ratio is 17–38.
- **Unlike the template.** There, RIS kept a lower MAPE at equal time against
  Veach–Guibas. Two differences probably explain the change; neither was measured
  on its own:
  - RT1's `UniformArea` already selects triangles by power, where the template's
    Veach–Guibas baseline selects emitters uniformly. RT1's baseline therefore has
    less to gain from resampling.
  - With direct lighting only, as in the template, RT1's `UniformArea` transport
    costs 0.9–1.3 ms per sample, against 2.7 ms for the template's Veach–Guibas
    primary and MIS kernels. The resampled transport costs 20–22 ms, against
    12.3 ms for the template's RIS.
- **Cost.** It comes from the per-vertex target evaluations; see the template's
  explanation in [Equal-time benchmark](#equal-time-benchmark). The
  `ResampledFormFactor` mode lowers it; see
  [Approximate target measurements](#approximate-target-measurements).
- **Bias.** At 64 samples, the resampled image's mean transport luminance is within
  0.1% of the reference in each case: 0.07359 against 0.07358, 0.04461 against
  0.04464, 0.12527 against 0.12519, and 0.07978 against 0.07972. The textured
  emitters give no measurable bias, but this is not a test.

## Approximate target

Implemented October 29, 2026, as the template's `RIS_TARGET` setting and RT1's
`ResampledFormFactor` mode (sampler version 3). It replaces the exact `E₀` and `E₁`
of the [target](#proposed-estimator) by Hill and Heitz's approximation (see
[Sources reviewed](#sources-reviewed)), which Shah et al. use for speed.

- **Approximation.** For the triangle's unit vertex directions `dᵢ`, the vector
  form factor is `F = (1/2π) Σ r(dᵢ·dᵢ₊₁) dᵢ × dᵢ₊₁`, oriented towards the triangle,
  where `r` is the course's quadratic fit of θ/sin θ (within 0.73%). The integral is
  `F.z` when no vertex lies below the horizon, zero when none lies above it, and
  otherwise the clipped proxy sphere `max((|F|² + F.z) / (|F| + 1), 0)`. `E₁` applies
  the same steps after transforming the vertices by `M₁⁻¹`. FoundationMath owns both
  as `TryApproximateClampedCosinePolygonIntegral` and
  `TryApproximateLinearlyTransformedCosinePolygonIntegral`, in C++ and Slang.
- **Departure.** Using `F.z` for triangles entirely above the horizon reproduces
  the course's tabulated variant there, where the table-free fit would not.
  Results are clamped to `[0, 1]`, which the fitted ratio can exceed slightly.
- **Support.** The proxy sphere can lie below the horizon while part of the
  triangle is above it, so the approximate `E₀` alone can be zero where the exact
  one is positive. The target therefore uses
  `Ẽ₀ = max(E₀ approximation, 2⁻⁶ |d₀ · (d₁ × d₂)|)` whenever a vertex lies above the
  horizon. The triple product is positive for every triangle whose plane does not
  contain the receiver, which is exactly when the exact projected solid angle can be
  positive. So `p̂ > 0` wherever the triangle has area above the horizon and positive
  emission, and the estimator's expected value is unchanged. The FoundationMath, RT1
  unit and template weight tests check this on random triangles, including ones the
  sphere alone would drop.
- **What stays exact.** The candidate weights, `Ẑ` and `P̃` all use the approximate
  `p̂`, at both MIS sites. The selected triangle and a BSDF hit's triangle still
  clip and prepare both polygons, because their samples, densities `p₀`, `p₁` and
  Peters' coefficients `c_t` need the exact integrals. Changing the target changes
  only variance and cost.
- **Cost per vertex.** The exact target prepares two polygons for each of `M + K`
  candidates and for the selected triangle. The approximate one evaluates three
  fitted edge terms twice per candidate and prepares polygons only for the selected
  triangle and BSDF hits.
- **Identity.** RT1's version 2 keeps the exact target and its captures. Version 3
  shares version 2's random slots and reads a prefix of its candidate and
  normalization slots, with frozen `M` and `K` chosen from the
  [measurements](#approximate-target-measurements). The LUHDR sampler field records
  3, with the resampled flag 128. The template's `RIS_TARGET` defaults to the
  approximation; `0` restores the exact target.

### Approximate target measurements

Both opt-in benchmarks measured the approximate target with the method of
[Equal-time benchmark](#equal-time-benchmark) and
[RT1 equal-time evaluation](#rt1-equal-time-evaluation), on Apple M5 (Metal,
macOS 26.6), `macos-clang-development` with Clang 23.1.3 and Slang 2026.1.2, on
October 29, 2026. The machine was shared with other builds, at a load average of
22–78. Kernel times of one package varied by up to 1.7× between runs under that
load, so costs are compared within one run.

**Template.** One run of `[.ris-projected-solid-angle-benchmark]` cooked the exact
target and seven `RIS_TARGET = 1` variants
(`LUDUS_RIS_PROJECTED_SOLID_ANGLE_BENCHMARK_VARIANTS`), all measured against the
same Veach–Guibas reference and its own timing rounds. In this run Veach–Guibas's
MIS shade kernel took 0.56–0.69 ms per sample, against 2.08–2.28 ms in the earlier
recorded run, so its equal-time sample counts are three times as high as there.

Primary plus MIS shade kernel per sample, and its ratio to Veach–Guibas:

| Target | `M` | `K` | Diffuse | Specular |
| --- | --- | --- | --- | --- |
| Exact | 32 | 8 | 13.25 ms (16.4×) | 12.13 ms (18.1×) |
| Form factor | 32 | 8 | 3.33 ms (4.1×) | 3.07 ms (4.6×) |
| Form factor | 16 | 8 | 2.60 ms (3.2×) | 2.37 ms (3.5×) |
| Form factor | 16 | 4 | 2.51 ms (3.1×) | 2.14 ms (3.2×) |
| Form factor | 8 | 8 | 2.21 ms (2.7×) | 2.13 ms (3.2×) |
| **Form factor** | **8** | **4** | **1.95 ms (2.4×)** | **1.80 ms (2.7×)** |
| Form factor | 8 | 2 | 1.97 ms (2.4×) | 1.82 ms (2.7×) |
| Form factor | 4 | 8 | 1.88 ms (2.3×) | 1.70 ms (2.5×) |
| Veach–Guibas | — | — | 0.81 ms | 0.67 ms |

Shadow and BSDF rays per pixel do not depend on the target: 0.91 and 0.47
(diffuse), 0.91 and 0.53 (specular).

At 64 samples per pixel, each cell gives MAPE / linear RMSE / tone-mapped MSE, then
Veach–Guibas at the same kernel time, with its sample count:

| Target, `M`, `K` | Diffuse | Veach–Guibas, equal time | Specular | Veach–Guibas, equal time |
| --- | --- | --- | --- | --- |
| Exact, 32, 8 | 0.0387 / 0.0734 / 0.00046 | 0.0271 / 0.0248 / 0.000084 (1049) | 0.0341 / 0.0771 / 0.00044 | 0.0207 / 0.0249 / 0.000058 (1157) |
| Form factor, 32, 8 | 0.0389 / 0.0734 / 0.00046 | 0.0498 / 0.0431 / 0.00028 (263) | 0.0342 / 0.0771 / 0.00044 | 0.0373 / 0.0428 / 0.00019 (293) |
| Form factor, 16, 8 | 0.0409 / 0.0743 / 0.00048 | 0.0559 / 0.0488 / 0.00037 (206) | 0.0362 / 0.0788 / 0.00046 | 0.0422 / 0.0484 / 0.00025 (226) |
| Form factor, 16, 4 | 0.0409 / 0.0743 / 0.00048 | 0.0568 / 0.0494 / 0.00038 (198) | 0.0362 / 0.0789 / 0.00046 | 0.0444 / 0.0504 / 0.00028 (204) |
| Form factor, 8, 8 | 0.0438 / 0.0755 / 0.00051 | 0.0603 / 0.0527 / 0.00043 (175) | 0.0390 / 0.0804 / 0.00049 | 0.0444 / 0.0504 / 0.00028 (204) |
| **Form factor, 8, 4** | **0.0439 / 0.0755 / 0.00051** | **0.0640 / 0.0559 / 0.00049 (155)** | **0.0390 / 0.0804 / 0.00049** | **0.0479 / 0.0541 / 0.00033 (172)** |
| Form factor, 8, 2 | 0.0439 / 0.0755 / 0.00051 | 0.0638 / 0.0558 / 0.00049 (156) | 0.0390 / 0.0804 / 0.00049 | 0.0478 / 0.0541 / 0.00033 (173) |
| Form factor, 4, 8 | 0.0478 / 0.0770 / 0.00056 | 0.0652 / 0.0572 / 0.00051 (149) | 0.0429 / 0.0829 / 0.00053 | 0.0493 / 0.0559 / 0.00036 (162) |

Veach–Guibas at 64 samples measured 0.0976 / 0.0867 / 0.0012 (diffuse) and
0.0767 / 0.0902 / 0.00092 (specular). The new defaults, `M = 8` and `K = 4`, at
fewer samples:

| Variant | Samples | Default RIS | Veach–Guibas, equal time (samples) |
| --- | --- | --- | --- |
| Diffuse | 4 | 0.157 / 0.295 / 0.0095 | 0.237 / 0.214 / 0.0082 (10) |
| Diffuse | 16 | 0.083 / 0.149 / 0.0023 | 0.124 / 0.112 / 0.0019 (39) |
| Specular | 4 | 0.141 / 0.316 / 0.0095 | 0.179 / 0.213 / 0.0061 (11) |
| Specular | 16 | 0.074 / 0.159 / 0.0022 | 0.093 / 0.110 / 0.0013 (43) |

What the template results show:

- **Same error, a quarter of the cost.** At `M = 32`, `K = 8` the approximate
  target has the exact target's error to within 1% at every budget, for 25% of its
  kernel time. Its equal-sample gain over Veach–Guibas (MAPE 2.2–2.6×) is unchanged.
- **`M` and `K`.** Fewer candidates raise equal-sample error slowly (MAPE +13% at
  `M = 8`, +23% at `M = 4`) and lower cost. `K` changes no error at three
  significant digits and costs about 0.1–0.35 ms per sample from 8 to 4. MAPE ×
  √time is lowest at `M = 8`, `K = 4` in both variants, within about 7% of
  `M = 16` and `M = 4`; `K = 2` measured no further gain. These are the new
  defaults.
- **Equal time.** With the defaults, RIS has a lower MAPE than Veach–Guibas at every
  budget: 31–34% lower (diffuse) and 19–22% lower (specular). With the exact target
  in the same run it was 42–65% *higher*, because this run's Veach–Guibas kernel was
  fast. Veach–Guibas keeps the lower linear RMSE, by 1.3–1.5×, and the lower
  tone-mapped MSE, by 1.04–1.19× (diffuse) and 1.5–1.7× (specular).
- **What limits RMSE.** RIS's linear RMSE barely moves with `M`, `K` or the target
  (0.073–0.077 diffuse at 64 samples), while MAPE does. The squared error therefore
  comes from pixels where emitter selection is not the bottleneck; which term of
  the estimator dominates there was not measured.

**RT1.** `[.reference-light-sampling-benchmark]` renders all three modes in each
run, timed in the order `UniformArea`, version 2, version 3, version 3, version 2,
`UniformArea`. Version 3's `M` and `K` are compile-time constants, so each setting
needs its own build and run. Equal-sample errors are deterministic: every run that
renders a given mode and setting reproduces them bit for bit.

Transport kernel time per sample in run 1 (version 3 with `M = 32`, `K = 8`), and
its ratio to `UniformArea`:

| Case | `UniformArea` | Version 2, exact target | Version 3, form factor |
| --- | --- | --- | --- |
| Diffuse, direct | 1.10 ms | 21.8 ms (19.7×) | 4.15 ms (3.8×) |
| Specular, direct | 1.63 ms | 20.5 ms (12.6×) | 4.42 ms (2.7×) |
| Diffuse, depth 4 | 3.72 ms | 85.6 ms (23.0×) | 16.8 ms (4.5×) |
| Specular, depth 4 | 3.40 ms | 87.8 ms (25.8×) | 18.1 ms (5.3×) |

Shadow rays per pixel are those of version 2 within 0.2%. The specular
`UniformArea` time above is high for this machine. In other runs it measured
0.74–2.66 ms, so its ratios are less certain than the others.

Version 3 against `UniformArea` at the same kernel time in run 1, as MAPE / RMSE
(`UniformArea` samples in parentheses):

| Case | Samples | Version 3 | `UniformArea`, equal time |
| --- | --- | --- | --- |
| Diffuse, direct | 4 | 0.084 / 0.064 | 0.090 / 0.074 (15) |
| Diffuse, direct | 16 | 0.042 / 0.031 | 0.047 / 0.037 (60) |
| Diffuse, direct | 64 | 0.022 / 0.017 | 0.024 / 0.019 (240) |
| Specular, direct | 4 | 0.063 / 0.116 | 0.088 / 0.115 (11) |
| Specular, direct | 16 | 0.032 / 0.057 | 0.045 / 0.060 (43) |
| Specular, direct | 64 | 0.017 / 0.029 | 0.024 / 0.031 (173) |
| Diffuse, depth 4 | 4 | 0.186 / 0.093 | 0.139 / 0.084 (18) |
| Diffuse, depth 4 | 16 | 0.098 / 0.047 | 0.075 / 0.042 (72) |
| Diffuse, depth 4 | 64 | 0.051 / 0.025 | 0.040 / 0.021 (289) |
| Specular, depth 4 | 4 | 0.286 / 0.141 | 0.253 / 0.102 (21) |
| Specular, depth 4 | 16 | 0.183 / 0.071 | 0.156 / 0.051 (85) |
| Specular, depth 4 | 64 | 0.108 / 0.036 | 0.088 / 0.026 (340) |

Version 3's equal-sample errors match version 2's within 1% in every case, as in the
template. For comparison, version 2 at equal time in the same run lost by 1.5–2.8×
in MAPE.

Equal-sample MAPE / RMSE at 64 samples for each version 3 setting measured, with
`UniformArea` at 64 samples:

| Case | `M = 32`, `K = 8` | `M = 16`, `K = 8` | `M = 8`, `K = 4` | `UniformArea` |
| --- | --- | --- | --- | --- |
| Diffuse, direct | 0.0221 / 0.0171 | 0.0239 / 0.0200 | 0.0266 / 0.0226 | 0.0453 / 0.0358 |
| Specular, direct | 0.0168 / 0.0289 | 0.0188 / 0.0313 | 0.0217 / 0.0355 | 0.0375 / 0.0494 |
| Diffuse, depth 4 | 0.0514 / 0.0247 | 0.0543 / 0.0275 | 0.0582 / 0.0304 | 0.0793 / 0.0446 |
| Specular, depth 4 | 0.108 / 0.0359 | 0.119 / 0.0395 | 0.132 / 0.0447 | 0.174 / 0.0582 |

On the CPU oracle at 96 × 64 (diffuse room, run 1), version 3 also matched version
2's error. Its wall time to 16 samples was 1.6 s against 2.4 s with direct lighting,
and 3.6 s against 5.0 s at depth 4. These times include setup and are not
equal-time measurements; the oracle's exact polygon preparation for the selected
triangle and BSDF hits is a larger share of its time.

Run 2 repeated `M = 32`, `K = 8` on this change's final code, merged with
`develop`. It reproduced every equal-sample error of run 1 bit for bit. Its
`UniformArea` kernel times differed by up to 1.7×: 0.65, 1.60, 5.00 and 4.86 ms
per sample in the four cases. Version 3 took 4.26, 4.17, 18.7 and 16.9 ms. At equal
time (MAPE / RMSE, `UniformArea` samples in parentheses):

| Case | Samples | Version 3 | `UniformArea`, equal time |
| --- | --- | --- | --- |
| Diffuse, direct | 64 | 0.022 / 0.017 | 0.018 / 0.015 (419) |
| Specular, direct | 64 | 0.017 / 0.029 | 0.024 / 0.031 (167) |
| Diffuse, depth 4 | 64 | 0.051 / 0.025 | 0.043 / 0.023 (240) |
| Specular, depth 4 | 64 | 0.108 / 0.036 | 0.106 / 0.032 (223) |

Because the equal-sample errors are deterministic, the cost ratio at which version
3 breaks even with `UniformArea` follows from them: the squared ratio of their
64-sample errors. A measured cost ratio below it wins at equal time:

| Case | Break-even cost ratio, RMSE / MAPE | Measured, run 1 / run 2 |
| --- | --- | --- |
| Diffuse, direct | 4.4 / 4.2 | 3.8 / 6.5 |
| Specular, direct | 2.9 / 5.0 | 2.7 / 2.6 |
| Diffuse, depth 4 | 3.3 / 2.4 | 4.5 / 3.7 |
| Specular, depth 4 | 2.6 / 2.6 | 5.3 / 3.5 |

Version 2's measured ratios were 12.6–30.6 in the same runs.

What the RT1 results show:

- **Cost.** The approximate target cuts version 2's transport kernel time by
  4.6–5.3× at the same `M` and `K`, with the same equal-sample error.
- **Equal time.** Version 3 moves RT1's resampled mode from a clear loss to
  about break-even, so the outcome depends on this machine's timing noise.
  - **Specular, direct.** Version 3 wins in both runs, by 28–30% in MAPE and 6–8%
    in RMSE at 64 samples.
  - **Diffuse, direct.** The cost ratio straddles break-even. Version 3 won in run 1
    (by 7% in MAPE, 9% in RMSE at 64 samples) and lost in run 2 (by 20% and 17%),
    where `UniformArea`'s kernel ran 1.7× faster.
  - **Depth 4.** `UniformArea` still wins, by up to 1.3× in MAPE and 1.4× in RMSE.
    In run 2's specular room the MAPE was nearly level (2%). Each indirect vertex pays for
    resampling, and indirect bounces gain less from it.
  - **Version 2.** It lost every case by 1.5–2.8× in MAPE.
  So Shah et al.'s equal-time claim holds in RT1 for specular direct lighting. The
  other cases now measure 0.9–2.0× their break-even cost, against 2.5–10× for
  version 2.
- **`M` and `K`.** Fewer candidates raise RT1's error faster than the template's.
  From `M = 32` to `M = 8`, RMSE² grows by 1.5–1.75×, so `M = 8` would have to cost
  under 0.57–0.66× as much; it measured about 0.7× at best, because the candidates
  are only part of the kernel. Timing noise between runs is too large to separate
  `M = 16` from `M = 32` on time: the same `M = 8` kernel measured 3.0 and 4.7 ms
  with direct lighting. `M = 16`'s RMSE² is 17–37% higher, for about half of the
  time `M = 8` saves. Version 3 therefore freezes `M = 32` and `K = 8`, version 2's
  counts. `K = 4` at `M = 32` was not measured in RT1.



## Ownership

| Piece | Owner | Notes |
| --- | --- | --- |
| Convex polygon horizon clipping (up to seven input vertices), clipped projected-solid-angle integral, Peters sampler and density | FoundationMath `projected_solid_angle.hpp` and `shaders/projected_solid_angle.slang`, implemented by the polygonal-light work | The sampler is ported from the BSD-licensed `polygon_sampling.glsl` with its notice. Clipping is an independent reimplementation. This design consumes these primitives and adds none of its own. |
| Generic LTC polygon transform | FoundationMath, beside those primitives | Implemented as `linearly_transformed_cosine.hpp` and `shaders/linearly_transformed_cosine.slang` ([contract](../modules/foundation/math.md#linearly-transformed-cosines-over-polygons)): transform the vertices by `M⁻¹`, then reuse the same clipping, integral and sampler. Credits Heitz et al. 2016 (§3.1, Eq. 3, Algorithm 1) and Peters 2021 §4.1. |
| Sequential weighted reservoir | FoundationMath | Implemented as `weighted_reservoir.hpp` and `shaders/weighted_reservoir.slang` ([contract](randomness.md#sequential-weighted-reservoir)): explicit order, zero totals, and selection-frequency tests on exact small integers. The ReSTIR template may adopt it separately; this design does not change that template. |
| LTC table for Ludus GGX/correlated Smith/Schlick, its fitter and provenance | Renderer (embedded table and a GPU buffer) plus an offline Ludus tool. **Owned by this RIS/many-area-light workstream** (decided October 27, 2026). The polygonal-light LTC templates reuse the same table and fitter rather than building their own. | Implemented as `tools/ltc-fit`, `data/ltc_table.bin` with its provenance and `shaders/ltc_table_layout.h`, and the `ltc_table.slang` and CPU lookups ([fit, regeneration and accuracy](../development/ltc-table.md)). Reimplemented from Heitz et al. §4 and Peters §§4.1–4.2, not from GPLv3 utilities. Like Peters' renderer it has 51 `F0` slices, chosen by the luminance of `F0`; the albedo stays exact per channel. It covers roughness down to 0.045. A poor fit costs variance, not correctness. Render graphs bind it with `scene_interface.polygonal_light_sampling`. |
| Compute-BVH graph traversal strategy | Renderer, at the shared traversal boundary | Implemented as `ComputeBvh`, sharing the RT1 GPU reference's BVH traversal. Shared with the polygonal-light templates; a prerequisite for both |
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
- Peters' `v`; `K` is settled for the template (`K = 4` changed no error at three
  significant digits; see [Approximate target measurements](#approximate-target-measurements))
  but not measured below 8 in RT1;
- the diffuse albedo approximation;
- LTC table resolution (the table follows Peters' 64 × 64 × 51 cells; see
  [its accuracy](../development/ltc-table.md#accuracy));
- whether two shadow rays beat Peters' one-sample variant for this workload.
