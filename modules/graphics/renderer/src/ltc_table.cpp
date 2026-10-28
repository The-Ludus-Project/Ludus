// Lookup of the embedded GGX LTC table; see ltc_table.hpp for the Heitz et al. 2016
// and Peters 2021 references and the table layout.

#include "internal/ltc_table_data.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/ltc_table.hpp>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
// The reference BSDF admits perceptual roughness from 0.045; the table rows below it repeat that fit.
constexpr float32 MINIMUM_ROUGHNESS = 0.045F;
// Rec. 709 luminance weights select the F0 slice for a colored F0.
constexpr math::Vector3 LUMINANCE_WEIGHTS{0.2126F, 0.7152F, 0.0722F};
// Unit-length tolerance on squared length, matching the reference BSDF's direction contract.
constexpr float32 UNIT_TOLERANCE = 1.0e-5F;
// Below this tangent length the view is parallel to the normal and any tangent is valid.
constexpr float32 PARALLEL_TANGENT_LENGTH = 1.0e-6F;

/// A sample coordinate on a uniform axis: the lower index and the interpolation weight toward the next one.
struct AxisCoordinate final
{
    usize Index = 0;
    float32 Weight = 0.0F;
};

[[nodiscard]] AxisCoordinate MakeCoordinate(float32 value, usize count) noexcept
{
    const float32 position = math::Clamp(value, 0.0F, 1.0F) * static_cast<float32>(count - 1);
    const usize index = math::Min(static_cast<usize>(position), count - 2);
    return AxisCoordinate{ .Index = index, .Weight = position - static_cast<float32>(index) };
}

[[nodiscard]] bool IsUnit(math::Vector3 value) noexcept
{
    const float32 lengthSquared = math::Dot(value, value);
    return math::IsFinite(value) == true && math::Abs(lengthSquared - 1.0F) <= UNIT_TOLERANCE;
}

[[nodiscard]] LtcTableEntry Blend(LtcTableEntry a, LtcTableEntry b, float32 t) noexcept
{
    return LtcTableEntry
    {
        .InverseM00 = math::Lerp(a.InverseM00, b.InverseM00, t),
        .InverseM02 = math::Lerp(a.InverseM02, b.InverseM02, t),
        .InverseM11 = math::Lerp(a.InverseM11, b.InverseM11, t),
        .InverseM20 = math::Lerp(a.InverseM20, b.InverseM20, t),
    };
}

[[nodiscard]] LtcAlbedoEntry Blend(LtcAlbedoEntry a, LtcAlbedoEntry b, float32 t) noexcept
{
    return LtcAlbedoEntry
    {
        .FresnelScale = math::Lerp(a.FresnelScale, b.FresnelScale, t),
        .FresnelBias = math::Lerp(a.FresnelBias, b.FresnelBias, t),
    };
}

[[nodiscard]] usize GetTransformIndex(usize roughness, usize inclination, usize fresnel) noexcept
{
    return (fresnel * LTC_TABLE_INCLINATION_COUNT + inclination) * LTC_TABLE_ROUGHNESS_COUNT + roughness;
}

/// Bilinear interpolation in roughness and inclination within one F0 slice.
[[nodiscard]] LtcTableEntry SampleSlice(AxisCoordinate roughness, AxisCoordinate inclination, usize fresnel) noexcept
{
    const LtcTableEntry* transforms = internal::LTC_TABLE_TRANSFORMS;
    const LtcTableEntry low = Blend(transforms[GetTransformIndex(roughness.Index, inclination.Index, fresnel)],
                                    transforms[GetTransformIndex(roughness.Index + 1, inclination.Index, fresnel)],
                                    roughness.Weight);
    const LtcTableEntry high = Blend(transforms[GetTransformIndex(roughness.Index, inclination.Index + 1, fresnel)],
                                     transforms[GetTransformIndex(roughness.Index + 1, inclination.Index + 1, fresnel)],
                                     roughness.Weight);
    return Blend(low, high, inclination.Weight);
}
} // namespace

BorrowedView<const LtcTableEntry> GetLtcTableTransforms() noexcept
{
    return {internal::LTC_TABLE_TRANSFORMS, LTC_TABLE_TRANSFORM_COUNT};
}

BorrowedView<const LtcAlbedoEntry> GetLtcTableAlbedo() noexcept
{
    return {internal::LTC_TABLE_ALBEDO, LTC_TABLE_ALBEDO_COUNT};
}

rhi::RasterStatus
TryMakeLtcTangentFrame(math::Vector3 normalWorld, math::Vector3 viewWorld, math::Matrix3& outWorldToTangent) noexcept
{
    if (IsUnit(normalWorld) == false || IsUnit(viewWorld) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::Vector3 tangent = viewWorld - normalWorld * math::Dot(normalWorld, viewWorld);
    if (math::Length(tangent) <= PARALLEL_TANGENT_LENGTH)
    {
        const math::Vector3 helper = math::Abs(normalWorld.X) < 0.5F ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
        tangent = math::Cross(helper, normalWorld);
    }
    static_cast<void>(math::TryNormalize(tangent, tangent));
    const math::Vector3 bitangent = math::Cross(normalWorld, tangent);
    // Rows tangent, bitangent, normal: the transpose of the matrix with those columns.
    outWorldToTangent = math::Transpose(math::Matrix3{{tangent, bitangent, normalWorld}});
    return rhi::RasterStatus::Ready;
}

rhi::RasterStatus TryLookupSpecularLtc(float32 perceptualRoughness,
                                       float32 viewCosine,
                                       math::Vector3 specularF0,
                                       SpecularLtc& outLtc) noexcept
{
    if (math::IsFinite(perceptualRoughness) == false || perceptualRoughness < 0.0F || perceptualRoughness > 1.0F ||
        math::IsFinite(viewCosine) == false || viewCosine < 0.0F || viewCosine > 1.0F ||
        math::IsFinite(specularF0) == false || specularF0.X < 0.0F || specularF0.X > 1.0F || specularF0.Y < 0.0F ||
        specularF0.Y > 1.0F || specularF0.Z < 0.0F || specularF0.Z > 1.0F)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const AxisCoordinate roughness =
        MakeCoordinate(math::Max(perceptualRoughness, MINIMUM_ROUGHNESS), LTC_TABLE_ROUGHNESS_COUNT);
    const float32 inclination = math::Atan2(math::Sqrt(math::Max(0.0F, 1.0F - viewCosine * viewCosine)), viewCosine);
    const AxisCoordinate inclinationAxis = MakeCoordinate(inclination / math::kHalfPiF, LTC_TABLE_INCLINATION_COUNT);
    const AxisCoordinate fresnel = MakeCoordinate(math::Dot(specularF0, LUMINANCE_WEIGHTS), LTC_TABLE_FRESNEL_COUNT);
    const LtcTableEntry entry = Blend(SampleSlice(roughness, inclinationAxis, fresnel.Index),
                                      SampleSlice(roughness, inclinationAxis, fresnel.Index + 1),
                                      fresnel.Weight);
    const LtcAlbedoEntry* albedoTable = internal::LTC_TABLE_ALBEDO;
    const usize albedoBase = inclinationAxis.Index * LTC_TABLE_ROUGHNESS_COUNT + roughness.Index;
    const LtcAlbedoEntry albedo = Blend(Blend(albedoTable[albedoBase], albedoTable[albedoBase + 1], roughness.Weight),
                                        Blend(albedoTable[albedoBase + LTC_TABLE_ROUGHNESS_COUNT],
                                              albedoTable[albedoBase + LTC_TABLE_ROUGHNESS_COUNT + 1],
                                              roughness.Weight),
                                        inclinationAxis.Weight);
    SpecularLtc result;
    result.InverseTransform = math::Matrix3{{
        math::Vector3{entry.InverseM00, 0.0F, entry.InverseM20},
        math::Vector3{0.0F, entry.InverseM11, 0.0F},
        math::Vector3{entry.InverseM02, 0.0F, 1.0F},
    }};
    result.Albedo = specularF0 * albedo.FresnelScale + math::Vector3{1.0F, 1.0F, 1.0F} * albedo.FresnelBias;
    outLtc = result;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer
