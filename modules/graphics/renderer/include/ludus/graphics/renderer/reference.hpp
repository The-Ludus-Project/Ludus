#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/rhi/ray.h>

namespace ludus::graphics::renderer
{
/// Frozen RT1 RGB metallic/roughness reflection model. The L4 single-scattering
/// GGX/correlated Smith/Schlick and Fresnel-weighted Lambert evaluation is shared;
/// transmission, volumes, spectral transport and energy compensation are absent.
struct ReferenceBsdf final
{
    /// Linear RGB reflectance, each channel in [0,1].
    ludus::foundation::math::Vector3 BaseColor{1, 1, 1};
    /// Metallic fraction in [0,1]; dielectric normal-incidence reflectance is 0.04.
    ludus::foundation::float32 Metallic = 0;
    /// Perceptual roughness in [0.045,1]; microfacet alpha is its square.
    ludus::foundation::float32 Roughness = 0.5F;
};
/// BSDF value and full mixture density in inverse steradians, without the cosine.
struct ReferenceBsdfEvaluation final
{
    /// Nonnegative linear RGB f; zero outside the shading-normal upper hemisphere.
    ludus::foundation::math::Vector3 Value{};
    /// Full cosine-diffuse/visible-GGX reflection mixture PDF, not a selected-lobe PDF.
    ludus::foundation::float32 Pdf = 0;
};
/// One continuation proposal. Below-surface GGX proposals are explicit null events;
/// their probability mass is retained instead of retrying/renormalizing the sampler.
struct ReferenceBsdfSample final
{
    /// Unit outgoing world direction when Accepted, otherwise zero.
    ludus::foundation::math::Vector3 Direction{};
    /// Matching full-mixture evaluation and density when Accepted.
    ReferenceBsdfEvaluation Evaluation{};
    /// False is an ordinary zero-contribution sample, distinct from invalid input.
    bool Accepted = false;
};
/// Evaluate the frozen reflection profile. Normal, outgoing view and incoming light
/// must be finite unit vectors within 1e-5 squared-length tolerance. Directions
/// point away from the surface. Invalid input preserves output; no allocation.
[[nodiscard]] rhi::RasterStatus EvaluateReferenceBsdf(const ReferenceBsdf&,
                                                      ludus::foundation::math::Vector3 normal,
                                                      ludus::foundation::math::Vector3 view,
                                                      ludus::foundation::math::Vector3 light,
                                                      ReferenceBsdfEvaluation&) noexcept;
/// Sample the matching mixture with three independent values in [0,1): X selects
/// the lobe, Y/Z sample it. Pure metals use only visible-GGX reflection; other
/// materials mix equal-probability cosine diffuse and GGX. Failure preserves output.
/// No allocation, retries, mutable RNG, energy clamp or hidden roughness change.
[[nodiscard]] rhi::RasterStatus SampleReferenceBsdf(const ReferenceBsdf&,
                                                    ludus::foundation::math::Vector3 normal,
                                                    ludus::foundation::math::Vector3 view,
                                                    ludus::foundation::math::Vector3 samples,
                                                    ReferenceBsdfSample&) noexcept;
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
/// Next-event estimator for emissive triangles, recorded in capture metadata. Both modes
/// estimate the same truncated integral, so their means agree statistically, never per
/// pixel. Analytic lights, the environment and the uniform category draw are shared. See
/// docs/architecture/many-area-light-sampling-reference-review.md for the estimator.
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
    /// UpdateReferenceCapture, plus the renderer's LTC table (3407872 bytes), which every
    /// capture shares and which is charged when rendering is enabled. Excludes RHI
    /// registries, transfer rings, pipelines and driver padding. At least the table's
    /// size for GpuCompute; other traversals ignore it.
    ludus::foundation::usize GpuByteBudget = 0;
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
    /// Emissive-triangle next-event estimator. Recorded in the integral key: changing it
    /// replaces the job and resets accumulation. Values outside the enumeration are rejected.
    ReferenceLightSampling LightSampling = ReferenceLightSampling::UniformArea;
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
    /// ResampledFormFactor (128 per bounce).
    ludus::foundation::uint32 SamplerVersion = 1;
    /// The capture's emissive-triangle next-event estimator.
    ReferenceLightSampling LightSampling = ReferenceLightSampling::UniformArea;
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
