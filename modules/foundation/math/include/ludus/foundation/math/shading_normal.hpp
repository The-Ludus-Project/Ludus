#pragma once

// Shading normals clipped to the hemisphere of the outgoing direction.
//
// Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights",
// ACM Transactions on Graphics 40(4), Article 140, 2021, section 6.1,
// https://doi.org/10.1145/3450626.3459672: "Shading normals get clipped to the
// hemisphere of the outgoing light direction to avoid black pixels due to normal
// mapping." The paper names neither the clip's construction nor a margin; both
// are Ludus's and are listed on the FoundationMath module page
// (docs/modules/foundation/math.md, "Shading normal clipping"). Implemented from
// the paper's text.
//
// The shader port with matching numerics is shaders/shading_normal.slang.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math
{
using core::float32;

/// @brief Cosine with the outgoing direction below which ClipShadingNormalToOutgoingHemisphere moves a normal.
///
/// Peters 2021, section 6.1 gives no margin. Ludus keeps the clipped normal this far inside the hemisphere because
/// the standard BSDF is zero for an outgoing direction on the shading horizon. After renormalization the clipped
/// cosine is at least SHADING_NORMAL_MINIMUM_OUTGOING_COSINE / sqrt(1 + SHADING_NORMAL_MINIMUM_OUTGOING_COSINE^2).
/// FoundationMath's shaders/shading_normal.slang declares the same value.
inline constexpr float32 SHADING_NORMAL_MINIMUM_OUTGOING_COSINE = 0.01F;

/// @brief Clip a unit shading normal to the hemisphere of a unit outgoing direction (Peters 2021, section 6.1).
///
/// A normal whose cosine with outgoingDirection is at least SHADING_NORMAL_MINIMUM_OUTGOING_COSINE is returned
/// unchanged. Otherwise the normal moves along outgoingDirection until that cosine is
/// SHADING_NORMAL_MINIMUM_OUTGOING_COSINE and is renormalized, which keeps the result continuous in both inputs; a
/// normal facing directly away becomes outgoingDirection. Callers shade with the result everywhere it stands for the
/// surface: the shading frame, the light sampling techniques and the BSDF.
///
/// @param normal Unit shading normal, for example an interpolated or normal-mapped normal.
/// @param outgoingDirection Unit direction from the surface towards the viewer.
/// @param outNormal The clipped unit normal; unchanged on failure.
/// @return Success; NonFiniteInput for a nonfinite input. Unit length is a precondition that is not detected; inputs
/// that violate it so far that the clipped vector is zero return Degenerate.
/// @note Pure function with no allocation or thread affinity.
[[nodiscard]] MathStatus
TryClipShadingNormalToOutgoingHemisphere(Vector3 normal, Vector3 outgoingDirection, Vector3& outNormal) noexcept;
} // namespace ludus::foundation::math
