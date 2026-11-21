# Renderer Sampling Architecture

**Proposed.** The first delivery is a project-authored continuation
proposal in one render-graph template. The interfaces and filenames below
describe that design.

Sampling has three independently useful controls. A sample pattern determines
how coordinates are distributed across pixels, samples, and dimensions. A
proposal maps those coordinates to an outcome and provides its probability.
An estimator uses that probability to weight the outcome's contribution.
Render-graph templates expose a named proposal where changing it can improve
variance while preserving the integral. Authors can also experiment with
sample patterns and estimators at their respective boundaries.

## Ownership

| Responsibility | Owner | Examples |
| --- | --- | --- |
| Random words, addresses, and unit coordinates | FoundationMath | Existing PCG32 stream and Philox addressed sampling |
| General distributions and geometric mappings | FoundationMath | Bounded integers, uniform disk area, normal pairs, prepared weighted categorical selection |
| Relationships among pixel, path, and frame samples | Renderer | Independent Philox baseline, stratification, scrambled Sobol, spatiotemporal blue noise |
| Scene-dependent proposals | Renderer and project shaders | Continuation directions, BxDF lobes, light selection, emitter points, environment directions |
| Estimation and reuse | Integrator or render-graph passes | Throughput, MIS, RIS, ReSTIR, visibility, and history |

```text
Sample identity → sample pattern → unit coordinates
                                           ↓
Shading context and parameters  →  proposal draw and probability
                                           ↓
                                   estimator or resampling
```

[Randomness](randomness.md) owns generator behavior, key/address layout,
determinism, and the shared CPU and shader primitives. Scene lookup, lights,
and query budgets come from the [scene shader contract](shared-scene-shader-contract.md).
The [renderer systems contract](renderer-systems.md) owns the reference
integrals and the GPU and CPU parity requirements. The separation between
unit coordinates and their use follows the sampling interface described by
Matt Pharr, Wenzel Jakob, and Greg Humphreys in
[*Physically Based Rendering*, 4th ed., §8.3](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Sampling_Interface).

## Random coordinates and sample patterns

Use the existing independent Philox pattern as the baseline. A pass allocates
dimensions by purpose, such as camera jitter, light selection, emitter
coordinates, and continuation. Its schema records each block's width and
grouping. A two-dimensional request identifies coordinates used together,
which lets a pattern stratify them jointly. Pixel or work-item identity,
sample ordinal, purpose, and pass or path-event scope determine addressed
draws. Existing sequential shader draws consume a cursor in the invocation
context. Reserve disjoint addresses when a pass mixes sequential draws and
explicit blocks.

A sample pattern declares its valid sample counts and dimension groups,
randomization, table or resource requirements, and what happens when it
exhausts its range. For example, a stratified group declares the sample count
and jitter distribution. A blue-noise profile declares its spatial and
temporal indexing and repeat period. Each profile specifies the probability law
of coordinates used by the estimator, including any correlations that affect
joint or conditional proposals. Changing a pattern needs evidence that the
configured estimator still integrates the same quantity. Stratification,
importance sampling, and their separate effects on variance are described in
[*Physically Based Rendering*, 4th ed., §2.2](https://www.pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency).

Changing a proposal may change the number or kinds of coordinates it needs.
Give variable-attempt transforms a bounded attempt budget within their own
address block before letting them participate in shared path dimensions.
Keep independent raw-word draws available for discrete mappings that require
more bits than a unit float provides. An addressed pass can replay the same
sample range across dispatch and tile boundaries. A new sampling configuration
starts a new accumulation and invalidates history that depends on the old
proposal or pattern. Record the proposal, parameters or table revision,
pattern, dimension schema, and seed with benchmark captures.

### Stratify a pixel at four samples

For a box-filtered pixel, divide its unit square into four equal quadrants.
Use the sample ordinal to choose the quadrant and the existing pair of
camera-jitter dimensions to jitter inside it. With coordinates `u, v` in
`[0, 1)`, sample ordinal 2 selects the quadrant with origin `(0, 0.5)`
and produces `(u / 2, 0.5 + v / 2)` in real arithmetic. The implementation
keeps rounded coordinates below the quadrant's upper edges. The mean of
one draw per quadrant estimates the same pixel average as four independent
whole-pixel draws.
Keep the BSDF and emitter draws at their original addresses. The four-sample
comparison can then isolate how the camera pattern affects the error of an
oblique silhouette or high-contrast edge.

## Distribution library

| Family | Engine library contract |
| --- | --- |
| Uniform | Half-open real intervals and bounded integer ranges with documented endpoints. Preserve the existing unit-float and unbiased integer primitives. |
| Normal | Standard normal and explicit mean and standard deviation. An initial normal-pair transform uses a documented pair of unit coordinates. |
| Poisson and binomial | Discrete count outcomes and PMFs. Validate rates, trial counts, and parameter ranges. Define overflow behavior and document any deliberate truncation and its normalization. |
| Gamma and beta | Continuous densities with documented shape, scale when applicable, support, and endpoint handling. Bound any rejection loops and report exhaustion. |
| Weighted categorical | Prepare an immutable probability table from finite nonnegative weights. Expose both sample and normalized PMF for the admitted category. Table ownership and revision follow the consumer. |
| Disk, sphere, and hemisphere | Name the measure in the API, including disk area or directional solid angle. Document coordinate space and any required transform. |

Ship new transforms with explicit parameter validation, a coordinate or
attempt budget, and independent distribution checks. The input requirements
include endpoint behavior. For example, a mapping that uses a logarithm must
define how it handles a unit draw at zero. Keep probabilities and status
semantics aligned across C++ and the shader forms an admitted consumer uses.
The [FoundationMath randomness contract](randomness.md#spread-nonuniform-distributions-and-graphics)
already specifies a normal-pair baseline and the numerical expectations for
transcendental mappings. The
[weighted selection contract](randomness.md#weighted-selection-and-authored-gameplay-policies)
owns the general prepared-selector requirements.

For example, a graph that needs two normal perturbations can request one
normal pair from two reserved unit coordinates. With standard normal
outputs `z0, z1`, `mean + standardDeviation * z0` sets the first
perturbation's location and spread. The caller owns the two dimensions and
can replay the draw without a cached spare carried between invocations.

Stratification, scrambled Sobol, and blue noise are sample-pattern families.
A renderer profile can use those coordinates with an admitted distribution
transform. Qualify each pairing for its dimensionality and workload. For
spatiotemporal masks, include spatial and temporal residual spectra and
reconstruction behavior in the comparison. Alan Wolfe, Nathan Morrical,
Tomas Akenine-Möller, and Ravi Ramamoorthi show why both axes matter in
[*Spatiotemporal Blue Noise Masks*, EGSR 2022](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks).

## Proposal and estimator contracts

A proposal takes explicit conditioning data, such as a receiver and view
direction, plus its allocated sample coordinates or random source. An
immutable parameter or table view supplies any tunable distribution data.
For continuous continuation, the outcome is a unit world-space direction
and the probability is a PDF per steradian. The proposal can evaluate that
same PDF for an arbitrary direction from the same receiver and parameter
revision. The integrator evaluates the physical BxDF and cosine terms, applies
the proposal probability to its contribution, and uses the same proposal in
every matching MIS evaluation.

Pick a contract per outcome and measure. Distinguish selection PMFs, area
densities, solid-angle densities, and delta-event probability masses in the
result type or interface. For a sampled emitter, combine selection mass
with the conditional probability and convert to the comparison measure with
the correct Jacobian. A mixed BxDF proposal reports the probability of the
sampled event, including lobe selection and overlapping continuous lobes
where applicable. Admit support over the contributing domain of the declared
estimator. A proposal that loses support can be combined with a
positive-support proposal using the actual mixture probability.

Distinguish an ordinary null outcome from invalid inputs, exhausted
randomness, or an inconsistent probability. A documented null outcome
contributes zero. A successful draw requires an admissible outcome and finite,
positive probability in its measure. In a release implementation, route
invalid results through the owning invocation's failure contract. At cook or
test time, use sampled-versus-evaluated checks, normalization under the
declared measure, and an independent reference to catch errors that type
checking cannot detect. The [scene estimator rules](shared-scene-shader-contract.md#proposal-probabilities-and-estimator-rules)
own light-measure conversion and existing delta behavior.

### RIS and ReSTIR

Keep resampling at the estimator boundary. A candidate provider supplies an
outcome and its original proposal probability in the estimator's measure.
The integrator owns the target function, visibility policy, and contribution.
For the existing sequential RIS primitive, candidate weights use the declared
target divided by the source proposal and the reservoir retains the selected
outcome's identity, total weight, and candidate count. The integrator uses
the reservoir contribution weight. FoundationMath owns the
[sequential weighted reservoir primitive](randomness.md#sequential-weighted-reservoir).

A ReSTIR pass additionally owns temporal or spatial reuse, which receiver
and proposal produced each candidate, and the validity and normalization
rules of the selected estimator. Document any change-of-variables and
visibility or history decisions for that particular reuse algorithm. A new
candidate sampler supplies its own draw and probability together. A new
reuse algorithm changes a pass or estimator and requires its own reference
evidence. The [ReSTIR DI template](../development/render-graph-authoring.md#use-the-built-in-restir-di-template)
is the existing graph consumer. [Bitterli et al., *Spatiotemporal reservoir resampling for real-time
ray tracing with dynamic direct lighting*, SIGGRAPH 2020](https://research.nvidia.com/publication/2020-07_spatiotemporal-reservoir-resampling-real-time-ray-tracing-dynamic-direct)
is the reference for that distinction.

#### Weight ReSTIR's initial light candidates

Consider an initial RIS pass with two discrete lights, A and B. At the
current receiver the target values are 1 and 3. The project chooses each
candidate from a weighted categorical proposal with masses `1/4` and
`3/4`. The candidate provider reports its selected light's mass. The pass
divides the target by that mass and sends the resulting weight to the
reservoir.

| Candidate | Source PMF | Target value | Candidate weight |
| --- | --- | --- | --- |
| A | `1/4` | `1` | `1 / (1/4) = 4` |
| B | `3/4` | `3` | `3 / (3/4) = 4` |

If the stream draws A then B and the reservoir selects B, the total weight
is 8, the candidate count is 2, and the integration multiplier for B is
`8 / (2 × 3) = 4/3`. A uniform source proposal would instead report `1/2`
for either light and give candidate weights 2 and 6. In a ReSTIR pass that
also samples points on an emitter, include the emitter-point PDF with the
light PMF in the measure used by its estimator. This example ends at
initialization. Temporal and spatial passes consume the reservoir under
their own reuse contracts.

## Project-authored proposals

Offer a named role in a working graph, starting with the continuation
proposal of [Veach–Guibas MIS](../development/render-graph-authoring.md#compare-light-and-bsdf-sampling-with-mis).
The generated project contains `main_shaders/continuation_proposal.slang` with
its default binding. The shading shader includes this file. To replace the
proposal, the author adds a project shader that supplies the draw and
probability functions, then binds the new type in the included file.

For this slot, the generic requires static methods with these roles.

| Method | Contract |
| --- | --- |
| `TrySampleDirection` | Accept the same typed shading context and a three-coordinate block. Return a status and write the unit world-space direction on success. |
| `EvaluatePdfSolidAngle` | Accept that conditioning context and a unit world-space direction. Return the proposal's PDF per steradian for MIS and the sampled contribution. |

The first slot allows pure sampling from those three coordinates and the
shading context. Proposals requiring extra resources, dimensions, or effects
need a technique with that wider declared contract. The integrator obtains
the block from its existing addressed dimensions and
uses `Sample<ContinuationProposal>(shadingPoint, coordinates)` for a draw.
The generic evaluates the matching PDF at the sampled direction. The
light-sampled MIS path calls the same bound type to evaluate its PDF for
the light direction. The initial slot covers the continuous directions of
this template. Use separate measure-aware contracts for delta and other
discrete outcomes.

The Slang binding is compile-time and supports both SDK and project-defined
types. Settings can tune parameters. When a graph needs multiple selectable
implementations, use its [typed shader extension contract](render-graph-authoring.md#typed-shader-extension-points-proposed)
to bind baseline and experimental variants to the same integrator before
compilation. LUSL currently
deduces template types from value arguments. A LUSL version can bind the
same draw and PDF contract through fixed-signature functions in a project
`.luslh` include. This uses the existing shader-source closure and diagnostics
in the Editor, CLI, and installed SDK. Surface selectable implementations
through the same editor contract as other typed shader extensions.
[LUSL template rules](../modules/tooling/shader-language.md#interfaces-and-templates).

### Substitute cosine-weighted continuation

For a purely diffuse reflective test scene, compare a uniform hemisphere
proposal with a cosine-weighted one. Put the following implementation in
`main_shaders/cosine_hemisphere_proposal.slang`. The template supplies
`DirectionProposal`, `ShadingPoint`, and FoundationMath's `PI`.

```slang
struct CosineHemisphereProposal : DirectionProposal
{
    static bool TrySampleDirection(ShadingPoint shadingPoint,
                                   float3 coordinates,
                                   out float3 outDirectionWorld)
    {
        float3 normalWorld = shadingPoint.ShadingNormalWorld;
        float3 referenceWorld = abs(normalWorld.z) > 0.5 ? float3(1, 0, 0) : float3(0, 0, 1);
        float3 tangentWorld = normalize(cross(referenceWorld, normalWorld));
        float3 bitangentWorld = cross(normalWorld, tangentWorld);
        float radius = sqrt(coordinates.x);
        float azimuth = 2 * PI * coordinates.y;
        float cosine = sqrt(1 - coordinates.x);
        outDirectionWorld = tangentWorld * (radius * cos(azimuth)) +
                            bitangentWorld * (radius * sin(azimuth)) +
                            normalWorld * cosine;
        return true;
    }

    static float EvaluatePdfSolidAngle(ShadingPoint shadingPoint, float3 directionWorld)
    {
        return max(0, dot(shadingPoint.ShadingNormalWorld, directionWorld)) / PI;
    }
};
```

Bind the new type in `main_shaders/continuation_proposal.slang`.

```slang
#include "cosine_hemisphere_proposal.slang"
typealias ContinuationProposal = CosineHemisphereProposal;
```

The draw uses the first two coordinates in the reserved block and the
receiver's unit shading normal. On a light-sampled MIS path, evaluating the
same proposal at a direction with cosine `0.5` returns `1 / (2π)` per
steradian. The ordinary uniform hemisphere proposal has that PDF at every
direction in its support. For an analytic control with integrand
`cos(θ)` on the hemisphere, the cosine proposal yields a constant
contribution of `π` per draw in exact arithmetic. Uniform draws have
contributions `2π cos(θ)` with the same mean. This gives the custom proposal
a reference for its draw and PDF before measuring full-graph variance.

## Delivery and acceptance

1. **Prove one project override.** Extract the default continuation draw and
   PDF together from the Veach–Guibas template. Preserve its three addressed
   dimensions and the default estimator. Create the graph using an installed
   SDK, build and render its default, then change the project binding to a
   custom continuous proposal for a supported reflective fixture. Verify
   that the custom proposal affects both MIS paths and that the artifact
   rebuilds when its include changes. Show a missing or incompatible binding
   error at the authored shader location. Compare unfiltered scene-linear
   means against an analytic or converged reference, and report error at
   equal sample count and render time across multiple fixed seeds.
2. **Qualify sample patterns independently.** Use the [RT1 stratified pixel
   benchmark](https://github.com/The-Ludus-Project/Ludus/issues/1431) with
   the same pixel integral, reconstruction, and non-camera dimension addresses.
   Compare raw average error, spatial structure, and cost. Carry approved
   reference profiles through both the CPU oracle and GPU tracer under the
   [reference parity contract](renderer-systems.md#cpu-and-gpu-reference-implementation-requirement).
   Treat scrambled Sobol and spatiotemporal blue noise as separate candidates
   with their own sample-count and artifact qualification.
3. **Grow the engine distribution library.** Reuse the existing uniform,
   bounded integer, and reservoir code. Qualify a normal pair and a prepared
   weighted selector with named consumers, then add Poisson, binomial,
   gamma, and beta using algorithms qualified for each supported parameter
   range. Check PMFs or CDFs against independent references, exercise extremes
   and exhaustion, and compare CPU and GPU results with the right exact or
   statistical tolerances. [Initial persisted weighted selection](https://github.com/The-Ludus-Project/Ludus/issues/1297).
4. **Expand measured proposal roles.** Add emitter and environment candidates
   and ReSTIR initial proposals where templates can show the same estimator
   with two viable providers. Use the [measured BRDF proposal
   benchmark](https://github.com/The-Ludus-Project/Ludus/issues/1817) for
   a table-backed BxDF candidate. Record setup and memory cost as well as
   render time. Validate reuse variants independently, including known
   invalidation cases and bias under their declared policy.

For techniques from SIGGRAPH, Eurographics, or EGSR, record the paper and
the exact workload, estimator, measure, state requirements, and adopted
algorithm. Add the implementation at its owning boundary once its comparison
establishes a benefit for a supported workload.
