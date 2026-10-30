# LTC table fitting and accuracy

The renderer owns a table of linearly transformed cosines (LTCs) fitted to its
specular BSDF. The [many-area-light resampling estimator](../architecture/many-area-light-sampling-reference-review.md#ownership)
and the polygonal-light LTC templates share it. Each uses the table to guide
specular sampling of an emissive polygon and to weight lights. The BSDF itself
is always evaluated exactly, so an inaccurate fit costs variance, not
correctness.

The table, its fitter and their provenance belong to the RIS/many-area-light
workstream, as decided on October 27, 2026. The polygonal-light templates reuse
them and do not fit their own.

| File | Role |
| --- | --- |
| `tools/ltc-fit/` (`ludus_ltc_fit`) | Offline fitter |
| `modules/graphics/renderer/data/ltc_table.bin` | Fitted table, 3.4 MB of little-endian float32 values |
| `modules/graphics/renderer/data/ltc_table_provenance.json` | Fitter version, parameters and the residual error of every transform |
| `modules/graphics/renderer/shaders/ltc_table_layout.h` | Generated layout constants shared by C++ and Slang, installed with the renderer shaders |
| `cmake/shaders/generate_ltc_table.py` | Build step that checks the binary against the layout and embeds it in the renderer as C++ |
| `modules/graphics/renderer/shaders/ltc_table.slang` | Shader lookup from a bound buffer, installed with the renderer shaders |
| `modules/graphics/renderer/src/internal/ltc_table.h` | CPU lookup, for the RT1 CPU oracle and the tests |
| `modules/graphics/renderer/src/internal/ltc_table_data.h` | The embedded table, which the renderer also uploads for shaders |

FoundationMath's [LTC polygon transform](../modules/foundation/math.md#linearly-transformed-cosines-over-polygons)
integrates and samples the looked-up LTC inside a polygon.

## Sources and licensing

The fit follows Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
"Real-Time Polygonal-Light Shading with Linearly Transformed Cosines",
*ACM Transactions on Graphics* 35(4), Article 41, 2016, SIGGRAPH 2016
([DOI](https://doi.org/10.1145/2897824.2925895)), section 4. From there it takes:

- the cosine-weighted BRDF as the target (Equation 5);
- the four-parameter matrix of Equation 6;
- the L3 error;
- storage of `M⁻¹` and the BRDF norm over `(θ, √α)`.

Christoph Peters, "BRDF Importance Sampling for Polygonal Lights",
*ACM Transactions on Graphics* 40(4), Article 140, 2021
([DOI](https://doi.org/10.1145/3450626.3459672)), sections 4.1–4.2, explains how
the table drives sampling, and why the specular albedo is stored beside the
transform. Its renderer uses a 64 × 64 × 51 table whose third axis is Schlick's
`F0` (section 4.1, page 9); Ludus follows that resolution.

Both papers' published LTC tables and utilities are licensed under the GPL, and
Ludus neither copies nor imports them. Ludus reimplements the method from the
papers' text and fits its own table.

## What is fitted

The target is the specular lobe of `EvaluateReferenceBsdf`
(`modules/graphics/renderer/src/reference_bsdf.cpp`) and of the GPU preview's
`Evaluate` (`renderer_gpu_preview.slang`):

- GGX with `α = roughness²`;
- the height-correlated Smith visibility `0.5 / (n·l Λv + n·v Λl)`;
- Schlick's Fresnel with `F0 = 0.04 (1 − metallic) + baseColor · metallic`.

The fitter repeats that formula in float64 rather than linking the renderer.

As in Peters' renderer, each transform fits the lobe with Schlick's Fresnel for
one scalar `F0`, so the shape follows the Fresnel weighting. Beside the
transforms, the table stores two `F0`-independent moments of the exact lobe:

- the norm `n = ∫ ρ cos`;
- the Fresnel norm `f = ∫ (1 − v·h)⁵ ρ cos`.

Schlick's Fresnel is linear in `F0`, so the specular albedo is exactly
`F0 · (n − f) + f` for each channel. `ComputeSpecularLtcAlbedo` returns it.
Peters tabulates the albedo per `F0` slice instead; the moments keep it exact for
colored `F0`.

**Layout.** `ltc_table_layout.h` defines the counts. The table is a sequence of
four-float entries:

- Transforms: 51 `F0` slices, linear over `[0, 1]` (the paper gives the count but
  not the spacing; Peters' supplemental renderer also spaces them evenly with
  both endpoints). Each slice has 64 rows, linear in perceptual roughness from
  0.045, the renderer's admitted minimum, to 1, and 64 columns, linear in the
  view inclination `θ` over `[0, π/2]`, as in Heitz et al. Each entry is
  `(M02, M11, M20, M22)` of `M⁻¹ ∝ [[1, 0, M02], [0, M11, 0], [M20, 0, M22]]`, in
  the frame whose x-z plane contains the view.
- Moments: one 64 × 64 grid of `(n, f, 0, 0)` at `LUDUS_LTC_TABLE_MOMENT_OFFSET`.

**Lookup.** As in Peters' renderer, the lookup reduces an RGB `F0` to its
Rec.709 luminance, clamps it to `[0, 1]` and takes the nearest slice
(`GetLtcTableFresnelSlice`). Within the slice it interpolates bilinearly over
roughness and inclination, then rotates `M⁻¹` by the view's azimuth into the
caller's tangent frame, so the result can go straight to
`TryMakeLinearlyTransformedCosine`.

## Fitting procedure

These parts of the procedure are Ludus choices; the papers do not specify them.

- **Objective.** Heitz et al.'s L3 error between the LTC and the lobe
  `ρ F cos` normalized by its own norm `F0 · (n − f) + f`, over the sphere. It
  is estimated from three fixed, stratified sample sets of 32 × 32 points,
  combined with the balance heuristic:
  - visible-normal samples of the BRDF (Heitz, JCGT 7(4), 2018);
  - uniform samples of the upper hemisphere;
  - cosine samples mapped through `M`.

  With a small `F0`, Schlick's `(1 − v·h)⁵` moves much of the lobe far from the
  mirror direction, where visible-normal samples are rare; the uniform samples
  cover it. Fixed samples make the objective deterministic and smooth across
  cells.
- **Optimizer.** Nelder–Mead over `(log A, B, log C, D)`, with three restarts of
  at most 1500 iterations. The simplex steps scale with the lobe's width. The
  search is confined to `A, C ∈ [10⁻⁶, 10⁶]` and `|B|, |D| ≤ 10³`: at a small
  `F0` and smooth roughness an unconfined fit stretches the LTC towards the
  horizon until its stored entries overflow. The `F0 = 1` slice stays within
  `A ∈ [0.004, 273]`, `|B| ≤ 145` and `|D| ≤ 0.4`.
- **Initialization.** Each `F0` slice is an independent job. Within a slice,
  rows run from rough to smooth, and each row from normal to grazing incidence.
  A cell starts from the best of these candidates:
  - its two already-fitted neighbors;
  - the identity;
  - four lobes aligned with the target's average direction, at widths of 0.5α
    to 4α.

  The aligned guesses are what make the sharpest rows converge.
- **Moments.** 256 × 256 stratified points, each giving one visible-normal and
  one uniform hemisphere sample, combined with the balance heuristic. Visible-normal
  samples alone underestimate the Fresnel norm `f` of smooth lobes, whose
  `(1 − v·h)⁵`-weighted mass lies far from the mirror direction: at roughness
  0.06 and a 6° view they gave 2.2 × 10⁻¹⁰ where float64 quadrature over the
  sphere gives 1.44 × 10⁻⁸. The combined estimate matches the quadrature.
- **Residual.** The relative L1 error between the LTC and the normalized lobe,
  in `[0, 2]`, on independent 64 × 64 sample sets. It is recorded for every
  transform in `ltc_table_provenance.json`.

The fit is deterministic and does not depend on the worker count. A slice took
about 4.3 minutes of one core in a release build on a loaded Apple M5, so the
whole table took about 3.7 CPU-hours (2 h 20 min with nine workers).

## Regenerate the table

```bash
cmake --preset macos-clang-release -DLUDUS_BUILD_LTC_FITTER=ON
```

```bash
cmake --build out/build/macos-clang-release --target ludus_ltc_fit
```

```bash
./out/build/macos-clang-release/tools/ltc-fit/ludus_ltc_fit modules/graphics/renderer/shaders modules/graphics/renderer/data --workers 8
```

The first directory receives `ltc_table_layout.h`, the second `ltc_table.bin` and
`ltc_table_provenance.json`; both must already exist. `--workers` sets the job
system's worker threads (default 8, at most 64). Any Linux or macOS preset works.

**What to change.**

- A change that alters the fitted values bumps `LTC_FITTER_VERSION` in
  `tools/ltc-fit/ltc_fit.hpp`.
- A change to the stored layout bumps the generated `LUDUS_LTC_TABLE_VERSION`.

**After regenerating.**

- Run the renderer's `[ltc]` tests, and update the accuracy figures below and in
  `tests/ltc_table_tests.cpp`.
- Test builds compile the fitter and run `ludus_ltc_fit_quick`, a
  low-resolution `--quick` fit. That run checks only that the tool works.

## Use the table

| Language | Include | Notes |
| --- | --- | --- |
| Slang | `ltc_table.slang` | Declares the table buffer at `LUDUS_LTC_TABLE_BINDING`, which the includer defines. |
| C++ (renderer) | `src/internal/ltc_table.h` | Reads the embedded table directly. |

At 3.4 MB the table is too large for shader constants, so shaders read the
renderer's GPU copy from a read-only storage buffer of
`LUDUS_LTC_TABLE_ENTRY_COUNT` `float4` entries. Its contents are the bytes of
`internal::LTC_TABLE`. Define `LUDUS_LTC_TABLE_BINDING` and include
`ltc_table.slang` after the declarations of every lower binding: Slang numbers
Metal buffers in declaration order rather than by the declared slot.
`tests/ltc_table_parity.slang` binds the table this way.

Render-graph shaders request the table with `"polygonal_light_sampling": true`
in the ray technique's `scene_interface`
([authoring](render-graph-authoring.md#select-the-interface-in-the-manifest)).
The graph wrapper then defines the binding as slot 13 and includes
`ltc_table.slang` before the authored callbacks, which call `LookupSpecularLtc`
directly. The renderer
uploads `internal::LTC_TABLE` once per ray context and binds it to every pass of
that context. `tests/sdk_consumer/rg2/ltc_table` reads it from a compute pass on
the GPU and compares the result with the CPU lookup.

Include `ltc_table_layout.h` directly only for its layout constants.

```slang
LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> Inputs;
#define LUDUS_LTC_TABLE_BINDING 1
#include "ltc_table.slang"
#include "linearly_transformed_cosine.slang"

SpecularLtc ltc = LookupSpecularLtc(material.Roughness, viewTangent, f0);
float3 specularAlbedo = ComputeSpecularLtcAlbedo(ltc, f0);
LinearlyTransformedCosine lobe;
LinearlyTransformedCosinePolygon polygon;
if (TryMakeLinearlyTransformedCosine(ltc.InverseTransform, lobe) == true &&
    TryPrepareLinearlyTransformedCosinePolygon(lightTangent, lobe, polygon) == true)
{
    float unshadowedSpecular = GetLinearlyTransformedCosinePolygonIntegral(polygon); // times specularAlbedo
}
```

The render-graph ray helpers (`HELPERS` in `render_graph_ray.py` and
`cmake/LudusRenderGraph.cmake`) stage `ltc_table.slang` and `ltc_table_layout.h`.
Stage FoundationMath's `linearly_transformed_cosine.slang` with the template that
first uses it.

## Accuracy

**Residual of the fit** (relative L1 error; `ltc_table_provenance.json` has the
full grid). The `F0 = 0` slice is listed apart; see below.

| Cells | Median | 95th percentile | Largest |
| --- | --- | --- | --- |
| `F0 > 0` (50 slices) | 4.5% | 41% | 94% (the θ = 90° column) |
| `F0 > 0`, θ < 90° | | 37% | 76% |
| `F0 > 0`, roughness 0.045 | 0.08% | 2.1% | 92% (the θ = 90° column) |
| `F0 = 0` | 35% | 61% | 189% |

- **Minimum roughness.** At the renderer's sharpest lobe, roughness 0.045,
  every view up to 81° fits within 0.9% in every `F0 > 0` slice. With
  `F0 = 1` the residual grows smoothly from 1e-5 at normal incidence to 2% at
  85.7° and 6% at 88.6°.
- **Grazing views.** The residual rises towards grazing views, most at medium
  roughness. That matches Heitz et al.'s observation that LTC fits degrade at
  grazing incidence and high roughness. Cells with θ ≤ 60° stay below 25%,
  with a 99th percentile of 16%.
- **The θ = 90° column.** It is fitted at `cos θ = 10⁻⁴` and is degenerate: a
  lobe pressed against the horizon that a single LTC cannot represent. The
  lookup reaches it only through interpolation within 1.4° of grazing.
- **No optimizer failures for `F0 > 0`.** No cell's residual jumps relative to
  its smoother-roughness neighbor.
- **The `F0 = 0` slice.** Its target is the pure `(1 − v·h)⁵`-weighted lobe.
  At roughness up to 0.21 and views between 9° and 23°, that lobe is a ring
  near the horizon around a near-empty center, and the L3 fit settles on a poor
  local optimum: 48 cells exceed 100%. Lookups choose this slice only when the
  luminance of `F0` is below 0.01. The lobe's albedo `f` in those cells is
  between 6·10⁻⁹ and 2·10⁻⁵, so the poor shape costs LTC-sampling variance on
  a negligible lobe, not correctness.

**Integral checks against Monte Carlo of the exact BSDF.**
`tests/ltc_table_tests.cpp` compares two values, under unit radiance:

- the table's unshadowed specular shading of a polygon, `F0 · (n − f) + f`
  times the LTC integral;
- a 65,536-sample Monte Carlo estimate that samples and evaluates
  `SampleReferenceBsdf` with a metallic material.

The test covers:

- roughness 0.045, 0.1, 0.25, 0.5 and 1;
- views at 0°, 30°, 60° and 80°;
- squares centered on the mirror direction (half sizes 0.05 to 2 at unit
  distance), one square 20° off it, and a square covering nearly the whole
  hemisphere;
- `F0` = 0.04, 0.5 and 1 per channel, each read from its own slice.

That gives 335 comparisons with estimates of at least 10⁻³:

| Comparisons | Median | 90th percentile | Largest |
| --- | --- | --- | --- |
| All | 0.37% | 5.0% | 22% |
| Roughness 0.045 | 0.007% | 0.05% | 16% |
| Whole-hemisphere square | 0.07% | | 3.3% |

- **Largest overall.** The 22% error is a 0.007 estimate at roughness 1, 80°,
  `F0 = 0.04`, on a small square around the mirror direction.
- **Roughness 0.045 outlier.** The 16% error is the far tail, 20° off the
  mirror direction, of a lobe whose estimate there is only 0.0015.
- **Whole hemisphere.** The stored norms match a quadrature of the BRDF to four
  digits (for example 0.9408 at roughness 0.24 and 80°). The 3.3% error at
  roughness 0.25, 80° and `F0 = 0.04` therefore comes from the fitted shape at
  a grazing view, not from the normalization.
- **Norms are estimates.** The fitter's moments are Monte Carlo integrals, so
  a lobe whose albedo is within noise of one can store a norm slightly above
  it; the largest is 1.00001.
- **Test bounds.** The test requires a median of at most 1% and a 90th
  percentile of at most 15% overall. At roughness 0.045 it requires a median of
  at most 0.5% and a 90th percentile of at most 1%. The whole-hemisphere square
  must stay within 4%, and every norm at most 1 + 10⁻⁴.

Develop's earlier single-slice table measured a median of 0.32%, a 90th
percentile of 7.7% and a largest error of 85%. Its Fresnel moment `f` was also
too small for smooth lobes (66 times at roughness 0.06 and a 6° view), because
the fitter sampled only near the specular lobe
([fitting procedure](#fitting-procedure)).

**GPU lookup.** The renderer's `[ltc]` GPU test compares `ltc_table.slang` with
the CPU lookup on 4096 cases, including clamped roughness, views below the
horizon or along the normal, and `F0` outside [0, 1]. On Metal (Apple M5),
entries relative to the matrix's largest entry agree to a 99th percentile of
1.4e-6 and at most 1.2e-5. The GPU's arctangent differs by a few ulps. Vulkan
and WGSL first run in batch CI.

The [default clustered deferred graph](render-graph-authoring.md#default-clustered-deferred-lighting)
also uses this table for unshadowed direct mesh area lighting. It clips real emitter
polygons (merged coplanar triangles) against the receiver horizon before integrating the fitted specular lobe.
The table is uploaded once per graph program; scene and camera replacements retain
independent immutable resource bindings. This raster approximation does not change
the CPU/GPU reference path estimators or establish shadow/indirect-light support.
