#pragma once

// RT1's resampled next-event estimator for emissive triangles: ReferenceLightSampling::ResampledProjectedSolidAngle
// (sampler version 2) and ResampledFormFactor (sampler version 3), which differ only in how the RIS target evaluates
// E_0 and E_1 and in their frozen candidate counts. reference_path.cpp calls it for the emissive category of the
// uniform light draw and for the BSDF-hit MIS density; renderer_reference.slang ports every function below with the
// same addressed dimensions. The estimator and its decisions are
// docs/architecture/many-area-light-sampling-reference-review.md ("Proposed estimator", "Decision: BSDF-hit MIS", "RT1
// profile split and identity"); the ris-projected-solid-angle render graph template
// (render_graphs/ris_projected_solid_angle/main_shaders) implements the same estimator for one bounce.
//
// Thanks to Ishaan Shah, Aakash KT and P. J. Narayanan, "Combining Resampled Importance and Projected Solid Angle
// Samplings for Many Area Light Rendering", SIGGRAPH Asia 2023 Technical Communications,
// https://doi.org/10.1145/3610543.3626165, sections 3 and 4 and Algorithm 1: lights, not points, are resampled with
// the unshadowed LTC shading of the whole light as the target. Their single-lobe limitation (section 6) is addressed
// with both of Peters' techniques.
// Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4),
// Article 140, 2021, https://doi.org/10.1145/3450626.3459672: section 4.1 (LTC sampling, which an empty LTC-space clip
// disables), section 4.2 (coefficients c_j = L_e a_j E_j and diffuse albedo clamped to 0.01 per channel) and section
// 4.3 (clamped optimal MIS with visibility estimate v). Implemented from the paper's text; the coefficients are reduced
// to BT.709 luminance, one scalar weight per technique, where Peters weights each color channel.
// Thanks to Chris Wyman, "Weighted Reservoir Sampling: Randomly Sampling Streams", Ray Tracing Gems II, chapter 22,
// 2021, for the single-sample streaming update, used through FoundationMath's weighted_reservoir.hpp.
// Thanks to Stephen Hill and Eric Heitz, "Real-Time Area Lighting: a Journey from Research to Production", SIGGRAPH
// 2016 Advances in Real-Time Rendering course, https://advances.realtimerendering.com/s2016/, slides 77 and 90-102:
// sampler version 3's target evaluates their vector form factor and clipped proxy sphere (FoundationMath's
// TryApproximateClampedCosinePolygonIntegral) instead of clipped polygon integrals, as Shah et al. do for speed.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/rhi/raster.h>

#include "../../shaders/renderer_reference_layout.h"

namespace ludus::graphics::renderer::internal
{
struct ReferenceJob;

/// Peters' visibility estimate v of the clamped optimal MIS between the two light techniques (section 4.3); a frozen
/// constant of sampler version 2.
inline constexpr foundation::float32 REFERENCE_LIGHT_TECHNIQUE_VISIBILITY = 0.5F;
/// Light techniques of one resampled emitter: the diffuse projected-solid-angle sample and the specular LTC sample.
inline constexpr foundation::usize REFERENCE_LIGHT_TECHNIQUE_COUNT = 2;
/// Sampler version 3's approximate E_0 is at least this fraction of the unit-direction triple product, a positive proxy
/// of the triangle's unclipped solid angle, whenever a vertex lies above the horizon: Hill and Heitz's proxy sphere can
/// lie below the horizon while part of the triangle is above it, and the RIS support invariant needs p-hat > 0 there.
inline constexpr foundation::float32 REFERENCE_APPROXIMATE_TARGET_DEFENSIVE_FRACTION = 0.015625F;

/// The receiver's shading frame (+z the unit shading normal) and its two light techniques. They guide sampling and
/// weights only; the BSDF is always evaluated exactly. A path stores the previous vertex's frame so that the BSDF-hit
/// MIS density reads exactly the inputs that the vertex's light sample used.
struct ReferenceReceiverFrame final
{
    foundation::math::Vector3 PositionWorld{};
    foundation::math::Vector3 TangentWorld{};
    foundation::math::Vector3 BitangentWorld{};
    foundation::math::Vector3 NormalWorld{};
    /// M_1: the specular LTC; the identity when HasSpecular is false.
    foundation::math::LinearlyTransformedCosine Specular{};
    bool HasSpecular = false;
    /// Y(a_0): luminance of the clamped diffuse albedo.
    foundation::float32 DiffuseAlbedoLuminance = 0;
    /// Y(a_1): luminance of the specular LTC's Fresnel-weighted albedo.
    foundation::float32 SpecularAlbedoLuminance = 0;
};

/// Z-hat: the estimate of the target's total over every emitter that the BSDF-hit surrogate density divides by. It is
/// a pure function of (pixel, sample, depth) and the receiver frame, computed at most once per vertex.
struct ReferenceTargetTotal final
{
    foundation::float32 Value = 0;
    bool IsReady = false;
};

/// One emissive triangle prepared for both light techniques at one receiver.
struct ReferenceTriangleLight final
{
    /// The triangle in the shading frame clipped to z >= 0 (technique 0, density p_0 = cos(theta) / (pi E_0)).
    foundation::math::ProjectedSolidAnglePolygon Diffuse{};
    /// The triangle transformed by M_1^-1 and clipped there (technique 1, density p_1 = p_M1 / E_1).
    foundation::math::LinearlyTransformedCosinePolygon Specular{};
    /// E_0: the clipped projected solid angle over pi.
    foundation::float32 DiffuseIntegral = 0;
    /// E_1: the LTC integral; zero when the LTC-space clip is empty or the receiver has no specular LTC.
    foundation::float32 SpecularIntegral = 0;
};

/// One technique's sample of the resampled emitter, before the BSDF and visibility are known.
struct ReferenceResampledLightSample final
{
    /// Unit world direction from the receiver to PointWorld.
    foundation::math::Vector3 DirectionWorld{};
    /// The point where DirectionWorld meets the emitter's plane, inside the triangle.
    foundation::math::Vector3 PointWorld{};
    /// Emitted radiance towards the receiver after sidedness, opacity and range; may be zero.
    foundation::math::Vector3 Radiance{};
    /// Distance from the receiver to PointWorld.
    foundation::float32 DistanceWorld = 0;
    /// p_s: this technique's solid-angle density at DirectionWorld.
    foundation::float32 TechniqueDensity = 0;
    /// w_s: Peters' clamped optimal MIS weight of this technique.
    foundation::float32 TechniqueWeight = 0;
    /// q = P-tilde(y) (p_0 + p_1): the light strategy's surrogate density against BSDF sampling.
    foundation::float32 LightDensity = 0;
    /// False when the technique is disabled or its direction misses the triangle; the sample then adds nothing.
    bool HasSample = false;
};

/// The emissive branch of one vertex's light draw: the resampled triangle and one sample per technique. The
/// contribution of sample s is f cos(theta) L w_s PowerWeight(q, p_bsdf) / p_s times ContributionScale.
struct ReferenceResampledLight final
{
    /// The selected emitter triangle; meaningful only when HasSelection is true.
    foundation::uint32 TriangleIndex = 0;
    /// W / P_E: the reservoir's contribution weight over the emissive category probability.
    foundation::float32 ContributionScale = 0;
    foundation::StaticArray<ReferenceResampledLightSample, REFERENCE_LIGHT_TECHNIQUE_COUNT> Samples{};
    /// False when no candidate had a positive target, a valid zero contribution.
    bool HasSelection = false;
};

/// BT.709 luminance of a linear RGB value.
[[nodiscard]] foundation::float32 ComputeReferenceLuminance(foundation::math::Vector3 value) noexcept;

/// Build the frame and both techniques once per receiver. normalWorld and viewWorld are unit vectors. The diffuse
/// albedo approximates the Fresnel-weighted Lambert lobe, baseColor (1 - metallic) (1 - F0) with Schlick's F0 at
/// normal incidence, clamped per channel to 0.01; the specular LTC comes from the renderer's table at the receiver's
/// roughness, view and F0. A failed LTC construction disables the specular technique.
[[nodiscard]] ReferenceReceiverFrame MakeReferenceReceiverFrame(foundation::math::Vector3 positionWorld,
                                                                foundation::math::Vector3 normalWorld,
                                                                foundation::math::Vector3 viewWorld,
                                                                const ReferenceBsdf& bsdf) noexcept;

/// Prepare a triangle's two polygons. Returns false for a nonfinite vertex in the shading frame or after the LTC
/// transform; an empty or edge-on clip succeeds with a zero integral, which disables that technique.
[[nodiscard]] bool TryPrepareReferenceTriangleLight(const ReferenceReceiverFrame& frame,
                                                    foundation::math::Vector3 position0World,
                                                    foundation::math::Vector3 position1World,
                                                    foundation::math::Vector3 position2World,
                                                    ReferenceTriangleLight& outLight) noexcept;

/// Sampler version 3's approximate (E_0, E_1) without clipping or preparing either polygon: Hill and Heitz's clipped
/// proxy-sphere form factor of the triangle and of its transform by the specular LTC (zero without one). E_0 is zero
/// when no vertex lies above the horizon and otherwise at least REFERENCE_APPROXIMATE_TARGET_DEFENSIVE_FRACTION of the
/// unit-direction triple product, so it is positive wherever the exact E_0 is.
/// @return False for a nonfinite vertex in the shading frame or after the LTC transform, leaving outIntegrals
/// unchanged.
[[nodiscard]] bool TryApproximateReferenceTriangleIntegrals(const ReferenceReceiverFrame& frame,
                                                            foundation::math::Vector3 position0World,
                                                            foundation::math::Vector3 position1World,
                                                            foundation::math::Vector3 position2World,
                                                            foundation::math::Vector2& outIntegrals) noexcept;

/// The two techniques' solid-angle densities (p_0, p_1) for a unit world direction through the triangle; zero for a
/// technique that cannot sample it. No containment test is performed.
[[nodiscard]] foundation::math::Vector2
EvaluateReferenceLightTechniqueDensities(const ReferenceReceiverFrame& frame,
                                         const ReferenceTriangleLight& light,
                                         foundation::math::Vector3 directionWorld) noexcept;

/// Peters' clamped optimal MIS weights (w_0, w_1) for one sample per technique: visibility times the weighted balance
/// heuristic c_j p_j / sum_k c_k p_k plus (1 - visibility) times the balance heuristic p_j / sum_k p_k. Inputs are
/// finite and nonnegative. The weights sum to one wherever a density is positive and are zero where neither technique
/// samples; a technique with zero density gets zero weight. Where every c_j p_j is zero the weighted part falls back
/// to the balance heuristic.
[[nodiscard]] foundation::math::Vector2 ComputeReferenceLightTechniqueWeights(foundation::math::Vector2 coefficients,
                                                                              foundation::math::Vector2 densities,
                                                                              foundation::float32 visibility) noexcept;

/// The surrogate probability P-tilde(t) = P_E min(p-hat(t) / Z-hat, min(1, M p_src(t))) that the resampled light
/// strategy proposes triangle t. When Z-hat is zero and p-hat positive the ratio is infinite, so the inclusion bound
/// applies; a zero target, source probability or emissive category probability returns zero.
[[nodiscard]] foundation::float32 EvaluateReferenceSurrogateProbability(foundation::float32 emissiveCategoryProbability,
                                                                        foundation::float32 target,
                                                                        foundation::float32 sourcePmf,
                                                                        foundation::float32 targetTotal,
                                                                        foundation::uint32 candidateCount) noexcept;

/// Map a direction from originWorld to the point where it meets the triangle's plane. outBarycentric weights
/// position1World (X) and position2World (Y). Returns false, leaving outputs unchanged, for a direction parallel to
/// the plane, a plane behind the origin or a point outside the triangle in FP32; such a sample contributes zero.
[[nodiscard]] bool TryIntersectReferenceTrianglePlane(foundation::math::Vector3 originWorld,
                                                      foundation::math::Vector3 directionWorld,
                                                      foundation::math::Vector3 position0World,
                                                      foundation::math::Vector3 position1World,
                                                      foundation::math::Vector3 position2World,
                                                      foundation::float32& outDistanceWorld,
                                                      foundation::math::Vector2& outBarycentric) noexcept;

/// Resample one emitter at the receiver of vertex `depth` and draw one sample per light technique, under the job's
/// resampled light-sampling mode. Reads the mode's candidate slots (from 12), LTC slots 8-9, diffuse slots 1-2 and,
/// through the surrogate density, its normalization slots (from 76) of that vertex. emissiveCategoryProbability is P_E,
/// the uniform category draw's emitter share. inoutTargetTotal caches Z-hat for the vertex's BSDF-hit density.
/// @return Ready, with outLight written (possibly without a selection); InvalidDescription for a nonfinite target,
/// reservoir or Z-hat evaluation, leaving outLight unchanged.
[[nodiscard]] rhi::RasterStatus SampleReferenceResampledLight(const ReferenceJob& job,
                                                              foundation::uint32 pixel,
                                                              foundation::uint32 sample,
                                                              foundation::uint32 depth,
                                                              const ReferenceReceiverFrame& frame,
                                                              foundation::float32 emissiveCategoryProbability,
                                                              ReferenceTargetTotal& inoutTargetTotal,
                                                              ReferenceResampledLight& outLight) noexcept;

/// The light strategy's density q = P-tilde(t) (p_0 + p_1) for a BSDF direction from the receiver of vertex `depth`
/// that reached emitter triangle `triangle`, with Z-hat from inoutTargetTotal or that vertex's normalization slots.
/// @return Ready with outDensity written; InvalidDescription for a nonfinite evaluation, leaving outDensity unchanged.
[[nodiscard]] rhi::RasterStatus EvaluateReferenceResampledLightDensity(const ReferenceJob& job,
                                                                       foundation::uint32 pixel,
                                                                       foundation::uint32 sample,
                                                                       foundation::uint32 depth,
                                                                       const ReferenceReceiverFrame& frame,
                                                                       foundation::float32 emissiveCategoryProbability,
                                                                       foundation::uint32 triangle,
                                                                       foundation::math::Vector3 directionWorld,
                                                                       ReferenceTargetTotal& inoutTargetTotal,
                                                                       foundation::float32& outDensity) noexcept;
} // namespace ludus::graphics::renderer::internal
