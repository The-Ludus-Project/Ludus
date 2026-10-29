#include <ludus/foundation/math/shading_normal.hpp>

// Peters 2021, section 6.1, implemented from the paper's text; the construction and the margin are Ludus's. The shader
// port in shaders/shading_normal.slang performs the same operations.
namespace ludus::foundation::math
{
MathStatus
TryClipShadingNormalToOutgoingHemisphere(Vector3 normal, Vector3 outgoingDirection, Vector3& outNormal) noexcept
{
    if (IsFinite(normal) == false || IsFinite(outgoingDirection) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    const float32 cosine = Dot(normal, outgoingDirection);
    if (cosine >= SHADING_NORMAL_MINIMUM_OUTGOING_COSINE)
    {
        outNormal = normal;
        return MathStatus::Success;
    }
    // |n + (m - c) v|^2 = 1 + m^2 - c^2 >= m^2 for unit n and v and c in [-1, m), so the length is never zero.
    return TryNormalize(normal + outgoingDirection * (SHADING_NORMAL_MINIMUM_OUTGOING_COSINE - cosine), outNormal);
}
} // namespace ludus::foundation::math
