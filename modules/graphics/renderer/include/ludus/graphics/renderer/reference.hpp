#pragma once

#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/rhi/ray.h>

namespace ludus::graphics::renderer
{
struct AtmosphereDescription;
/// Caller-owned random address for layered material estimates; no mutable/global RNG.
/// Scope identifies the receiver/path, Event its sample, and Dimension the material
/// event (0..255, typically the scene bounce). Each event reserves 256 local dimensions
/// in separate evaluation, PDF and continuation key domains. Equal inputs replay.
struct ReferenceBsdfRandomContext final
{
    foundation::math::RandomKey Key{};         ///< Explicit root key, copied; no lifetime requirement.
    foundation::math::RandomAddress Address{}; ///< Logical material event; invalid dimensions reject.
};
/// Declared reflection integral of RT1's GGX lobes, recorded in capture metadata. Authored materials keep
/// their scattering model; the profile chooses how a capture integrates it, so one scene compares both.
/// See "Multiple-scattering GGX profile" in docs/architecture/renderer-systems.md.
enum class ReferenceMaterialProfile : ludus::foundation::uint8
{
    /// The default: single-scattering GGX with height-correlated Smith visibility. Energy scattered between
    /// microfacets is lost, so a white rough metal reflects about 0.31 of a white furnace at roughness 1.
    SingleScattering,
    /// Opt-in prototype: every MetallicRoughness, Conductor and DiffuseConductorMix GGX lobe adds Christopher Kulla
    /// and Alejandro Conty's reciprocal compensation lobe for scattering between microfacets, from a tabulated
    /// unit-Fresnel directional albedo, and the revised Fresnel multiplier Favg^2 Eavg / (1 - Favg (1 - Eavg)).
    /// Lossless GGX reflection then integrates to unit albedo within the table's accuracy. Diffuse lobes, project
    /// BRDFs, closure programs, coatings and dielectrics are unchanged. The lobe is cosine sampled with the probability
    /// of the deficit 1 - E(view) within each GGX proposal; sampling keeps three fixed dimensions.
    MultipleScatteringGgx,
};
/// Field operator of an optical reference capture's finite light connections (positive OpticalWavelengthWorld).
/// Both operators are scalar and estimate the integral that render-graph-authoring.md declares for them, in
/// "Trace scene optical connections"; the CPU oracle and GPU compute traversals implement both.
enum class ReferenceOpticalConnection : ludus::foundation::uint8
{
    /// The default: a Gaussian mode propagated through the scene's interaction regions on a 16 x 16 paraxial
    /// angular-spectrum lattice, which diffracts where geometry clips the mode.
    AngularSpectrum,
    /// The Uniform Theory of Diffraction: the ballistic field of the straight connection plus the coherent field
    /// of each scene edge that diffracts it once, from the incident shadow-boundary terms of Kouyoumjian and
    /// Pathak's wedge coefficient. Only edges whose diffraction points lie in the connection's Gaussian envelope
    /// contribute; corner and multiple diffraction are omitted.
    UtdEdgeDiffraction,
    /// Fraunhofer edge diffraction: the ballistic field plus, coherently, the boundary-diffraction waves of each
    /// interaction region's obstacle projected across the connection, integrated edge by edge with Steinberg et
    /// al.'s closed-form Fraunhofer edge waves under the same Gaussian beam as AngularSpectrum. Corners are included;
    /// each edge diffracts once, and only where the source and receiver see its stationary point.
    FraunhoferEdgeDiffraction,
};
/// RT1 RGB reflection input. Scattering selects legacy metallic/roughness,
/// Lambertian diffuse or isotropic rough conductor with complex-IOR Fresnel.
/// CoatedDiffuse is a rough dielectric layer over Lambertian reflection with
/// absorption and zero medium scattering. Both typed rough models use Scattering
/// alpha directly. Coatings require isotropy, alpha >= LUDUS_COATING_MIN_ALPHA,
/// and IOR >= 1. Their internal depth is LUDUS_COATING_MAX_DEPTH; it does not consume
/// scene bounces. Dielectric and ThinDielectric are smooth nonabsorbing interfaces
/// with one real index (equal Eta channels, zero K, alpha below
/// LUDUS_SMOOTH_INTERFACE_ALPHA): their reflection and transmission are directional
/// deltas, sampled only. Rough transmission, scattering media, absorption inside
/// dielectrics and spectral dispersion are absent.
struct ReferenceBsdf final
{
    /// Linear RGB reflectance, each channel in [0,1]; conductor evaluation uses Eta/K instead.
    ludus::foundation::math::Vector3 BaseColor{1, 1, 1};
    /// Metallic fraction in [0,1]. For DiffuseConductorMix, the weight and proposal probability of
    /// the rough conductor lobe. The legacy dielectric normal-incidence reflectance is 0.04.
    ludus::foundation::float32 Metallic = 0;
    /// Legacy perceptual roughness in [0.045,1]; typed conductor/coating alpha comes from Scattering.
    ludus::foundation::float32 Roughness = 0.5F;
    /// Typed optical parameters; default preserves the legacy metallic/roughness model.
    content::scene_format::MaterialScattering Scattering{};
    /// Closure program of a Scattering.Model Program material, borrowed for the duration of each
    /// evaluation or sampling call; null otherwise. BaseColor, Metallic and Roughness are its surface
    /// inputs. A Program material without a program accepted by IsMaterialProgramValid is invalid input.
    const content::scene_format::MaterialProgram* ProgramOrNull = nullptr;
    /// Dielectric only: the view direction lies inside the medium, behind the surface's
    /// oriented front side, so refraction uses the inverse relative index. Thin slabs ignore it.
    bool IsViewerInside = false;
    /// Declared reflection integral of the GGX lobes; values outside the enumeration are invalid input.
    ReferenceMaterialProfile Profile = ReferenceMaterialProfile::SingleScattering;
};
/// BSDF value and directional density in inverse steradians, without the cosine.
struct ReferenceBsdfEvaluation final
{
    /// Nonnegative linear RGB f; zero outside the shading-normal upper hemisphere.
    ludus::foundation::math::Vector3 Value{};
    /// Exact proposal density for legacy/diffuse/conductor; separate layered directional PDF estimate for coatings.
    ludus::foundation::float32 Pdf = 0;
};
/// One continuation proposal. Below-surface GGX proposals are explicit null events;
/// their probability mass is retained instead of retrying/renormalizing the sampler.
/// Dielectric proposals are specular: see IsSpecular.
struct ReferenceBsdfSample final
{
    /// Unit outgoing world direction when Accepted, otherwise zero.
    ludus::foundation::math::Vector3 Direction{};
    /// Evaluation/density when Accepted. A coating may return a proportional internal-path representation; see
    /// HasProportionalPdf.
    ReferenceBsdfEvaluation Evaluation{};
    /// False is an ordinary zero-contribution sample, distinct from invalid input.
    bool Accepted = false;
    /// Complete continuation throughput multiplier, including the outgoing cosine.
    /// Layered samples use their internal-path weight, independently of the MIS density.
    foundation::math::Vector3 Weight{};
    /// Directional density estimate used for external light/BSDF MIS, in inverse steradians.
    foundation::float32 MisPdf = 0;
    /// True when Evaluation is a proportional path representation: its f*cos/pdf
    /// ratio remains valid, but Evaluation.Pdf must not be used for external MIS.
    bool HasProportionalPdf = false;
    /// True for a directional delta lobe (dielectric reflection or transmission). Evaluation
    /// and MisPdf are zero, Weight is the complete throughput multiplier, Direction may lie
    /// below the surface (transmission), and no light sample can reach this lobe, so emission
    /// found along Direction takes full weight instead of a MIS weight.
    bool IsSpecular = false;
};
/// Evaluate the selected reflection profile. Normal, outgoing view and incoming light
/// must be finite unit vectors within 1e-5 squared-length tolerance. Directions
/// point away from the surface. Dielectrics evaluate to zero value and density for
/// every direction pair (their lobes are deltas). Coatings use one stochastic layered estimate and
/// an independently addressed MIS density estimate. Advance random.Address.Event
/// between estimates to converge. Invalid input preserves output; no allocation.
[[nodiscard]] rhi::RasterStatus EvaluateReferenceBsdf(const ReferenceBsdf&,
                                                      ludus::foundation::math::Vector3 normal,
                                                      ludus::foundation::math::Vector3 view,
                                                      ludus::foundation::math::Vector3 light,
                                                      ReferenceBsdfEvaluation&,
                                                      ReferenceBsdfRandomContext random = {}) noexcept;
/// Sample the matching mixture with three independent values in [0,1): X selects
/// the lobe, Y/Z sample it. Diffuse uses cosine sampling; conductors and legacy
/// pure metals use visible-GGX reflection; legacy mixed surfaces mix cosine diffuse
/// and GGX equally. MultipleScatteringGgx cosine samples the fraction 1 - E(view) of each GGX proposal for its
/// compensation lobe. DiffuseConductorMix uses Metallic as the conductor proposal
/// probability and returns the weighted sum of both lobe densities. ProjectBrdf
/// chooses one authored cosine-power lobe and returns the full mixture density. A closure program
/// chooses one of its lobes in proportion to the mean of its weight and returns the full mixture density.
/// Dielectrics reflect with probability equal to their
/// Fresnel reflectance (a thin slab's includes its interreflections) and otherwise refract, or pass straight through a
/// thin slab; transmitted radiance is scaled by the inverse squared relative index. Coatings sample an internal layered
/// path using the supplied random context; Weight and MisPdf have distinct contracts. Internal depth and roulette
/// termination are ordinary null samples. Failure preserves output. No allocation, retries, global RNG, energy clamp or
/// hidden roughness change.
[[nodiscard]] rhi::RasterStatus SampleReferenceBsdf(const ReferenceBsdf&,
                                                    ludus::foundation::math::Vector3 normal,
                                                    ludus::foundation::math::Vector3 view,
                                                    ludus::foundation::math::Vector3 samples,
                                                    ReferenceBsdfSample&,
                                                    ReferenceBsdfRandomContext random = {}) noexcept;
/// Selected traversal implementation, recorded in capture metadata.
enum class ReferenceTraversal : ludus::foundation::uint8
{
    /// Explicit asynchronous native BLAS/TLAS queries; unsupported devices reject.
    NativeQueries,
    /// Explicit CPU BVH oracle for validation/offline work. Never silently selected
    /// as a hardware fallback; the same retained geometry/material schema is used.
    CpuOracle,
    /// GPU reference: compute kernels traverse the same median BVH and run transport,
    /// shading and compensated accumulation on the device. Requires compute support,
    /// not native ray queries. Committed sums are read back only for copy/export.
    GpuCompute,
};
/// Direct-light next-event estimator, recorded in capture metadata. The modes estimate
/// the same truncated integral with different proposals and variance.
/// See "Light-sampling modes" in docs/architecture/renderer-systems.md.
enum class ReferenceLightSampling : ludus::foundation::uint8
{
    /// Sampler version 1: one power-CDF emitter triangle, one uniform area sample and its
    /// shadow ray. The default and the validation oracle; captures stay bit-identical.
    UniformArea,
    /// Sampler version 2: resampled importance sampling of one emitter triangle from 32
    /// power-CDF candidates with an unshadowed LTC target, then one projected-solid-angle
    /// and one LTC sample of it under clamped optimal MIS, so up to two shadow rays per
    /// vertex. BSDF hits weigh against a surrogate density with an 8-candidate estimate of
    /// the target's total. Requires the renderer's LTC table; GpuCompute kernels without it
    /// are rejected when reference rendering is enabled.
    ResampledProjectedSolidAngle,
    /// Sampler version 3: ResampledProjectedSolidAngle with a cheaper target, Hill and
    /// Heitz's approximate horizon-clipped form factors of the triangle and of its LTC
    /// transform instead of clipped polygon integrals, kept positive wherever the triangle
    /// has area above the horizon. It estimates the same integral with different variance
    /// and cost, and has its own frozen candidate counts. The selected emitter's samples,
    /// densities and MIS weights are those of ResampledProjectedSolidAngle. Same LTC table
    /// requirement and random layout.
    ResampledFormFactor,
    /// Sampler version 4: independently sample one directional light and one remaining
    /// source per eligible surface vertex. A lone directional light is always selected.
    /// Non-directional lights, emissive triangles and environment retain the UniformArea
    /// proposal within the remaining sources. Uses up to two shadow rays per vertex.
    /// CPU oracle and GPU compute only. Estimates the same geometric integral.
    SeparateDirectional,
};
/// Opt-in retained CPU geometry and native query setup; call before creating meshes.
/// Buffers are admitted against explicit budgets, rather than silently truncating
/// geometry, paths or pixels. Default zero budgets intentionally require authoring.
struct ReferenceRenderingOptions final
{
    /// Explicit traversal choice; native is the default request.
    ReferenceTraversal Traversal = ReferenceTraversal::NativeQueries;
    /// Renderer-owned retained mesh/capture CPU bytes, including transient setup work.
    ludus::foundation::usize CpuByteBudget = 0;
    /// Device-wide known ray buffer/AS bytes plus pending build scratch. Ordinary
    /// raster allocations and opaque driver/pipeline allocations are reported separately.
    ludus::foundation::usize RayByteBudget = 0;
    /// At most this many pixels advance in one tile; positive and at most 2048.
    /// Native mode also requires twice this count within RayCapabilities::MaxRays, and a
    /// ResampledProjectedSolidAngle or ResampledFormFactor capture three times it (two
    /// shadow rays per vertex);
    /// a capture whose count exceeds MaxRays is rejected as Unsupported.
    /// GpuCompute advances every pixel by one sample per dispatch and ignores the value.
    ludus::foundation::uint32 TilePixels = 0;
    /// GpuCompute only: renderer-owned packed scene, texel, uniform, accumulation and
    /// status buffer bytes across live captures, including a replacement built by
    /// UpdateReferenceCapture, plus the renderer's LTC table (3407872 bytes) and GGX albedo
    /// table (16640 bytes), which every capture shares and which are charged when rendering
    /// is enabled. Excludes RHI registries, transfer rings, pipelines and driver padding. At
    /// least both tables' size for GpuCompute; other traversals ignore it.
    ludus::foundation::usize GpuByteBudget = 0;
};
/// Corners of one ReferenceMediumDescription boundary triangle.
inline constexpr ludus::foundation::usize REFERENCE_MEDIUM_TRIANGLE_CORNERS = 3;
/// Most boundary triangles of one ReferenceMediumDescription. Both implementations test every
/// boundary triangle on every path segment and visibility ray, so cost grows linearly with it.
inline constexpr ludus::foundation::usize MAX_REFERENCE_MEDIUM_TRIANGLES = 4096;
/// Largest admitted magnitude, in world metres, of each coordinate of a ReferenceMediumDescription
/// boundary corner. Applications that generate a boundary check their corners against it before a capture.
inline constexpr ludus::foundation::float32 MAX_REFERENCE_MEDIUM_COORDINATE_WORLD = 1000000;
/// One bounded homogeneous absorbing medium for CpuOracle and GpuCompute reference captures.
///
/// The boundary is an invisible, index-matched interface (pbrt-v4's `interface` material): it
/// neither scatters nor occludes, and it is not part of the snapshot, so raster, GPU preview and
/// render-graph passes never see it. It must be a closed, consistently oriented triangle mesh whose
/// oriented front side (counterclockwise corners, right-hand normal) faces the outside: every
/// directed edge appears exactly once and its reverse exactly once, with bit-identical corners.
/// Several disjoint closed pieces form one medium; overlapping or nested media are unsupported.
///
/// Both tracers follow every camera, continuation and light-visibility segment through the
/// boundary's ordered crossings. A crossing against the outward normal enters the medium, one
/// along it leaves; the camera's initial state is the crossing count along its primary ray, so
/// camera origins inside the medium need no extra input. Each segment is attenuated by the
/// Beer-Lambert transmittance exp(-AbsorptionPerWorld * length inside), per RGB channel. Light
/// sources and scattering surfaces inside the medium are allowed. A segment whose crossings leave
/// the inside count outside {0, 1}, for example through an open boundary or a ray through an edge
/// that was not resolved, fails its path with ReferenceFailure::InvalidMediumCrossing.
///
/// Thanks to Julian Fong, Magnus Wrenninge, Christopher Kulla and Ralf Habel, "Production
/// Volume Rendering", SIGGRAPH 2017 course, sections 2.3.1, 3.2 and 3.4,
/// https://doi.org/10.1145/3084873.3084907, for closed-form homogeneous transmittance and
/// transmittance accumulated over ordered boundary crossings.
struct ReferenceMediumDescription final
{
    /// Borrowed world-space corners, REFERENCE_MEDIUM_TRIANGLE_CORNERS per triangle for
    /// 1..MAX_REFERENCE_MEDIUM_TRIANGLES triangles, copied during Begin/Update. Each corner is finite and within
    /// +/-MAX_REFERENCE_MEDIUM_COORDINATE_WORLD metres; every triangle has positive area.
    ludus::foundation::BorrowedView<const ludus::foundation::math::Vector3> BoundaryCornersWorld;
    /// Linear RGB absorption coefficient sigma_a of the enclosed medium, per world metre. Each channel
    /// is finite and in [0, content::scene_format::MAX_MEDIUM_ABSORPTION_PER_WORLD]; zero is vacuum.
    ludus::foundation::math::Vector3 AbsorptionPerWorld{};
};
/// Immutable reference integral and stop target. Camera coordinates and matrices
/// match raster canonical XY [-1,1], depth [0,1], Y up, top-left image rows.
struct ReferenceCaptureDescription final
{
    /// Exact unjittered raster world-to-clip matrix, finite and invertible.
    ludus::foundation::math::Matrix4 WorldToClip = ludus::foundation::math::Matrix4::Identity();
    /// Perspective pinhole position in world meters; ignored for orthographic origins.
    ludus::foundation::math::Vector3 CameraPosition{0, 0, 1};
    /// Orthographic rays start on the unprojected near plane; perspective starts at the pinhole.
    bool OrthographicCamera = false;
    /// False uses near depth zero/far one; true uses reverse-Z near one/far zero.
    /// Primary clipping follows this raster convention; secondary ranges are independent.
    bool ReverseZ = false;
    /// Infinite reverse-Z perspective far plane; secondary distance still stays bounded.
    bool InfiniteFar = false;
    /// Physical capture width; positive and admitted within the CPU budget.
    ludus::foundation::uint32 Width = 0;
    /// Physical capture height; positive and admitted within the CPU budget.
    ludus::foundation::uint32 Height = 0;
    /// Minimum committed samples per pixel requested before completion, 1..16777215.
    /// Increasing only this value continues the same accumulation.
    ludus::foundation::uint32 TargetSamples = 1;
    /// Maximum scattering events, 1..64. Emission/environment after the last event
    /// is evaluated. This explicitly truncates transport and introduces depth bias.
    ludus::foundation::uint32 MaxDepth = 4;
    /// Begin compensated Russian roulette after this many scattering events, 1..64.
    ludus::foundation::uint32 RouletteDepth = 3;
    /// Finite positive secondary-ray range in meters, at most 2000000.
    ludus::foundation::float32 SecondaryDistance = 1000000;
    /// Constant linear environment radiance in [0,10000], independently sampled
    /// with uniform sphere density. No baked lighting, AO or prefiltered IBL is added.
    ludus::foundation::math::Vector3 Environment{};
    /// Optional ready HDR map, retained at Begin/Update; replaces Environment.
    /// Reference transport samples raw source radiance and its solid-angle CDF,
    /// never raster prefiltered mips or diffuse SH. Same device is required.
    const EnvironmentMap* EnvironmentTextureOrNull = nullptr;
    /// Borrowed analytic directional/point/spot lights, validated and copied during
    /// Begin/Update. Existing L4 range/spot windows are part of this declared profile.
    const DirectLight* Lights = nullptr;
    /// At most 128 unique lights; no L4 eight-light selection or overflow dropping.
    ludus::foundation::usize LightCount = 0;
    /// Fixed integer texture mip for both CPU shading and native opacity acceptance.
    /// Overrides per-material LOD selection, mip interpolation and LOD clamps.
    /// Every sampled color/surface/emission texture must retain this CPU mip.
    /// NativeQueries rejects authored masked materials until native opacity can
    /// evaluate routed UVs and vertex alpha; CpuOracle/GPU preview support them.
    ludus::foundation::uint32 TextureMip = 0;
    /// Reproducible Philox capture seed; sampling is addressed by pixel/sample/dimension.
    ludus::foundation::uint64 Seed = 0;
    /// Disable pixel jitter for primary-ray AOV/raster parity experiments. Recorded
    /// in the integral key; this estimates center rays rather than pixel footprints.
    bool CenterRays = false;
    /// Direct-light next-event estimator. Recorded in the integral key: changing it
    /// replaces the job and resets accumulation. Values outside the enumeration are rejected.
    ReferenceLightSampling LightSampling = ReferenceLightSampling::UniformArea;
    /// Reflection integral of GGX lobes, applied by every traversal. Recorded in the integral key: changing it replaces
    /// the job and resets accumulation. Values outside the enumeration are rejected.
    ReferenceMaterialProfile MaterialProfile = ReferenceMaterialProfile::SingleScattering;
    /// Field operator of the optical connections that a positive OpticalWavelengthWorld, declared next, enables.
    /// Recorded in the integral key: changing it replaces the job and resets accumulation. UtdEdgeDiffraction or
    /// FraunhoferEdgeDiffraction with a zero wavelength, and values outside the enumeration, are rejected as
    /// InvalidDescription.
    ReferenceOpticalConnection OpticalConnection = ReferenceOpticalConnection::AngularSpectrum;
    /// Zero retains geometric reference transport. Positive metres enable scalar paraxial Gaussian-mode
    /// coupling on finite light connections, including coherent diffraction by successive scene regions.
    /// The positive range starts at one nanometre; smaller values reject before capture preparation.
    /// Rough surface scattering stays RGB and unpolarized. Uses next-event estimation alone for light endpoints
    /// reached from rough vertices: continuation emission/environment after them is omitted, avoiding double counting
    /// with optical visibility. Emission reached through glass (smooth dielectric reflection and transmission, which
    /// no light sample reaches) keeps full weight; its connection is relayed coherently back through the glass events
    /// to the last rough vertex or the camera. Requires UniformArea and CpuOracle/GpuCompute; NativeQueries rejects
    /// this profile. Lattice, projection and range limits are documented in render-graph-authoring.md, "Wave tracing".
    /// Changing wavelength resets accumulation. Nonfinite/negative values are rejected. No silent fallback.
    foundation::float32 OpticalWavelengthWorld = 0;
    /// Optional borrowed bounded absorbing medium, validated and copied during Begin/Update; null is
    /// vacuum and keeps every existing capture bit for bit. The medium joins the integral key:
    /// changing its corners or absorption replaces the job. CpuOracle and GpuCompute only:
    /// NativeQueries, GPU preview and positive OpticalWavelengthWorld report Unsupported, so an
    /// application selects one of the two supported traversals explicitly. Invalid boundaries and
    /// coefficients report InvalidDescription.
    const ReferenceMediumDescription* MediumOrNull = nullptr;
    /// Optional World atmosphere for camera rays that miss geometry. Begin/Update resolve and copy
    /// its celestial boundary at CameraPosition when the observer is inside the atmosphere shell.
    /// Solar and lunar disks, stars and planets contribute scene-linear background radiance after
    /// extinction and ground occlusion. Environment and Lights still supply surface lighting.
    /// When ShouldLightScene is true, supply the resolved atmosphere as EnvironmentTextureOrNull.
    /// Camera misses then reconstruct its RGB radiance with bilinear filtering.
    /// Null or disabled leaves the ordinary background. Invalid active fields reject the capture.
    const AtmosphereDescription* AtmosphereBackgroundOrNull = nullptr;
};
/// Renderer-scoped progressive job identity; copies borrow, Release cancels future
/// work and detaches accepted query uses through their native completion.
struct ReferenceCapture final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    /// Private registry access; applications cannot construct capture identities.
    friend struct internal::Access;
};
/// Raw unexposed linear means and sample count for one top-left-oriented pixel.
/// AOVs sum to Radiance: primary visible emission/sky, first-surface direct transport,
/// and later indirect transport. Overlays, denoising and tone mapping are excluded.
struct ReferencePixel final
{
    /// Scene-linear RGB mean, zero while Samples is zero.
    ludus::foundation::math::Vector3 Radiance{};
    /// Primary visible emission/environment mean.
    ludus::foundation::math::Vector3 Emission{};
    /// First-surface direct lighting mean, including BSDF-hit emitters/environment.
    ludus::foundation::math::Vector3 Direct{};
    /// Later-surface transport mean.
    ludus::foundation::math::Vector3 Indirect{};
    /// Committed samples; failed or uncommitted tile work never increments it.
    ludus::foundation::uint32 Samples = 0;
};
/// Validation stage that rejected an uncommitted reference path or tile.
/// This records a failure, never an ordinary zero-contribution or null BSDF sample.
enum class ReferenceFailure : ludus::foundation::uint8
{
    /// No path validation failure has been recorded.
    None,
    /// Traversal returned an invalid distance, barycentric coordinate or primitive identity.
    InvalidHit,
    /// The competing emissive-surface density was negative or nonfinite.
    InvalidEmitterPdf,
    /// Decoding authored surface parameters failed.
    InvalidSurfaceParameters,
    /// A light proposal had an invalid density or radiance.
    InvalidLightSample,
    /// Evaluation of the surface scattering function failed.
    InvalidBsdfEvaluation,
    /// The weighted direct-light contribution was negative or nonfinite.
    InvalidDirectContribution,
    /// Offsetting or constructing the finite visibility segment failed.
    InvalidShadowSegment,
    /// Sampling the surface scattering function failed.
    InvalidBsdfSample,
    /// Updated path throughput was negative or nonfinite.
    InvalidThroughput,
    /// A generated ray origin was nonfinite or outside the admitted world range.
    InvalidRayOrigin,
    /// Accumulated path radiance was negative or nonfinite.
    InvalidRadiance,
    /// Publishing the tile would overflow its accumulated pixel sums.
    InvalidAccumulation,
    /// Constructing a camera ray from the admitted projection failed.
    InvalidCameraRay,
    /// The compute shader received an incompatible packed scene layout.
    InvalidSceneLayout,
    /// The GPU traversal exhausted its bounded pending-node stack.
    TraversalCapacityExceeded,
    /// An optical interaction region contains too many triangles; no partial connection was published.
    OpticalRegionCapacity,
    /// Optical propagation or ballistic escape exhausted the bounded interaction steps.
    OpticalPropagationCapacity,
    /// A segment's ordered medium-boundary crossings left the inside count outside {0, 1}.
    InvalidMediumCrossing,
    /// An optical connection relayed through glass found the dielectrics on one of its legs disagreeing about the
    /// leg's medium (touching or nested glass of different indices), or more specular events than it admits.
    OpticalInterface,
};

/// Bounded capture progress, reproducibility and failure evidence.
struct ReferenceCaptureInfo final
{
    /// Explicit selected traversal; NativeQueries never denotes a CPU fallback.
    ReferenceTraversal Traversal = ReferenceTraversal::NativeQueries;
    /// RGB surface/integrator schema: version 4 records emitter sidedness and
    /// weighted area samples; earlier surface-map/range semantics are preserved.
    ludus::foundation::uint32 ProfileVersion = LIGHTING_PROFILE_VERSION;
    /// Frozen Philox address and dimension convention version: 1 for UniformArea (8
    /// dimensions per bounce), 2 for ResampledProjectedSolidAngle and 3 for
    /// ResampledFormFactor (128 per bounce), and 4 for SeparateDirectional (UniformArea
    /// addresses plus one independent directional draw per bounce).
    ludus::foundation::uint32 SamplerVersion = 1;
    /// The capture's direct-light next-event estimator.
    ReferenceLightSampling LightSampling = ReferenceLightSampling::UniformArea;
    /// The capture's declared reflection integral of GGX lobes.
    ReferenceMaterialProfile MaterialProfile = ReferenceMaterialProfile::SingleScattering;
    /// Original capture seed.
    ludus::foundation::uint64 Seed = 0;
    /// Nonwrapping accepted integral revision; changes reset every pixel's accumulation.
    ludus::foundation::uint64 Revision = 1;
    /// Non-cryptographic frozen scene/content fingerprint, low 64 bits; diagnostic,
    /// never the sole identity or accumulation-invalidation key.
    ludus::foundation::uint64 SceneFingerprintLow = 0;
    /// High 64 bits of the same versioned canonical-content fingerprint.
    ludus::foundation::uint64 SceneFingerprintHigh = 0;
    /// Physical image width.
    ludus::foundation::uint32 Width = 0;
    /// Physical image height.
    ludus::foundation::uint32 Height = 0;
    /// Smallest committed pixel count; zero exposes incomplete coverage.
    ludus::foundation::uint32 MinimumSamples = 0;
    /// Largest committed pixel count; tile progress may temporarily differ.
    ludus::foundation::uint32 MaximumSamples = 0;
    /// Desired per-pixel completion target.
    ludus::foundation::uint32 TargetSamples = 0;
    /// Declared truncated scattering depth.
    ludus::foundation::uint32 MaxDepth = 0;
    /// Committed path samples across all pixels.
    ludus::foundation::uint64 CommittedSamples = 0;
    /// Accepted primary/continuation queries, excluding visibility queries.
    ludus::foundation::uint64 ClosestQueries = 0;
    /// Accepted opacity-filtered visibility queries.
    ludus::foundation::uint64 VisibilityQueries = 0;
    /// Zero-area world triangles retained for stable source primitive identity but omitted from transport.
    /// Their presence does not reject an otherwise valid scene. Matches across CPU and GPU reference paths.
    foundation::uint64 SkippedTriangles = 0;
    /// Ordinary null BSDF proposals; no density renormalization or sample dropping.
    ludus::foundation::uint64 NullBsdfSamples = 0;
    /// Paths ended by compensated roulette.
    ludus::foundation::uint64 RouletteTerminations = 0;
    /// Invalid PDF/nonfinite contributions; any such event fails the uncommitted tile.
    ludus::foundation::uint64 InvalidContributions = 0;
    /// Pixel index of the last invalid contribution, meaningful only after failure.
    ludus::foundation::uint32 FailurePixel = 0;
    /// Scattering depth of the last invalid contribution.
    ludus::foundation::uint32 FailureDepth = 0;
    /// Validation stage of the last invalid contribution; None until a path check fails.
    ReferenceFailure Failure = ReferenceFailure::None;
    /// Zero-based sample index of that path; meaningful when InvalidContributions is nonzero.
    ludus::foundation::uint32 FailureSample = 0;
    /// Renderer-owned CPU bytes retained by this job, excluding shared mesh/texture bytes.
    ludus::foundation::usize CpuBytes = 0;
    /// False while native builds/query results or further samples are outstanding.
    bool Complete = false;
};
} // namespace ludus::graphics::renderer
