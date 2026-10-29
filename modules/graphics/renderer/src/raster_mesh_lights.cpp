#include "internal/raster_mesh_lights.h"

#include "internal/ltc_table.h"
#include "internal/reference_data.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", TOG 35(4),
// Article 41 (2016), section 3, https://doi.org/10.1145/2897824.2925895.
// Actual transformed triangles are integrated, with a view-Fresnel Lambert term
// and the renderer's fitted specular LTC. Textured emission is piecewise constant.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
constexpr usize CORNER_COUNT = 3;
constexpr usize TEXTURED_PATCH_COUNT = 4;
constexpr float32 TRIANGLE_CENTROID_WEIGHT = 1.0F / CORNER_COUNT;
constexpr float32 DIELECTRIC_F0 = 0.04F;
constexpr float32 FRAME_AXIS_THRESHOLD = 0.9F;
constexpr float32 PLANE_EPSILON_METERS = static_cast<float32>(LUDUS_MESH_LIGHT_PLANE_EPSILON_METERS);
constexpr float32 PLANE_RELATIVE_EPSILON = static_cast<float32>(LUDUS_MESH_LIGHT_PLANE_RELATIVE_EPSILON);
float32 ComputeMagnitude(math::Vector3 value) noexcept
{
    return math::Max(math::Abs(value.X), math::Max(math::Abs(value.Y), math::Abs(value.Z)));
}
math::Vector3 GetPosition(math::Vector4 word) noexcept
{
    return {word.X, word.Y, word.Z};
}
} // namespace
rhi::RasterStatus AppendRasterMeshLights(const MeshDescription& mesh,
                                         const math::Affine3& meshToWorld,
                                         const SceneShaderMaterial& material,
                                         Array<math::Vector4>& inoutWords) noexcept
{
    using Status = rhi::RasterStatus;
    if (material.Emission == math::Vector3{})
    {
        return Status::Ready;
    }
    if (mesh.Positions == nullptr || mesh.Indices == nullptr || mesh.IndexCount % CORNER_COUNT != 0)
    {
        return Status::InvalidDescription;
    }
    math::Matrix3 normalMatrix;
    if (math::TryNormalMatrix(meshToWorld, {}, normalMatrix) != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    Array<math::Vector4> candidate;
    ReferenceSurfaceData sampling;
    sampling.UvTransform = material.UvTransform;
    sampling.Sampler = material.Sampler;
    const bool hasEmissionMap = material.EmissionTexture.Bytes != nullptr;
    sampling.Emission = material.Emission;
    sampling.HasEmissionMap = hasEmissionMap;
    ReferenceInstance samplingInstance;
    samplingInstance.Surface = &sampling;
    samplingInstance.EmissionMap = material.EmissionTexture;
    const usize patchCount = hasEmissionMap == true ? TEXTURED_PATCH_COUNT : 1;
    for (usize triangle = 0; triangle < mesh.IndexCount / CORNER_COUNT; ++triangle)
    {
        StaticArray<math::Vector3, CORNER_COUNT> positionsWorld{};
        StaticArray<math::Vector3, CORNER_COUNT> positionsMesh{};
        StaticArray<math::Vector2, CORNER_COUNT> coordinatesUv{};
        for (usize corner = 0; corner < CORNER_COUNT; ++corner)
        {
            const uint32 vertex = mesh.Indices[triangle * CORNER_COUNT + corner];
            if (vertex >= mesh.VertexCount)
            {
                return Status::InvalidDescription;
            }
            positionsMesh[corner] = mesh.Positions[vertex];
            positionsWorld[corner] = math::TransformPoint(meshToWorld, positionsMesh[corner]);
            if (math::IsFinite(positionsWorld[corner]) == false)
            {
                return Status::InvalidDescription;
            }
            coordinatesUv[corner] = mesh.Texcoords == nullptr ? math::Vector2{} : mesh.Texcoords[vertex];
        }
        const math::Vector3 crossWorld =
            math::Cross(positionsWorld[1] - positionsWorld[0], positionsWorld[2] - positionsWorld[0]);
        if (math::IsFinite(crossWorld) == false)
        {
            return Status::InvalidDescription;
        }
        if (math::Length(crossWorld) == 0)
        {
            continue;
        }
        math::Vector3 normalWorld;
        if (math::TryNormalize(
                normalMatrix * math::Cross(positionsMesh[1] - positionsMesh[0], positionsMesh[2] - positionsMesh[0]),
                normalWorld) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        for (usize patch = 0; patch < patchCount; ++patch)
        {
            StaticArray<math::Vector3, CORNER_COUNT> positionsPatchWorld = positionsWorld;
            math::Vector2 centroidUv =
                (coordinatesUv[0] + coordinatesUv[1] + coordinatesUv[2]) * TRIANGLE_CENTROID_WEIGHT;
            if (hasEmissionMap == true)
            {
                // Three corner patches and the central patch partition the source exactly.
                if (patch < CORNER_COUNT)
                {
                    const usize next = (patch + 1) % CORNER_COUNT;
                    const usize previous = (patch + 2) % CORNER_COUNT;
                    positionsPatchWorld[0] = positionsWorld[patch];
                    positionsPatchWorld[1] = (positionsWorld[patch] + positionsWorld[next]) * 0.5F;
                    positionsPatchWorld[2] = (positionsWorld[patch] + positionsWorld[previous]) * 0.5F;
                    centroidUv = (coordinatesUv[patch] * 2 + (coordinatesUv[next] + coordinatesUv[previous]) * 0.5F) *
                                 TRIANGLE_CENTROID_WEIGHT;
                }
                else
                {
                    for (usize corner = 0; corner < CORNER_COUNT; ++corner)
                    {
                        positionsPatchWorld[corner] =
                            (positionsWorld[corner] + positionsWorld[(corner + 1) % CORNER_COUNT]) * 0.5F;
                    }
                }
            }
            const math::Vector3 emission = SampleReferenceEmission(samplingInstance, centroidUv);
            if (math::IsFinite(emission) == false)
            {
                return Status::InvalidDescription;
            }
            if (emission == math::Vector3{})
            {
                continue;
            }
            if ((inoutWords.GetSize() + candidate.GetSize()) / MESH_LIGHT_TRIANGLE_WORDS >=
                MAX_RASTER_MESH_LIGHT_TRIANGLES)
            {
                return Status::CapacityExceeded;
            }
            const StaticArray<math::Vector4, MESH_LIGHT_TRIANGLE_WORDS> words{
                {{positionsPatchWorld[0].X, positionsPatchWorld[0].Y, positionsPatchWorld[0].Z, normalWorld.X},
                 {positionsPatchWorld[1].X, positionsPatchWorld[1].Y, positionsPatchWorld[1].Z, normalWorld.Y},
                 {positionsPatchWorld[2].X, positionsPatchWorld[2].Y, positionsPatchWorld[2].Z, normalWorld.Z},
                 {emission.X, emission.Y, emission.Z, material.IsEmissionTwoSided == true ? 1.0F : 0.0F}}};
            if (candidate.TryAddRange({words.GetData(), words.GetSize()}) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    return inoutWords.TryAddRange({candidate.GetData(), candidate.GetSize()}) == true ? Status::Ready
                                                                                      : Status::OutOfMemory;
}
// Position, normal, view and base colour are distinct vectors, and metallic and roughness distinct material terms.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
math::Vector3 EvaluateRasterMeshLight(BorrowedView<const math::Vector4> words,
                                      math::Vector3 positionWorld,
                                      math::Vector3 normalWorld,
                                      math::Vector3 viewWorld,
                                      math::Vector3 baseColor,
                                      float32 metallic,
                                      float32 roughness) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (words.GetSize() != MESH_LIGHT_TRIANGLE_WORDS || math::Dot(normalWorld, viewWorld) <= 0)
    {
        return {};
    }
    const math::Vector3 emitterNormalWorld{words[0].W, words[1].W, words[2].W};
    const float32 facing = math::Dot(emitterNormalWorld, positionWorld - GetPosition(words[0]));
    const float32 planeTolerance =
        PLANE_EPSILON_METERS +
        PLANE_RELATIVE_EPSILON * math::Max(ComputeMagnitude(positionWorld), ComputeMagnitude(GetPosition(words[0])));
    if (math::Abs(facing) <= planeTolerance || (words[CORNER_COUNT].W == 0 && facing < 0))
    {
        return {};
    }
    const math::Vector3 helperWorld =
        math::Abs(normalWorld.Z) < FRAME_AXIS_THRESHOLD ? math::Vector3{0, 0, 1} : math::Vector3{1, 0, 0};
    math::Vector3 tangentWorld;
    if (math::TryNormalize(math::Cross(helperWorld, normalWorld), tangentWorld) != math::MathStatus::Success)
    {
        return {};
    }
    const math::Vector3 bitangentWorld = math::Cross(normalWorld, tangentWorld);
    math::ProjectedPolygonInput polygon;
    polygon.VertexCount = CORNER_COUNT;
    for (usize corner = 0; corner < CORNER_COUNT; ++corner)
    {
        const math::Vector3 deltaWorld = GetPosition(words[corner]) - positionWorld;
        polygon.PositionsTangent[corner] = {math::Dot(deltaWorld, tangentWorld),
                                            math::Dot(deltaWorld, bitangentWorld),
                                            math::Dot(deltaWorld, normalWorld)};
    }
    math::ProjectedPolygonInput clipped;
    if (math::TryClipProjectedPolygonToUpperHemisphere(polygon, clipped) != math::MathStatus::Success ||
        clipped.VertexCount < CORNER_COUNT)
    {
        return {};
    }
    polygon = clipped;
    math::ProjectedSolidAnglePolygon diffuse;
    if (math::TryPrepareProjectedSolidAnglePolygon(polygon, diffuse) != math::MathStatus::Success)
    {
        return {};
    }
    const math::Vector3 f0 =
        math::Vector3{DIELECTRIC_F0, DIELECTRIC_F0, DIELECTRIC_F0} * (1 - metallic) + baseColor * metallic;
    const float32 complement = 1 - math::Max(math::Dot(normalWorld, viewWorld), 0.0F);
    const float32 fifth = complement * complement * complement * complement * complement;
    const math::Vector3 fresnel = f0 + (math::Vector3{1, 1, 1} - f0) * fifth;
    math::Vector3 result = math::Hadamard(baseColor, math::Vector3{1, 1, 1} - fresnel) *
                           ((1 - metallic) * diffuse.ProjectedSolidAngle / math::kPiF);
    const math::Vector3 viewTangent{math::Dot(viewWorld, tangentWorld),
                                    math::Dot(viewWorld, bitangentWorld),
                                    math::Dot(viewWorld, normalWorld)};
    const SpecularLtc specular = LookupSpecularLtc(roughness, viewTangent, f0);
    math::LinearlyTransformedCosine distribution;
    math::LinearlyTransformedCosinePolygon specularPolygon;
    if (math::TryMakeLinearlyTransformedCosine(specular.InverseTransform, distribution) == math::MathStatus::Success &&
        math::TryPrepareLinearlyTransformedCosinePolygon(polygon, distribution, specularPolygon) ==
            math::MathStatus::Success)
    {
        result = result + ComputeSpecularLtcAlbedo(specular, f0) *
                              math::GetLinearlyTransformedCosinePolygonIntegral(specularPolygon);
    }
    return math::Hadamard(GetPosition(words[CORNER_COUNT]), result);
}
} // namespace ludus::graphics::renderer::internal
