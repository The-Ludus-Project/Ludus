#include <ludus/graphics/renderer/hair_card.hpp>

#include <ludus/foundation/math/scalar.hpp>

// Thanks to James T. Kajiya and Timothy L. Kay, "Rendering Fur with Three Dimensional Textures", SIGGRAPH '89,
// pp. 271-280, "The specular component", for the tangent-cone highlight; see hair_card.hpp for Ludus's departures.
// renderer_lit.slang (EvaluateHairCard) mirrors every operation here.
namespace ludus::graphics::renderer
{
namespace math = ludus::foundation::math;
using ludus::foundation::float32;
using ludus::foundation::float64;
namespace
{
/// Squared length below which a direction is treated as vanishing.
constexpr float32 MIN_DIRECTION_LENGTH_SQUARED = 1e-12F;
/// Sine between the normal and +Y below which the fallback tangent uses +Z instead.
constexpr float32 MIN_TANGENT_SINE = 1e-3F;
constexpr float32 DEGREE_TO_RADIAN = math::kPiF / 180.0F; // ludus-style: magic-number degrees in a half turn

[[nodiscard]] math::Vector3 NormalizeOrZero(math::Vector3 value) noexcept
{
    const float32 lengthSquared = math::Dot(value, value);
    if ((lengthSquared > MIN_DIRECTION_LENGTH_SQUARED) == false)
    {
        return {};
    }
    return value * (1.0F / math::Sqrt(lengthSquared));
}

/// Width of the sine lobe shared by both highlights.
struct HighlightShape final
{
    float32 Exponent = MIN_HAIR_CARD_EXPONENT;
    float32 Normalization = 0;
};

[[nodiscard]] float32 EvaluateHighlight(const HairCardDirections& directions,
                                        math::Vector3 halfDirectionWorld,
                                        float32 shiftRadians,
                                        HighlightShape shape) noexcept
{
    const math::Vector3 shiftedStrandWorld = NormalizeOrZero(directions.StrandWorld * math::Cos(shiftRadians) +
                                                             directions.FacingNormalWorld * math::Sin(shiftRadians));
    const float32 cosine = math::Dot(shiftedStrandWorld, halfDirectionWorld);
    const float32 sine = math::Sqrt(math::Max(0.0F, 1.0F - cosine * cosine));
    return shape.Normalization *
           static_cast<float32>(math::Pow(static_cast<float64>(sine), static_cast<float64>(shape.Exponent)));
}
} // namespace

HairCardShading MakeHairCardShading(const content::scene_format::HairCardParameters& parameters) noexcept
{
    return
    {
        .StrandRadians = parameters.StrandDirectionDegrees * DEGREE_TO_RADIAN,
        .PrimaryShiftRadians = parameters.PrimaryShiftDegrees * DEGREE_TO_RADIAN,
        .SecondaryShiftRadians = parameters.SecondaryShiftDegrees * DEGREE_TO_RADIAN,
        .PrimaryWeight = parameters.PrimaryWeight,
        .SecondaryWeight = parameters.SecondaryWeight,
    };
}

float32 ComputeHairCardExponent(float32 roughness) noexcept
{
    const float32 alpha = roughness * roughness;
    const float32 alphaSquared = alpha * alpha;
    if (math::IsFinite(alphaSquared) == false || alphaSquared <= 0)
    {
        return MIN_HAIR_CARD_EXPONENT;
    }
    return math::Clamp(2 / alphaSquared - 2, MIN_HAIR_CARD_EXPONENT, MAX_HAIR_CARD_EXPONENT);
}

math::Vector3 ComputeHairCardFallbackTangent(math::Vector3 normalWorld) noexcept
{
    const math::Vector3 horizontal = math::Cross(math::Vector3{0, 1, 0}, normalWorld);
    if (math::Dot(horizontal, horizontal) > MIN_TANGENT_SINE * MIN_TANGENT_SINE)
    {
        return NormalizeOrZero(horizontal);
    }
    return NormalizeOrZero(math::Cross(normalWorld, math::Vector3{0, 0, 1}));
}

math::Vector3
ComputeHairCardStrand(math::Vector3 normalWorld, math::Vector4 tangentWorld, float32 strandRadians) noexcept
{
    const math::Vector3 tangentDirectionWorld{tangentWorld.X, tangentWorld.Y, tangentWorld.Z};
    math::Vector3 planarTangentWorld =
        NormalizeOrZero(tangentDirectionWorld - normalWorld * math::Dot(normalWorld, tangentDirectionWorld));
    float32 sign = tangentWorld.W < 0 ? -1.0F : 1.0F;
    if (math::Dot(planarTangentWorld, planarTangentWorld) == 0)
    {
        planarTangentWorld = ComputeHairCardFallbackTangent(normalWorld);
        sign = 1;
    }
    const math::Vector3 bitangentWorld = math::Cross(normalWorld, planarTangentWorld) * sign;
    return NormalizeOrZero(planarTangentWorld * math::Cos(strandRadians) + bitangentWorld * math::Sin(strandRadians));
}

math::Vector3 EvaluateHairCardReflectance(math::Vector3 baseColor,
                                          float32 roughness,
                                          const HairCardShading& shading,
                                          const HairCardDirections& directions) noexcept
{
    const float32 nl = math::Dot(directions.FacingNormalWorld, directions.LightDirectionWorld);
    const float32 nv = math::Dot(directions.FacingNormalWorld, directions.ViewDirectionWorld);
    if ((nl > 0) == false || (nv > 0) == false)
    {
        return {};
    }
    math::Vector3 result = baseColor * (1.0F / math::kPiF);
    const math::Vector3 halfDirectionWorld =
        NormalizeOrZero(directions.ViewDirectionWorld + directions.LightDirectionWorld);
    if (math::Dot(halfDirectionWorld, halfDirectionWorld) > 0)
    {
        HighlightShape shape;
        shape.Exponent = ComputeHairCardExponent(roughness);
        shape.Normalization = (shape.Exponent + 2) / (2 * math::kPiF);
        const float32 primary = shading.PrimaryWeight *
                                EvaluateHighlight(directions, halfDirectionWorld, shading.PrimaryShiftRadians, shape);
        const float32 secondary =
            shading.SecondaryWeight *
            EvaluateHighlight(directions, halfDirectionWorld, shading.SecondaryShiftRadians, shape);
        result = result + math::Vector3{primary, primary, primary} + baseColor * secondary;
    }
    return result * nl;
}
} // namespace ludus::graphics::renderer
