#pragma once

// Shading normals clipped to the hemisphere of the outgoing direction; the GPU port of FoundationMath's
// shading_normal.hpp with the same operations. Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal
// Lights", ACM Transactions on Graphics 40(4), Article 140, 2021, section 6.1, https://doi.org/10.1145/3450626.3459672:
// "Shading normals get clipped to the hemisphere of the outgoing light direction to avoid black pixels due to normal
// mapping." Implemented from the paper's text; the construction and the margin are Ludus's (FoundationMath module page,
// "Shading normal clipping").

// SHADING_NORMAL_MINIMUM_OUTGOING_COSINE of the C++ header: section 6.1 names no margin, and the standard BSDF is zero
// for an outgoing direction on the shading horizon.
static const float SHADING_NORMAL_MINIMUM_OUTGOING_COSINE = 0.01;

/// normalWorld and outgoingDirectionWorld are unit vectors. A normal whose cosine with outgoingDirectionWorld is at
/// least SHADING_NORMAL_MINIMUM_OUTGOING_COSINE is returned unchanged. Otherwise it moves along outgoingDirectionWorld
/// until that cosine is SHADING_NORMAL_MINIMUM_OUTGOING_COSINE and is renormalized, which keeps the result continuous
/// in both inputs; the clipped cosine is then at least SHADING_NORMAL_MINIMUM_OUTGOING_COSINE / sqrt(1 +
/// SHADING_NORMAL_MINIMUM_OUTGOING_COSINE^2), and a normal facing directly away becomes outgoingDirectionWorld. Callers
/// shade with the result everywhere: the shading frame, the light sampling techniques and the BSDF.
float3 ClipShadingNormalToOutgoingHemisphere(float3 normalWorld, float3 outgoingDirectionWorld)
{
    float cosine = dot(normalWorld, outgoingDirectionWorld);
    if (cosine >= SHADING_NORMAL_MINIMUM_OUTGOING_COSINE)
        return normalWorld;
    // |n + (m - c) v|^2 = 1 + m^2 - c^2 >= m^2 for unit n and v and c in [-1, m), so the length is never zero.
    return normalize(normalWorld + (SHADING_NORMAL_MINIMUM_OUTGOING_COSINE - cosine) * outgoingDirectionWorld);
}
