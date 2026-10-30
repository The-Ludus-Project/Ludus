# Shared scene, material and lighting shader contract

Status: initial portable scene-ray interface implemented, October 20, 2026.
Read [author scene-aware ray shaders](../development/render-graph-authoring.md#author-scene-aware-ray-shaders)
for exact delivered manifest fields, signatures, material defaults/errors and
limits. The remaining design below records the broader shared-interface target;
its generic capability declarations, arbitrary payload grammar, raster-stage
bindings and selective accumulation reuse are not delivered by this initial
profile. The existing triangle-ray profile remains available. No NEE or bounce
integrator is supplied by the engine.

This document owns the shared scene shader API proposal. The
[render graph architecture](render-graph-authoring.md#shared-schemas-and-cpugpu-layout)
owns composition, schemas and packages; [Renderer](renderer-systems.md) owns
extraction, materials and sampling; [RHI](rhi-gdi.md) owns device resources and
submission. See the [current ray profile](../development/render-graph-authoring.md#execute-a-triangle-ray-pipeline)
for delivered behavior.

## Decision and author control

Expose a versioned, read-only SceneContext to explicitly opted-in shader stages.
It provides surface/material lookup, typed light enumeration, light proposals,
their matching probabilities, geometric visibility and raw intersection queries.
The engine does not run an integrator when this interface is selected.

Raygen becomes the user-authored sample driver. It can trace, dispatch the user's
closest-hit/miss functions, keep its own payload and drive a bounded loop.
Closest-hit can inspect materials, sample lights, issue visibility queries and
write a continuation into the user's payload. The user chooses the estimator,
BSDF, MIS weights, throughput, termination and bounce count. Standard BSDF and
light-sampling routines are optional primitives, not mandatory shading policy.

The initial target is `camera -> x0 -> light` plus
`camera -> x0 -> x1 -> light`: direct illumination and one intermediate surface
for indirect illumination. Shadow segments do not count as surface bounces.
This needs at most two closest-surface queries and two shadow queries per sample
when choosing one light at each surface. It does not require recursive callbacks.

## Snapshot and graph contract

A full Scene port binds one immutable extraction generation and selects Geometry,
Materials and Lights. The delivered editor can expand a direct Scene-to-pipeline
wire into typed members and preserve a selected subset in the compiled pass.
See [connect or expand a Scene bundle](../development/render-graph-authoring.md#connect-or-expand-a-scene-bundle)
for the current groups, dependency rules and canvas workflow. The finer capability
model below remains a broader target:

| Proposed capability | Shader access | Required data |
| --- | --- | --- |
| `SceneSurfacesV1` | Raygen, closest-hit, admitted raster stages | Instances, triangles, UVs, normals, material and emission references |
| `SceneMaterialsV1` | Same stages | Material records and retained texture views/sampler policy |
| `SceneLightsV1` | Same stages | All admitted analytic sources, geometric emitters and environment |
| `SceneLightSamplingV1` | Same stages | Named proposal distribution and probability lookup |
| `SceneVisibilityV1` | Only stages on a qualified traversal profile | Occlusion queries and robust ray construction |
| `SceneTraceV1` | Only stages on a qualified traversal profile | Closest-surface queries |
| `AuthoredRayCallbacksV1` | Raygen | Statically linked user closest-hit/miss functions |

Enumeration and material lookup do not imply ray-query support in raster stages.
A raster shader may read the same logical lights and materials on an admitted
profile; requesting visibility there requires separate backend qualification.
Unsupported requirements reject activation, rather than disappearing from Scene.

Evolve the Renderer submission boundary to a typed SceneSnapshotDescription with
borrowed mesh, instance, material, analytic-light, emitter and environment tables.
Use Ludus-owned bounded views. Update validates and consumes/copies the inputs
before returning; GPU preparation retains all referenced resource incarnations.
An explicit adapter preserves the current flat-item Update overload. Renderer
must not depend on GameplayWorld or the editor's Qt types.

Instance/material references are explicit: do not recover them from flat tint.
The editor adapter must carry authored lights, emission, normals, UVs and texture
references, which its current graph extraction omits. Share this extraction with
shipping callers; do not build an editor-only lighting path.

Every shader-visible table index is a uint32 within that generation. Diagnostic
source identities use a uint2 representation of the existing 64-bit identity;
identities are not array indices. Geometry, materials, emitter maps, proposal
tables and their counts publish atomically. Old generations and GPU leases remain
alive until all producer/consumer work completes. Indices cannot cross generations.

Do not reuse raster's eight-light selection or area-proxy selection as the source
of truth. All supported sources must be present. Enforce explicit table and byte
budgets before publication; an overflow rejects the candidate without truncation.
Declare limits once in the owning schema and expose device-admitted limits in the
build/runtime report. Initial numeric limits require the implementation's packing
and platform evidence; this proposal does not invent unverified new capacities.

## Surface and material values

The following are logical records. Generate physical C++/Slang layouts from one
schema and verify each target's offsets, strides and resource reflection. No host
pointer or native bool crosses the wire. SceneContext is a compiler-bound resource
bundle, not a device pointer or an independently mutable global.

| Logical record | Required fields and meaning |
| --- | --- |
| `SurfaceHit` | Incoming Ray; instance, mesh-local primitive, material and optional emitter indices; barycentric weights; PositionWorld; conservative PositionErrorWorld; DistanceWorld; unflipped GeometricNormalWorld; ShadingNormalWorld; tangent frame and UV; explicit front-face flag |
| `MaterialData` | Base-color factor, tint, metallic, perceptual roughness, emission factor, emission sidedness, texture references, UV transforms, normal scale and alpha domain |
| `MaterialSample` | Texture-resolved BaseColor, Metallic, Roughness, Emission, ShadingNormalWorld and alpha; raw data does not include lighting |
| `LightReceiver` | PositionWorld, PositionErrorWorld, GeometricNormalWorld and ShadingNormalWorld; records the actual unoffset shading point |
| `TextureEvaluation` | Explicit mip policy; the same policy applies to camera hits, secondary hits and sampled emitter emission |
| `TriangleData` | Three world positions, geometric normal, world area, per-corner UVs, material index and source primitive mapping; no BVH-private offsets |

All positions/distances use world metres. Directions are finite unit vectors.
At a surface, both view and light directions point away from that surface.
Barycentrics weight vertices one and two; vertex zero has weight `1-u-v`.
Transform normals with the normal matrix. Establish outward orientation using
mesh winding and the existing transform/orientation policy, including reflected
instances. Preserve geometric versus shading normals; ray offsets and emission
sidedness use geometric normals, not a normal map. Never silently face-forward
one-sided emission. The first reflection profile rejects back-face reflection;
two-sided emission does not imply two-sided reflection.

Material evaluation decodes color/emission maps to the renderer's linear RGB
space, retains data maps as linear data and performs normal mapping consistently.
The traversal profile supports static opaque and alpha-masked triangles, reflection-only
metallic/roughness materials and explicit fixed texture mip evaluation. Missing
optional maps use their documented neutral factors; missing required attributes
or unsupported blended alpha/transmission/deformation rejects the scene profile.
`SceneShaderMaterial::IsAlphaMasked` and `AlphaCutoff` select the existing mask
domain in both the CPU oracle and GPU reference scene. Coverage multiplies base
texture alpha by interpolated vertex alpha before the inclusive cutoff test;
closest-hit and shadow queries use the same test. Raster color, G-buffer and shadow
passes receive the same mask state. The optional clustered opaque Z prepass has no
texture bindings; a material table containing masks rejects that profile with
`Unsupported` before publication, retaining the last drawable view. The default
deferred graph works without that prepass. The editor reconstructs its retained mip bilinearly;
the material API still permits an explicit nearest sampler.

Raw material values remain inspectable even when the shader uses its own BRDF.
Optional EvaluateStandardBsdf/SampleStandardBsdf use the existing Renderer
reflection model and return f without cosine, the full mixture directional PDF
in inverse steradians, and an accepted/null-sample result. Never hide a selected
lobe probability or retry a null sample. Roughness floors and normal conventions
belong to the named BSDF profile; custom BRDFs provide their own matching sampler.

## Light sources and mesh emitters

The shader sees LightInfo records with kind, source identity and typed-record
index. Checked typed getters expose each source's parameters; do not infer kinds
from struct padding or renderer-private packed words.

| Kind | Data and evaluation semantics |
| --- | --- |
| Directional | Outgoing light direction and RGB illuminance; discrete direction |
| Point | PositionWorld, RGB intensity and range window; inverse-square attenuation |
| Spot | Point data plus outgoing direction and inner/outer cone cosines |
| MeshEmitter | Instance/material association and a set of actual nondegenerate world-space triangles; texture-aware emission and sidedness |
| Environment | At most one aggregate constant or HDR environment, orientation and evaluation/sampling profile; no geometry |

Reuse the engine's documented light units: directional lux, point/spot candela,
emission RGB nits, and the existing RGB lighting convention. This is not a new
spectral-to-photometric conversion. Point/spot range and cone windows remain
explicit features of the selected Renderer lighting profile; an experimental
shader can instead inspect the typed data and author another evaluation.

Represent rectangle/disk/other authored finite area sources by their explicitly
tessellated emitter geometry in V1. Preserve any associated visible geometry and
source identity. Report tessellation as an approximation; do not claim analytic
shape sampling. Raster AreaOneSided/AreaTwoSided DirectLight samples remain named
quadrature approximations. If a source has both those raster proxies and geometry,
register its geometry once for ray sampling and omit the proxies from that ray
distribution. Do not treat three quadrature points as three exact area lights.

Build an emitter per instance/material-slot group with a potentially nonzero
emission factor/map, not per triangle and not per unique mesh asset. Instancing
changes world area, position and normal. Keep every potentially emitting triangle
in that group's sampling support, including textured black regions as legitimate
zero samples. Sampling must use emission at the sampled UV. Skip exactly
zero-area imported triangles consistently in BVH and emitter construction, retain
source-to-packed mappings and report the skipped count. Nonfinite data rejects.
An emitter containing no positive-area triangles contributes no sampleable source.

Every emissive hit has an emitter index for reverse PDF lookup, or a documented
invalid index for a zero-emission surface. User-authored extra emission that is
not registered as a scene source has no competing NEE proposal: its light PDF is
zero. Scene source emission evaluation and hit emission evaluation must agree.

## Public shader operations

These are proposed logical signatures, not compilable declarations of today's
SDK. All indexed reads validate bounds; invalid indices latch invocation failure.
Empty light sets and zero-contribution proposals are ordinary results, not errors.

```text
GetSceneInfo(scene) -> counts, generation, admitted profiles
GetMaterial(scene, materialIndex) -> MaterialData
EvaluateMaterial(scene, hit, textureEvaluation) -> MaterialSample
EvaluateEmission(scene, hit, directionAwayWorld, textureEvaluation) -> float3
GetLightInfo(scene, lightIndex) -> LightInfo
GetDirectionalLight/GetPointLight/GetSpotLight/GetMeshEmitter/GetEnvironment(...)
GetEmitterTriangle(scene, emitterIndex, primitiveInEmitter) -> TriangleData
GetEmitterBounds(scene, emitterIndex) -> EmitterBounds

SelectLight(scene, distribution, receiver, selectionAddress) -> SelectedLight
EvaluateLightSelectionPmf(scene, distribution, receiver, lightIndex) -> float
SampleIncidentLight(scene, selectedLight, receiver, shapeSample) -> LightSample
EvaluateLightPdfSolidAngle(scene, distribution, receiver, emissionEndpoint) -> float
EvaluateEnvironment(scene, directionWorld, textureEvaluation) -> float3

TraceClosest(scene, inout queries, ray) -> {Surface, Miss, Failure} and SurfaceHit
TestVisibility(scene, inout queries, receiver, lightSample) -> {Clear, Occluded, Failure}
SpawnSurfaceRay(hit, directionWorld, maximumDistanceWorld) -> Ray or Failure
TraceAndShade(scene, inout queries, ray, inout UserPayload) -> {Surface, Miss, Failure}
```

Expose resolved typed source data and a checked emitter-triangle iterator so
authors can implement their own selection/sampling. `GetEmitterBounds` adds
per-emitter world bounds computed once with the scene from the same
positive-area triangles: a sphere around their vertex bounding box, the cone of
their geometric normals (unit axis and the cosine of its half-angle, -1 when the
normals cancel or span every direction) and the material's emission sidedness.
A proposal can then weight each emitter in O(emitters) instead of iterating
every emitter triangle per receiver. The bounds only shape a proposal: sampling
and the reverse PMF must evaluate the same weight, which stays unbiased as long
as it is positive wherever the emitter can contribute. The default proposal is
optional. Its matched PDF routine applies only to that exact distribution;
custom proposals supply their own probabilities. SelectedLight records its
generation, distribution, source index and SelectionPmf. LightSample contains:

| Field | Contract |
| --- | --- |
| `HasSample` | False only for an ordinary empty/null proposal; output is initialized |
| `LightIndex`, endpoint identity | Source index and, for mesh samples, triangle/barycentric endpoint needed to evaluate emission/PDF |
| `DirectionWorld` | Unit receiver-to-source direction before numerical endpoint offsets |
| `IncidentCoefficient` | Unoccluded linear RGB Li for continuous sources; for delta sources, the coefficient after distance/range/cone evaluation |
| `SelectionPmf` | Probability of choosing the source, excluding its shape/direction sample |
| `ConditionalPdf` | Inverse-steradian density for continuous sources; sampled mass one for a delta source |
| `Measure` | `SolidAngle` or `Discrete`; no unitless ambiguity in MIS |
| `Endpoint` | Finite point/error/normal for mesh, point position for punctual, or explicit infinite endpoint for directional/environment |

IncidentCoefficient excludes receiver BRDF, receiver cosine, visibility, exposure
and division by sampling probabilities. It contains no hidden MIS weight.
For continuous sources Li contains no extra inverse-square factor; the solid-angle
PDF conversion accounts for geometry. For delta point/spot samples the coefficient
does include inverse-square attenuation. Do not multiply either case twice.

TestVisibility traces only an occlusion segment: it never invokes authored hit or
miss shading and never adds radiance. Offset the receiver robustly and the mesh
endpoint toward the receiver using error bounds; exclude only the numerical end
point, not the emitter instance. Other triangles of the same emitter can occlude.
An infinite endpoint traces to a conservative scene exit distance; this shadow
range is not the camera far plane. Surface secondary rays have their own explicit
maximum range. Finite transport cutoffs are reported as such, not exact infinity.
No hard-coded world epsilon is part of the public API.

## Proposal probabilities and estimator rules

V1 ships a deterministic UniformSources distribution: choose among admitted
analytic sources, mesh-emitter groups and the optional aggregate environment
with PMF `1 / sourceCount`. Selection uses the existing addressed bounded-integer
sampler. Within a mesh emitter choose a triangle proportional to its positive
world-space area, then sample uniformly on that triangle. If the group's total
area is A, conditional area density is `1/A`; splitting a triangle does not change
the group's selection probability or spatial density. Constant/HDR environments
start with uniform-sphere directional sampling, density `1/(4*pi)`, with radiance
evaluated at that direction. Importance distributions can be added by versioned
policy, without changing the meaning of the PDF API.

For a sampled emitter endpoint y seen from receiver x, let d be its distance and
c be the absolute emitter geometric-normal cosine. The continuous joint density
is `pLight = SelectionPmf * (1/A) * d*d / c`. Zero c or coincident endpoints
produce an ordinary zero sample. Back-side one-sided emission is zero without
resampling or renormalizing; two-sided emission changes emitted radiance, not A.
EvaluateLightPdfSolidAngle returns that same joint density for the exact hit
endpoint and same distribution/receiver. It is an endpoint-specific PDF, not a
sum over occluded emitter intersections sharing one direction.

An authored NEE estimator may compute
`f * IncidentCoefficient * receiverCosine * visibility / (SelectionPmf * ConditionalPdf)`.
For delta lights the denominator is a probability mass; a continuous BSDF cannot
compete and the MIS weight is one. Evaluating a delta source's solid-angle PDF
returns zero. For non-delta light/BSDF samples use probabilities in the same
measure and include sample counts when using more than one proposal.

When a BSDF continuation hits an emitter, the user can obtain its light PDF from
the previous unoffset receiver and apply the complementary MIS weight. Primary
visible emission has weight one. Environment misses follow the same rule for the
aggregate environment. Neither engine callbacks nor emission helpers apply MIS.

**Terminal-vertex rule:** if x1 performs NEE but the algorithm never samples a
BSDF continuation from x1, its light estimate has weight one. Comparing against
a hypothetical BSDF sample would lose energy. At x0 the actual x0-to-x1 ray can
compete for emitter/environment hits, so MIS there is valid. Alternatively, the
author can implement NEE-only direct illumination and omit competing non-delta
emitter-hit terms. The contract permits both algorithms and supplies their data.

Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, *Physically Based Rendering:
From Theory to Implementation*, fourth edition (2023), sections
[12.1, Light Interface](https://pbr-book.org/4ed/Light_Sources/Light_Interface),
[12.6, Light Sampling](https://pbr-book.org/4ed/Light_Sources/Light_Sampling), and
[13.4, A Better Path Tracer](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer),
for separating source selection, incident-light proposals and same-measure MIS.
This proposal adapts those boundaries to the existing finite-range RGB Renderer
and portable authored shader callbacks; it does not adopt PBRT's class hierarchy
or prescribe its integrator.

## Raygen and closest-hit linking

Introduce a distinct `AuthoredSceneRayV1` profile. Do not change the meaning of
the legacy `Ray GenerateRay(RayLaunch)` signature: it cannot express the requested
sample driver. The new recipe assigns three user functions and one user-defined
shader-local payload type:

```text
float3 UserRaygen(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
void UserClosestHit(SceneContext scene, SurfaceHit hit,
                    inout RayQueryContext queries, inout UserPayload payload)
void UserMiss(SceneContext scene, Ray ray,
              inout RayQueryContext queries, inout UserPayload payload)
```

RayLaunch contains pixel/extent, camera data, SampleIndex, Seed and an addressed
random-sampling interface. Expose subpixel samples and camera-ray construction;
the shader chooses jitter versus center rays. The sample index advances only
when that invocation's complete sample is accepted. UserPayload is defined in a
package-local shared Slang source, carries only bounded plain shader-local values
and is initialized by the author. Its compiled size is admitted against an
explicit local-payload budget; it is not a serialized CPU or native RT payload.

RayQueryContext is a separate invocation-local mutable context initialized once
by the generated wrapper. It carries query counters and the failure latch;
every query and callback forwards that same context by inout reference. Scene
buffers remain read-only. The public contract does not permit constructing,
resetting or copying this context into independent budgets. The linker validates
its forwarding in the supported callback profile; this is an API correctness
check, not a security boundary against arbitrary device code.

TraceAndShade is generated after the three function declarations are known. It
performs one TraceClosest, calls the selected user closest-hit or miss function
once, and returns status. Only raygen may call TraceAndShade; reject callbacks
that dispatch it directly or indirectly and reject recursive call graphs.
Closest-hit may call TraceClosest for raw queries or TestVisibility, but those
queries do not recursively dispatch user shading. This is ordinary linked
compute code on the current portable BVH profile, not an implied DXR/Vulkan
shader table or hardware traversal guarantee.

Ray callbacks may also be assigned as a complete set to a scene ComputePipeline.
The query boundary owns strategy dispatch: callback sources and ScenePayload do not
change between triangle-loop intersections and native queries. Strategy is part of
the cooked callback variant, so shared program IDs do not imply shared traversal.
See the [authoring contract](../development/render-graph-authoring.md#reuse-ray-callbacks-in-compute-pipelines)
for defaults, selection and the separate native-query versus ordinary-compute requirements.

The following is author-owned pseudocode. UserEstimateDirect, UserHandleEmission
and UserChooseContinuation are deliberately not engine helper implementations:

```text
UserRaygen(scene, launch, queries):
    payload = initialize radiance=0, throughput=1, sample address, depth=0
    ray = construct chosen camera ray from launch
    for depth in [0, 1]:
        payload.depth = depth
        payload.continue = false
        require TraceAndShade(scene, queries, ray, payload) != Failure
        if payload.continue == false: break
        ray = payload.nextRay
    return payload.radiance

UserClosestHit(scene, hit, queries, payload):
    material = EvaluateMaterial(scene, hit, chosenMipPolicy)
    UserHandleEmission(hit, material, payload)  // primary or previous-vertex MIS
    canContinue = payload.depth == 0
    direct = UserEstimateDirect(scene, queries, hit, material, payload, canContinue)
    payload.radiance += payload.throughput * direct
    if canContinue:
        UserChooseContinuation(scene, hit, material, payload)
        // update throughput by f*cos/pdf; retain previous receiver and pdf
        // use SpawnSurfaceRay; set continue only for an accepted BSDF sample

UserMiss(scene, ray, queries, payload):
    // evaluate the environment and apply the author's previous-vertex MIS
    payload.continue = false
```

canContinue alone does not prove a competing estimator exists: UserEstimateDirect
must use the actual BSDF sampling strategy chosen by UserChooseContinuation.
An ordinary null BSDF sample is retained as zero contribution, not retried.
At the second hit, direct lighting uses the terminal-vertex rule above.

The engine owns only launch, resource binding, query-budget enforcement and
output publication. The recipe declares maximum surface and shadow queries per
sample; the initial one-bounce example requests two of each and callback depth
one. Query counters are per launch invocation and shared across linked helpers.
Exceeding a bound, invalid rays, out-of-range indexing, nonfinite/negative output
or traversal failure latches failure and does not commit a partial sample. A
miss, occluded segment or zero sample is distinct from failure. Runtime checks
do not claim to sandbox arbitrary infinite user shader loops.

## Build, history and rollout

Declare interface/profile version, Scene and View port mapping, required scene
capabilities, distribution/profile IDs, payload source/type, callback functions,
query bounds, texture policy and output/history policy in the manifest recipe.
The exact JSON spelling is a schema implementation task; these are required
semantics, not new fields silently accepted by today's parser.

Generate declarations and bindings from authoritative SDK schemas, with separate
logical and physical layout identities. Link user functions through the existing
offline package build and validate the emitted compute interface. Reflection
must cover all reachable declared scene resources; conservative declared read
sets become actual RHI graph uses and lifetime dependencies. Pipeline nodes do
not wire internal table buffers individually. Shader files cannot reach the
private GPU-preview buffers through an undocumented include.

Record geometry, material/texture, light/emitter/distribution, camera, shader,
sampling and texture-policy identities in the accumulation key. A change resets
linear floating-point history; increasing only the target sample count does not.
Exposure/display changes need not reset raw radiance. History is a renderer-owned
persistent resource with declared graph read/write uses and tracked completion,
not an attachment-pool transient. A separate explicit display conversion produces
RGBA8 previews; do not clamp HDR samples before accumulation. This requires a
new internal storage/output profile, not pretending the legacy RGBA8 hit-return
path already preserves HDR. No denoising or temporal reprojection is assumed.

Implement in these reviewable stages:

1. Versioned scene/material/light schema, host submission and editor extraction;
   typed inspection shaders prove the actual selected World's data survives.
2. Shared material/emission evaluation, emitter maps, default proposals and
   probability queries; raster/data consumers can inspect without tracing.
3. Qualified closest/visibility queries, robust segments and user callback linking;
   query failures remain distinguishable from misses and occlusion.
4. Linear sample history and an SDK-authored one-bounce example implementing NEE
   in its own raygen/closest-hit sources; the engine still has no implicit NEE.

Each stage extends compiler schema, runtime admission, installed SDK declarations,
editor source/interface help and documentation together. Old graphs retain the
legacy profile. Opting in requires selecting the new recipe and converting
function signatures; incompatible layouts require a host rebuild rather than
reinterpretation. The existing immutable build/atomic activation workflow keeps
the last drawable generation on any rejected or pending candidate.

## Required acceptance evidence

| Area | Acceptance |
| --- | --- |
| Mixed sources | Two points, a spot, a directional and multiple instanced emitters remain individually inspectable; no raster selection drops or proxy duplication |
| Mesh support | Nonuniform/reflected transforms, multi-material meshes, split triangles and collapsed imported faces retain correct area, emission and source mapping |
| Sampling | Source PMFs normalize; sampled versus evaluated continuous PDFs agree; triangle subdivision preserves density; delta mass and solid-angle zero remain distinct |
| Material fidelity | Texture/UV/tint/normal/emission evaluation agrees between hit and sampled endpoints at the declared mip; one-sided emission does not leak |
| Visibility | Near-contact geometry, far/coplanar receivers and emitter self-occlusion; endpoint exclusion does not erase other triangles of the emitter |
| Author control | A user changes only raygen/closest-hit to implement NEE-only, MIS or one-bounce continuation; engine code and Scene wiring do not change |
| Integrator correctness | Empty/black scenes, visible emission, mixed delta/area light reference, terminal-vertex NEE energy and depth-one color bleeding match independent estimates within sampling error |
| Failure/lifetime | Exhausted budgets, malformed indices/rays, missing capabilities, rejected builds and pending updates preserve the prior drawable generation; no partial sample commits |
| History | Deterministic seed/sample addressing; light/material/camera/shader changes reset; sample-target increase and display-only exposure preserve raw means |
| SDK/platform | Clean installed consumer builds against public includes only; independently reflected layouts; qualify Metal first plus each claimed backend, without inferring hardware RT or arbitrary raster-stage query support |

The licensed staircase2 scene is optional end-to-end acceptance: verify its mesh
emitters survive World extraction, then run the user's authored direct and
one-bounce shaders with no synthetic replacement lights. Deterministic synthetic
fixtures establish CI correctness without requiring that scene. Frame time and
noise must be measured separately; this API design makes no real-time claim.

## Authored forward material execution

The `ForwardLit`, `ClusteredForward` and `ClusteredDeferred` raster profiles consume the existing `RenderGraphMesh`,
`RenderGraphItem` and `SceneShaderInputs` vocabulary. Its shader ABI is the
renderer-authored material profile; this does not make arbitrary SceneContext
queries or arbitrary raster storage bindings executable. The same World extraction
feeds native rays and raster materials, while their physical preparation remains
profile-specific. The engine default uses the portable clustered deferred profile, with an
explicit optional depth prepass; see [default clustered deferred lighting](../development/render-graph-authoring.md#default-clustered-deferred-lighting).
The original eight-light profile remains available; see the [forward-lit workflow and limits](../development/render-graph-authoring.md#use-the-forward-lit-template).

A private forward scene generation owns material incarnations and pins shared
immutable mesh/texture resources. Graph coverage and publication remain in RenderGraph;
material evaluation, light selection and environment preprocessing retain their
Renderer owners. Resources Manager still owns content leases/dependency replacement,
not GPU caches. A scene's source leases may be released after its borrowed inputs
have been consumed. GPU versions retire through existing Renderer/RHI retention.
