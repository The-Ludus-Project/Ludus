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
| `modules/graphics/renderer/shaders/ltc_table_data.inc` | Generated table, installed with the renderer shaders. It is a data fragment, so the `.inc` extension keeps formatting and lexical source checks off its 8,000 numeric lines. |
| `modules/graphics/renderer/shaders/ltc_table_provenance.json` | Fitter version, parameters and the residual error of every cell |
| `modules/graphics/renderer/shaders/ltc_table.slang` | Shader lookup, installed with the renderer shaders |
| `modules/graphics/renderer/src/internal/ltc_table.h` | CPU lookup, for the RT1 CPU oracle and the tests |

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
transform.

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

Each cell fits the shape with a Fresnel factor of one. It stores two moments of
the exact lobe:

- the norm `n = ∫ ρ cos`;
- the Fresnel norm `f = ∫ (1 − v·h)⁵ ρ cos`.

Schlick's Fresnel is linear in `F0`, so the specular albedo is exactly
`F0 · (n − f) + f` for each channel. `ComputeSpecularLtcAlbedo` returns it.

> **Departure from Peters 2021:** Peters' renderer tabulates a third axis of 51
> `F0` values (page 9). Ludus fits a single `F0`-independent shape and keeps
> `F0` exact in the albedo. Within a polygon, the shape's Fresnel weighting is
> then approximate. That shows at grazing views with a small `F0` (see
> [Accuracy](#accuracy)). The review lists the table resolution and `F0`
> handling as open questions to settle by measurement.

**Layout.**

- Rows: 64, linear in perceptual roughness from 0.045, the renderer's admitted
  minimum, to 1.
- Columns: 64, linear in the view inclination `θ` over `[0, π/2]`, as in Heitz
  et al.
- Each cell: `(M02, M11, M20, M22)` of `M⁻¹ ∝ [[1, 0, M02], [0, M11, 0], [M20, 0, M22]]`,
  in the frame whose x-z plane contains the view, plus `(n, f)`.

**Lookup.** The lookup interpolates bilinearly. It then rotates `M⁻¹` by the
view's azimuth into the caller's tangent frame, so the result can go straight to
`TryMakeLinearlyTransformedCosine`.

## Fitting procedure

These parts of the procedure are Ludus choices; the papers do not specify them.

- **Objective.** Heitz et al.'s L3 error between the LTC and `ρ cos / n` over
  the sphere. It is estimated from two fixed, stratified sample sets of 32 × 32
  points: visible-normal samples of the BRDF (Heitz, JCGT 7(4), 2018) and
  cosine samples mapped through `M`. The two sets are combined with the balance
  heuristic. Fixed samples make the objective deterministic and smooth across
  cells.
- **Optimizer.** Nelder–Mead over `(log A, B, log C, D)`, with three restarts of
  at most 1500 iterations. The simplex steps scale with the lobe's width.
- **Initialization.** Rows run from rough to smooth, and each row from normal
  to grazing incidence. A cell starts from the best of these candidates:
  - its two already-fitted neighbors;
  - the identity;
  - four lobes aligned with the target's average direction, at widths of 0.5α
    to 4α.

  The aligned guesses are what make the sharpest rows converge.
- **Moments.** 256 × 256 stratified visible-normal samples, for which
  `ρ cos / pdf = 2 V n·l (n·v + Λv)`.
- **Residual.** The relative L1 error `∫ |LTC − ρ cos / n|`, in `[0, 2]`, on
  independent 64 × 64 sample sets. It is recorded for every cell in
  `ltc_table_provenance.json`.

The fit is deterministic. Two runs on Apple M5 produced byte-identical files.
It takes about 2.5 minutes in a release build.

## Regenerate the table

```bash
cmake --preset macos-clang-release -DLUDUS_BUILD_LTC_FITTER=ON
cmake --build out/build/macos-clang-release --target ludus_ltc_fit
./out/build/macos-clang-release/tools/ltc-fit/ludus_ltc_fit modules/graphics/renderer/shaders
```

Any Linux or macOS preset works. The output directory must already exist.

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
| Slang | `ltc_table.slang` | Pulls in the generated data with `LUDUS_LTC_TABLE_ENTRY` defined as `float4`. |
| C++ (renderer) | `src/internal/ltc_table.h` | |

Include the generated header directly only for its layout constants.

```slang
#include "ltc_table.slang"
#include "linearly_transformed_cosine.slang"

SpecularLtc ltc = LookupSpecularLtc(material.Roughness, viewTangent);
float3 specularAlbedo = ComputeSpecularLtcAlbedo(ltc, f0);
LinearlyTransformedCosine lobe;
LinearlyTransformedCosinePolygon polygon;
if (TryMakeLinearlyTransformedCosine(ltc.InverseTransform, lobe) == true &&
    TryPrepareLinearlyTransformedCosinePolygon(lightTangent, lobe, polygon) == true)
{
    float unshadowedSpecular = GetLinearlyTransformedCosinePolygonIntegral(polygon); // times specularAlbedo
}
```

Staging these includes into render-graph ray helpers (`HELPERS` in
`render_graph_ray.py` and `cmake/LudusRenderGraph.cmake`) belongs to the
template that first uses them.

## Accuracy

**Residual of the fit** (relative L1 error; `ltc_table_provenance.json` has the
full grid):

| Cells | Median | 95th percentile | Largest |
| --- | --- | --- | --- |
| All | 4.3% | 41% | 94% |
| Roughness 0.045 | 0.07% | 1.5% | 92% (the θ = 90° column) |

- **Minimum roughness.** At the renderer's sharpest lobe, roughness 0.045,
  every view up to 81° fits within 1%. The residual grows smoothly from 1e-5
  at normal incidence to 2% at 86° and 6% at 88.6°.
- **Grazing views.** The residual rises towards grazing views, most at medium
  roughness. That matches Heitz et al.'s observation that LTC fits degrade at
  grazing incidence and high roughness. Cells with θ ≤ 60° stay below 17%.
- **The θ = 90° column.** It is fitted at `cos θ = 10⁻⁴` and is degenerate: a
  lobe pressed against the horizon that a single LTC cannot represent. The
  lookup reaches it only through interpolation within 1.4° of grazing.
- **No optimizer failures.** No cell's residual jumps relative to its
  smoother-roughness neighbor.

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
- `F0` = 0.04, 0.5 and 1 per channel.

That gives 335 comparisons with estimates of at least 10⁻³:

| Comparisons | Median | 90th percentile | Largest |
| --- | --- | --- | --- |
| All | 0.32% | 7.7% | 85% |
| Roughness 0.045 | 0.006% | 0.05% | 16% |
| Whole-hemisphere square (normalization) | 0.08% | | 1.2% |

- **Largest overall.** The 85% error is a 0.004 estimate at roughness 1, 80°,
  `F0 = 0.04`, where the `F0`-independent shape misplaces the Fresnel
  weighting.
- **Roughness 0.045 outlier.** The 16% error is the far tail, 20° off the
  mirror direction, of a lobe whose estimate there is only 0.001.
- **Test bounds.** The test requires a median of at most 1% and a 90th
  percentile of at most 15% overall. At roughness 0.045 it requires a median of
  at most 0.5% and a 90th percentile of at most 1%. The whole-hemisphere square
  must stay within 3%.

**GPU lookup.** The renderer's `[ltc]` GPU test compares `ltc_table.slang` with
the CPU lookup on 4096 cases, including clamped roughness and views below the
horizon or along the normal. On Metal (Apple M5), entries relative to the
matrix's largest entry agree to a 99th percentile of 1.5e-6 and at most 1.6e-5.
The GPU's arctangent differs by a few ulps. Vulkan and WGSL first run in batch
CI.
