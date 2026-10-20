#include <ludus/graphics/renderer/lighting.hpp>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/summation.hpp>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
rhi::RasterStatus ResolveMeshEmissionPower(math::Vector3 color,
                                           float32 lumens,
                                           float64 areaWorld,
                                           bool isTwoSided,
                                           math::Vector3& outEmission) noexcept
{
    constexpr math::Vector3 LUMINANCE_WEIGHTS{.2126F, .7152F, .0722F};
    constexpr float32 MAX_EMISSION = 10000;
    if (math::IsFinite(color) == false || color.X < 0 || color.Y < 0 || color.Z < 0 ||
        math::IsFinite(lumens) == false || lumens < 0 || math::IsFinite(areaWorld) == false || areaWorld <= 0)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const float32 luminance = math::Dot(color, LUMINANCE_WEIGHTS);
    if (luminance <= 0)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const float64 divisor = static_cast<float64>(luminance) * math::kPi * areaWorld * (isTwoSided == true ? 2.0 : 1.0);
    const math::Vector3 value = color * static_cast<float32>(static_cast<float64>(lumens) / divisor);
    if (math::IsFinite(value) == false || value.X > MAX_EMISSION || value.Y > MAX_EMISSION || value.Z > MAX_EMISSION)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    outEmission = value;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus DecodeSurfaceParameters(math::Vector4 texel,
                                          float32 metallic,
                                          float32 roughness,
                                          float32 normalScale,
                                          SurfaceParameters& outParameters) noexcept
{
    if (math::IsFinite(texel) == false || texel.X < 0 || texel.X > 1 || texel.Y < 0 || texel.Y > 1 || texel.Z < 0 ||
        texel.Z > 1 || texel.W < 0 || texel.W > 1 || math::IsFinite(metallic) == false || metallic < 0 ||
        metallic > 1 || math::IsFinite(roughness) == false || roughness < 0.045F || roughness > 1 ||
        math::IsFinite(normalScale) == false || normalScale < 0 || normalScale > 8)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::Vector2 xy{texel.X * 2 - 1, texel.Y * 2 - 1};
    const float32 square = math::Dot(xy, xy);
    if (square > 1)
    {
        xy = xy / math::Sqrt(square);
    }
    math::Vector3 normalTangent{xy.X * normalScale, xy.Y * normalScale, math::Sqrt(math::Max(0.0F, 1 - square))};
    if (normalScale == 0)
    {
        normalTangent = {0, 0, 1};
    }
    SurfaceParameters parameters;
    static_cast<void>(math::TryNormalize(normalTangent, parameters.NormalTangent));
    parameters.Metallic = metallic * texel.W;
    parameters.Roughness = math::Max(0.045F, roughness * texel.Z);
    outParameters = parameters;
    return rhi::RasterStatus::Ready;
}
namespace
{
bool Bounded(math::Vector3 value, float32 minimum, float32 maximum) noexcept
{
    return math::IsFinite(value) == true && value.X >= minimum && value.X <= maximum && value.Y >= minimum &&
           value.Y <= maximum && value.Z >= minimum && value.Z <= maximum;
}
float32 Fifth(float32 value) noexcept
{
    const auto square = value * value;
    return square * square * value;
}
bool NormalizeLight(DirectLight& inoutLight) noexcept
{
    DirectLight& light = inoutLight;
    return light.SourceId != 0 && static_cast<uint8>(light.Kind) <= static_cast<uint8>(LightKind::AreaTwoSided) &&
           Bounded(light.Color, 0, 1) == true && math::IsFinite(light.Intensity) == true && light.Intensity >= 0 &&
           light.Intensity <= 1000000 && Bounded(light.Position, -1000000, 1000000) == true &&
           math::IsFinite(light.Direction) == true && math::IsFinite(light.Range) == true && light.Range >= 0.01F &&
           light.Range <= 1000000 && math::IsFinite(light.InnerCos) == true && math::IsFinite(light.OuterCos) == true &&
           light.OuterCos > -1 && light.OuterCos < light.InnerCos && light.InnerCos <= 1 &&
           (light.Kind == LightKind::Point ||
            math::TryNormalize(light.Direction, light.Direction) == math::MathStatus::Success);
}
} // namespace
rhi::RasterStatus BuildTriangleAreaLights(const TriangleAreaLightDescription& description,
                                          AreaLightSamples& outSamples) noexcept
{
    const uint32 sampleCount = description.Quadrature == AreaLightQuadrature::ThreePoint ? 3U : 1U;
    if (description.FirstSampleSourceId == 0 || description.FirstSampleSourceId > ~uint64{0} - (sampleCount - 1) ||
        Bounded(description.Emission, 0, 10000) == false || math::IsFinite(description.Range) == false ||
        description.Range < .01F || description.Range > 1000000 ||
        static_cast<uint8>(description.Quadrature) > static_cast<uint8>(AreaLightQuadrature::ThreePoint))
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    for (const math::Vector3& positionWorld : description.PositionsWorld)
    {
        if (Bounded(positionWorld, -1000000, 1000000) == false)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
    }
    const math::Vector3 cross = math::Cross(description.PositionsWorld[1] - description.PositionsWorld[0],
                                            description.PositionsWorld[2] - description.PositionsWorld[0]);
    const float32 area = math::Length(cross) * .5F;
    math::Vector3 normalWorld;
    if (math::IsFinite(area) == false || area <= 0 ||
        math::TryNormalize(cross, normalWorld) != math::MathStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    // Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, 4th ed.,
    // section 12.5 Area Lights, https://pbr-book.org/4ed/Light_Sources/Area_Lights.
    // Adopt Lambertian Le*dA*cos(theta) transport and geometric sidedness.
    // This original bounded centroid/three-point raster quadrature replaces
    // stochastic continuous area sampling and approximates finite-source visibility.
    AreaLightSamples samples;
    samples.Count = sampleCount;
    const float32 maximum =
        math::Max(description.Emission.X, math::Max(description.Emission.Y, description.Emission.Z));
    const float32 intensity = maximum * area / float32(samples.Count);
    if (math::IsFinite(intensity) == false || intensity > 1000000)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    for (usize i = 0; i < samples.Count; ++i)
    {
        DirectLight& light = samples.Lights[i];
        light.SourceId = description.FirstSampleSourceId + i;
        light.Kind = description.IsTwoSided == true ? LightKind::AreaTwoSided : LightKind::AreaOneSided;
        light.Color = maximum > 0 ? description.Emission / maximum : math::Vector3{};
        light.Intensity = intensity;
        light.Direction = normalWorld;
        light.Range = description.Range;
        if (samples.Count == 1)
        {
            light.Position = description.PositionsWorld[0] / 3.F + description.PositionsWorld[1] / 3.F +
                             description.PositionsWorld[2] / 3.F;
        }
        else
        {
            light.Position = description.PositionsWorld[i] * (2.F / 3.F) +
                             description.PositionsWorld[(i + 1) % AREA_TRIANGLE_CORNERS] * (1.F / 6.F) +
                             description.PositionsWorld[(i + 2) % AREA_TRIANGLE_CORNERS] * (1.F / 6.F);
        }
    }
    outSamples = samples;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus BuildPlanarAreaLight(const PlanarAreaLightDescription& description, DirectLight& outLight) noexcept
{
    if (description.PositionsMesh.IsValid() == false || description.Indices.IsValid() == false ||
        description.PositionsMesh.IsEmpty() == true || description.Indices.IsEmpty() == true ||
        description.Indices.GetSize() > content::scene_format::MAX_INDICES ||
        description.Indices.GetSize() % AREA_TRIANGLE_CORNERS != 0 || description.SourceId == 0 ||
        Bounded(description.Emission, 0, 10000) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::Matrix3 normalMeshToWorld;
    if (math::TryNormalMatrix(description.MeshToWorld, {}, normalMeshToWorld) != math::MathStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::CompensatedSum totalArea;
    StaticArray<math::CompensatedSum, AREA_TRIANGLE_CORNERS> weightedPosition;
    math::Vector3 normalWorld;
    math::Vector3 originWorld;
    float32 planeTolerance = .0001F; // Metres: 0.1 mm plus affine FP32 coordinate allowance.
    for (usize triangle = 0; triangle < description.Indices.GetSize() / AREA_TRIANGLE_CORNERS; ++triangle)
    {
        StaticArray<math::Vector3, AREA_TRIANGLE_CORNERS> positionMesh;
        StaticArray<math::Vector3, AREA_TRIANGLE_CORNERS> positionWorld;
        for (usize corner = 0; corner < AREA_TRIANGLE_CORNERS; ++corner)
        {
            const uint32 vertex = description.Indices[triangle * AREA_TRIANGLE_CORNERS + corner];
            if (vertex >= description.PositionsMesh.GetSize())
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            positionMesh[corner] = description.PositionsMesh[vertex];
            positionWorld[corner] = math::TransformPoint(description.MeshToWorld, positionMesh[corner]);
            if (Bounded(positionWorld[corner], -1000000, 1000000) == false)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            planeTolerance = math::Max(planeTolerance, math::Length(positionWorld[corner]) * 2e-6F);
        }
        math::Vector3 triangleNormalWorld;
        const math::Vector3 normalMesh =
            math::Cross(positionMesh[1] - positionMesh[0], positionMesh[2] - positionMesh[0]);
        const float32 area =
            math::Length(math::Cross(positionWorld[1] - positionWorld[0], positionWorld[2] - positionWorld[0])) * .5F;
        if (math::IsFinite(area) == false || area <= 0 ||
            math::TryNormalize(normalMeshToWorld * normalMesh, triangleNormalWorld) != math::MathStatus::Success)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        if (triangle == 0)
        {
            normalWorld = triangleNormalWorld;
            originWorld = positionWorld[0];
        }
        if (math::Dot(normalWorld, triangleNormalWorld) < 1 - 1e-5F)
        {
            return rhi::RasterStatus::Unsupported;
        }
        for (const math::Vector3& position : positionWorld)
        {
            if (math::Abs(math::Dot(position - originWorld, normalWorld)) > planeTolerance)
            {
                return rhi::RasterStatus::Unsupported;
            }
        }
        totalArea.Add(float64(area));
        for (const math::Vector3& position : positionWorld)
        {
            weightedPosition[0].Add(float64(position.X) * float64(area) / 3);
            weightedPosition[1].Add(float64(position.Y) * float64(area) / 3);
            weightedPosition[2].Add(float64(position.Z) * float64(area) / 3);
        }
    }
    const float64 area = totalArea.GetResult();
    const math::Vector3 centroidWorld{float32(weightedPosition[0].GetResult() / area),
                                      float32(weightedPosition[1].GetResult() / area),
                                      float32(weightedPosition[2].GetResult() / area)};
    bool isCentroidOnPatch = false;
    for (usize triangle = 0; triangle < description.Indices.GetSize() / AREA_TRIANGLE_CORNERS; ++triangle)
    {
        const math::Vector3 a =
            math::TransformPoint(description.MeshToWorld,
                                 description.PositionsMesh[description.Indices[triangle * AREA_TRIANGLE_CORNERS]]);
        const math::Vector3 b =
            math::TransformPoint(description.MeshToWorld,
                                 description.PositionsMesh[description.Indices[triangle * AREA_TRIANGLE_CORNERS + 1]]);
        const math::Vector3 c =
            math::TransformPoint(description.MeshToWorld,
                                 description.PositionsMesh[description.Indices[triangle * AREA_TRIANGLE_CORNERS + 2]]);
        const float32 sign = math::Dot(math::Cross(b - a, c - a), normalWorld) >= 0 ? 1.F : -1.F;
        if (math::Dot(math::Cross(b - a, centroidWorld - a), normalWorld) * sign / math::Length(b - a) >=
                -planeTolerance &&
            math::Dot(math::Cross(c - b, centroidWorld - b), normalWorld) * sign / math::Length(c - b) >=
                -planeTolerance &&
            math::Dot(math::Cross(a - c, centroidWorld - c), normalWorld) * sign / math::Length(a - c) >=
                -planeTolerance)
        {
            isCentroidOnPatch = true;
            break;
        }
    }
    if (isCentroidOnPatch == false)
    {
        return rhi::RasterStatus::Unsupported;
    }
    const float32 maximum =
        math::Max(description.Emission.X, math::Max(description.Emission.Y, description.Emission.Z));
    const float64 intensity = float64(maximum) * area;
    if (math::IsFinite(intensity) == false || intensity > 1000000)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    DirectLight light;
    light.SourceId = description.SourceId;
    light.Kind = description.IsTwoSided == true ? LightKind::AreaTwoSided : LightKind::AreaOneSided;
    light.Color = maximum > 0 ? description.Emission / maximum : math::Vector3{};
    light.Intensity = float32(intensity);
    light.Position = centroidWorld;
    light.Direction = normalWorld;
    light.Range = description.Range;
    if (NormalizeLight(light) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    outLight = light;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus BuildPointShadowMatrix(math::Vector3 positionWorld,
                                         uint32 face,
                                         float32 nearDistance,
                                         float32 farDistance,
                                         math::Matrix4& outWorldToClip) noexcept
{
    if (face >= POINT_SHADOW_FACES || Bounded(positionWorld, -1000000, 1000000) == false ||
        math::IsFinite(nearDistance) == false || math::IsFinite(farDistance) == false || nearDistance <= 0 ||
        farDistance <= nearDistance || farDistance > 1000000)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::Vector3 directionWorld;
    math::Vector3 upWorld{0, 1, 0};
    switch (face)
    {
        case 0:
            directionWorld = {1, 0, 0};
            break;
        case 1:
            directionWorld = {-1, 0, 0};
            break;
        case 2:
            directionWorld = {0, 1, 0};
            upWorld = {0, 0, -1};
            break;
        case 3:
            directionWorld = {0, -1, 0};
            upWorld = {0, 0, 1};
            break;
        case 4:
            directionWorld = {0, 0, 1};
            break;
        default:
            directionWorld = {0, 0, -1};
            break;
    }
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    if (math::TryLookAt(positionWorld, positionWorld + directionWorld, upWorld, worldToView) !=
            math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(math::kPiF * .5F, 1, nearDistance, farDistance, viewToClip) !=
            math::MathStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    for (math::Vector4& column : viewToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    const math::Matrix4 worldToClip = viewToClip * worldToView;
    if (math::IsFinite(worldToClip) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    outWorldToClip = worldToClip;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus NormalizeDirectLights(const DirectLight* lights, usize count, LightSelection& output) noexcept
{
    if (count > 128)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    if (count != 0 && lights == nullptr)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    LightSelection result;
    for (usize i = 0; i < count; ++i)
    {
        auto light = lights[i];
        if (NormalizeLight(light) == false)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        usize at = 0;
        while (at < result.Count && result.Lights[at].SourceId < light.SourceId)
        {
            ++at;
        }
        if (at < result.Count && result.Lights[at].SourceId == light.SourceId)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        for (usize j = result.Count; j > at; --j)
        {
            result.Lights[j] = result.Lights[j - 1];
        }
        result.Lights[at] = light;
        ++result.Count;
    }
    output = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus SelectDirectLights(const DirectLight* lights, usize count, LightSelection& output) noexcept
{
    if (count > 128)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    if (count != 0 && lights == nullptr)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    LightSelection result;
    for (usize i = 0; i < count; ++i)
    {
        auto light = lights[i];
        if (NormalizeLight(light) == false)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (lights[j].SourceId == light.SourceId)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
        }
        usize at = 0;
        while (at < result.Count && result.Lights[at].SourceId < light.SourceId)
        {
            ++at;
        }
        if (at < 8)
        {
            if (result.Count < 8)
            {
                ++result.Count;
            }
            for (usize j = result.Count - 1; j > at; --j)
            {
                result.Lights[j] = result.Lights[j - 1];
            }
            result.Lights[at] = light;
        }
    }
    result.Dropped = static_cast<uint32>(count) - result.Count;
    output = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus BuildLightClusters(const LightClusterDescription& description,
                                     const DirectLight* lights,
                                     usize count,
                                     LightClusters& output) noexcept
{
    if (count > 128)
    {
        return rhi::RasterStatus::CapacityExceeded;
    }
    if ((count != 0 && lights == nullptr) || math::IsFinite(description.WorldToGrid) == false ||
        math::IsFinite(description.Bounds) == false || math::IsEmpty(description.Bounds))
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const auto extent = description.Bounds.Max - description.Bounds.Min;
    if (Bounded(extent, 0.01F, 2000000) == false || Bounded(description.Bounds.Min, -1000000, 1000000) == false ||
        Bounded(description.Bounds.Max, -1000000, 1000000) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const auto& columns = description.WorldToGrid.Columns;
    for (usize i = 0; i < 3; ++i)
    {
        for (usize j = 0; j < 3; ++j)
        {
            if (math::Abs(math::Dot(columns[i], columns[j]) - (i == j ? 1.0F : 0.0F)) > 1e-5F)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
        }
    }
    LightClusters result;
    for (usize i = 0; i < count; ++i)
    {
        auto validated = lights[i];
        if (NormalizeLight(validated) == false)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        usize at = 0;
        while (at < result.Count && result.Lights[at].SourceId < lights[i].SourceId)
        {
            ++at;
        }
        if (at < result.Count && result.Lights[at].SourceId == lights[i].SourceId)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        for (usize j = result.Count; j > at; --j)
        {
            result.Lights[j] = result.Lights[j - 1];
        }
        result.Lights[at] = validated;
        ++result.Count;
    }
    // Thanks to Olsson, Billeter and Assarsson, "Clustered Deferred and Forward Shading",
    // High Performance Graphics 2012, pp. 87-96, doi:10.2312/EGGH/HPG12/087-096.
    // Adopt spatial grouping from the authors' abstract; this portable CPU grid uses
    // linear cells and conservative range spheres, without depth/normal pruning.
    // See docs/architecture/renderer-systems.md for capacity and overflow departures.
    for (usize cell = 0; cell < 512; ++cell)
    {
        const math::Vector3 coordinate{static_cast<float32>(cell % 8),
                                       static_cast<float32>((cell / 8) % 8),
                                       static_cast<float32>((cell / 64) % 8)};
        const auto minimum =
            description.Bounds.Min +
            math::Vector3{extent.X * coordinate.X / 8, extent.Y * coordinate.Y / 8, extent.Z * coordinate.Z / 8};
        const auto maximum = minimum + extent / 8.0F;
        for (usize i = 0; i < count; ++i)
        {
            const auto& light = result.Lights[i];
            bool intersects = light.Kind == LightKind::Directional;
            if (intersects == false)
            {
                const auto center = math::TransformPoint(description.WorldToGrid, light.Position);
                if (math::IsFinite(center) == false)
                {
                    return rhi::RasterStatus::InvalidDescription;
                }
                const auto dx = math::Max(math::Max(minimum.X - center.X, center.X - maximum.X), 0.0F);
                const auto dy = math::Max(math::Max(minimum.Y - center.Y, center.Y - maximum.Y), 0.0F);
                const auto dz = math::Max(math::Max(minimum.Z - center.Z, center.Z - maximum.Z), 0.0F);
                // A conservative rounding margin protects tangencies and CPU/GPU cell boundaries.
                const auto magnitude =
                    math::Max(math::Max(math::Abs(center.X), math::Abs(center.Y)), math::Abs(center.Z));
                const auto translation = description.WorldToGrid.Translation;
                const auto shift =
                    math::Max(math::Max(math::Abs(translation.X), math::Abs(translation.Y)), math::Abs(translation.Z));
                const auto gridMagnitude = math::Max(
                    math::Max(math::Max(math::Abs(description.Bounds.Min.X), math::Abs(description.Bounds.Max.X)),
                              math::Max(math::Abs(description.Bounds.Min.Y), math::Abs(description.Bounds.Max.Y))),
                    math::Max(math::Abs(description.Bounds.Min.Z), math::Abs(description.Bounds.Max.Z)));
                const float64 radius = static_cast<float64>(light.Range) * 1.00002 + 0.001 +
                                       (static_cast<float64>(magnitude) + static_cast<float64>(shift) +
                                        static_cast<float64>(gridMagnitude)) *
                                           0.000004;
                intersects = static_cast<float64>(dx) * static_cast<float64>(dx) +
                                 static_cast<float64>(dy) * static_cast<float64>(dy) +
                                 static_cast<float64>(dz) * static_cast<float64>(dz) <=
                             radius * radius;
            }
            if (intersects == false)
            {
                continue;
            }
            auto& written = result.Counts[cell];
            if (written < 32)
            {
                result.Texels[cell * 32 + written] = static_cast<uint8>(i + 1);
                ++written;
                ++result.Assignments;
            }
            else if (result.Overflow[cell] == false)
            {
                result.Overflow[cell] = true;
                ++result.OverflowCells;
            }
        }
        if (result.Overflow[cell])
        {
            result.Texels[cell * 32] = 255;
        }
    }
    output = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus EvaluateLitBrdf(math::Vector3 baseColor,
                                  float32 metallic,
                                  float32 roughness,
                                  math::Vector3 normal,
                                  math::Vector3 view,
                                  math::Vector3 light,
                                  math::Vector3& output) noexcept
{
    if (Bounded(baseColor, 0, 1) == false || math::IsFinite(metallic) == false || metallic < 0 || metallic > 1 ||
        math::IsFinite(roughness) == false || roughness < 0.045F || roughness > 1 ||
        math::TryNormalize(normal, normal) != math::MathStatus::Success ||
        math::TryNormalize(view, view) != math::MathStatus::Success ||
        math::TryNormalize(light, light) != math::MathStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::Vector3 result;
    const auto nv = math::Max(math::Dot(normal, view), 0.0F);
    const auto nl = math::Max(math::Dot(normal, light), 0.0F);
    math::Vector3 half;
    if (nv > 0 && nl > 0 && math::TryNormalize(view + light, half) == math::MathStatus::Success)
    {
        // Thanks to Google/Filament authors, "Physically Based Rendering in Filament",
        // sections 4.4.1-4.4.4 and 5.2.2, https://google.github.io/filament/main/filament.html.
        // Adopt GGX, correlated Smith visibility and Schlick; use Fresnel-weighted Lambert
        // diffuse and single scattering (no compensation/DFG LUT). CPU reference, not copied code.
        const auto nh = math::Clamp(math::Dot(normal, half), 0.0F, 1.0F);
        const auto lh = math::Clamp(math::Dot(light, half), 0.0F, 1.0F);
        const auto alpha = roughness * roughness, a2 = alpha * alpha;
        const float32 denominator = (1 - nh * nh) + nh * nh * a2;
        const auto distribution = a2 / (math::kPiF * denominator * denominator);
        const auto visibility =
            0.5F /
            math::Max(nl * math::Sqrt(nv * nv * (1 - a2) + a2) + nv * math::Sqrt(nl * nl * (1 - a2) + a2), 1e-6F);
        const auto fresnelWeight = Fifth(1 - lh);
        const float32 colors[]{baseColor.X, baseColor.Y, baseColor.Z};
        float32 channels[3]{};
        for (usize i = 0; i < 3; ++i)
        {
            const auto f0 = 0.04F * (1 - metallic) + colors[i] * metallic;
            const auto fresnel = f0 + (1 - f0) * fresnelWeight;
            channels[i] =
                nl * (distribution * visibility * fresnel + colors[i] * (1 - metallic) * (1 - fresnel) / math::kPiF);
        }
        result = {channels[0], channels[1], channels[2]};
    }
    output = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus BuildDirectionalShadowMatrix(const math::Aabb3& bounds,
                                               math::Vector3 direction,
                                               uint32 resolution,
                                               math::Matrix4& output) noexcept
{
    if (math::IsFinite(bounds) == false || math::IsEmpty(bounds) || resolution < 4 || resolution > 2048 ||
        math::TryNormalize(direction, direction) != math::MathStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const auto up = math::Abs(direction.Y) > 0.9F ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
    math::Vector3 right;
    if (math::TryNormalize(math::Cross(up, direction), right) != math::MathStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const auto vertical = math::Cross(direction, right);
    auto minimum = math::Vector3{1e30F, 1e30F, 1e30F}, maximum = -minimum;
    for (usize corner = 0; corner < 8; ++corner)
    {
        const math::Vector3 p{(corner & 1) != 0 ? bounds.Max.X : bounds.Min.X,
                              (corner & 2) != 0 ? bounds.Max.Y : bounds.Min.Y,
                              (corner & 4) != 0 ? bounds.Max.Z : bounds.Min.Z};
        const math::Vector3 projected{math::Dot(p, right), math::Dot(p, vertical), math::Dot(p, direction)};
        minimum = {math::Min(minimum.X, projected.X),
                   math::Min(minimum.Y, projected.Y),
                   math::Min(minimum.Z, projected.Z)};
        maximum = {math::Max(maximum.X, projected.X),
                   math::Max(maximum.Y, projected.Y),
                   math::Max(maximum.Z, projected.Z)};
    }
    auto width = maximum.X - minimum.X, height = maximum.Y - minimum.Y;
    if (math::IsFinite(width) == false || math::IsFinite(height) == false || width <= 1e-5F || height <= 1e-5F)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const auto step = math::Max(width, height) / static_cast<float32>(resolution - 2);
    const auto cx =
        static_cast<float32>(math::Floor(static_cast<float64>((minimum.X + maximum.X) * 0.5F / step) + 0.5)) * step;
    const auto cy =
        static_cast<float32>(math::Floor(static_cast<float64>((minimum.Y + maximum.Y) * 0.5F / step) + 0.5)) * step;
    width += 2 * step;
    height += 2 * step;
    const auto near = minimum.Z - step, depth = maximum.Z - minimum.Z + 2 * step;
    if (math::IsFinite(near) == false || math::IsFinite(depth) == false || depth <= 0)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    math::Matrix4 matrix{};
    for (usize column = 0; column < 3; ++column)
    {
        const float32 x[]{right.X, right.Y, right.Z}, y[]{vertical.X, vertical.Y, vertical.Z},
            z[]{direction.X, direction.Y, direction.Z};
        matrix.Columns[column] = {2 * x[column] / width, 2 * y[column] / height, z[column] / depth, 0};
    }
    matrix.Columns[3] = {-2 * cx / width, -2 * cy / height, -near / depth, 1};
    if (math::IsFinite(matrix) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    output = matrix;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer
