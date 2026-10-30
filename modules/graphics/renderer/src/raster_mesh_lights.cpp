#include "internal/raster_mesh_lights.h"

#include "internal/ltc_table.h"
#include "internal/raster_mesh_light_polygons.h"
#include "internal/reference_data.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", TOG 35(4),
// Article 41 (2016), section 3, https://doi.org/10.1145/2897824.2925895.
// Actual transformed polygons are integrated, with a view-Fresnel Lambert term
// and the renderer's fitted specular LTC. Both integrals are additive over a
// partition of the emitter, so coplanar constant-emission triangles merge into
// convex polygon records (raster_mesh_light_polygons.h). Textured emission is
// piecewise constant over unmerged triangle patches.
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
constexpr usize NORMAL_WORD = 0;
constexpr usize EMISSION_WORD = 1;
// The CPU check fans a record into pieces whose receiver-horizon clip, which adds at most one corner, still fits
// the projected polygon input; the integrals are additive over the fan.
constexpr usize MAX_CHECK_PIECE_CORNERS = math::MAX_PROJECTED_POLYGON_INPUT_VERTICES - 1;
float32 ComputeMagnitude(math::Vector3 value) noexcept
{
    return math::Max(math::Abs(value.X), math::Max(math::Abs(value.Y), math::Abs(value.Z)));
}
math::Vector3 GetPosition(math::Vector4 word) noexcept
{
    return {word.X, word.Y, word.Z};
}
// One record: unit normal and sidedness, emission and corner count, then exactly that many world corners.
[[nodiscard]] bool TryAppendRecord(BorrowedView<const math::Vector3> cornersWorld,
                                   math::Vector3 normalWorld,
                                   math::Vector3 emission,
                                   bool isTwoSided,
                                   Array<math::Vector4>& inoutWords) noexcept
{
    if (inoutWords.TryAdd({normalWorld.X, normalWorld.Y, normalWorld.Z, isTwoSided == true ? 1.0F : 0.0F}) == false ||
        inoutWords.TryAdd({emission.X, emission.Y, emission.Z, static_cast<float32>(cornersWorld.GetSize())}) == false)
    {
        return false;
    }
    for (usize corner = 0; corner < cornersWorld.GetSize(); ++corner)
    {
        if (inoutWords.TryAdd({cornersWorld[corner].X, cornersWorld[corner].Y, cornersWorld[corner].Z, 0}) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace
rhi::RasterStatus AppendRasterMeshLights(const MeshDescription& mesh,
                                         const math::Affine3& meshToWorld,
                                         const SceneShaderMaterial& material,
                                         Array<math::Vector4>& inoutWords,
                                         uint32& inoutRecordCount) noexcept
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
    // Validate every triangle and keep those with positive world area.
    Array<uint32> emitters;
    for (usize triangle = 0; triangle < mesh.IndexCount / CORNER_COUNT; ++triangle)
    {
        StaticArray<math::Vector3, CORNER_COUNT> positionsWorld{};
        StaticArray<math::Vector3, CORNER_COUNT> positionsMesh{};
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
        if (emitters.TryAdd(static_cast<uint32>(triangle)) == false)
        {
            return Status::OutOfMemory;
        }
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
    // Constant emission is the same over a whole mesh, so its coplanar regions merge exactly.
    RasterEmitterPartition partition;
    if (hasEmissionMap == false && TryPartitionRasterEmitterRegions(mesh.Positions,
                                                                    mesh.Indices,
                                                                    {emitters.GetData(), emitters.GetSize()},
                                                                    partition) == false)
    {
        return Status::OutOfMemory;
    }
    uint32 recordCount = inoutRecordCount;
    const auto appendRecord = [&](BorrowedView<const math::Vector3> cornersWorld,
                                  math::Vector3 normalWorld,
                                  math::Vector3 emission) noexcept -> Status {
        if (recordCount >= MAX_RASTER_MESH_LIGHT_POLYGONS)
        {
            return Status::CapacityExceeded;
        }
        if (TryAppendRecord(cornersWorld, normalWorld, emission, material.IsEmissionTwoSided, candidate) == false)
        {
            return Status::OutOfMemory;
        }
        ++recordCount;
        return Status::Ready;
    };
    const usize patchCount = hasEmissionMap == true ? TEXTURED_PATCH_COUNT : 1;
    usize nextPolygon = 0;
    for (usize emitter = 0; emitter < emitters.GetSize(); ++emitter)
    {
        const uint32 triangle = emitters[emitter];
        for (; nextPolygon < partition.Polygons.GetSize() && partition.Polygons[nextPolygon].FirstTriangle == triangle;
             ++nextPolygon)
        {
            const RasterEmitterPolygon& polygon = partition.Polygons[nextPolygon];
            StaticArray<math::Vector3, MAX_MESH_LIGHT_POLYGON_CORNERS> cornersWorld{};
            for (usize corner = 0; corner < polygon.CornerCount; ++corner)
            {
                cornersWorld[corner] = math::TransformPoint(meshToWorld, polygon.PositionsMesh[corner]);
            }
            math::Vector3 normalWorld;
            if (math::TryNormalize(normalMatrix * polygon.NormalMesh, normalWorld) != math::MathStatus::Success)
            {
                return Status::InvalidDescription;
            }
            const math::Vector3 emission = SampleReferenceEmission(samplingInstance, {});
            if (math::IsFinite(emission) == false)
            {
                return Status::InvalidDescription;
            }
            const Status status = appendRecord({cornersWorld.GetData(), polygon.CornerCount}, normalWorld, emission);
            if (status != Status::Ready)
            {
                return status;
            }
        }
        if (partition.IsMerged.IsEmpty() == false && partition.IsMerged[emitter] == true)
        {
            continue;
        }
        StaticArray<math::Vector3, CORNER_COUNT> positionsWorld{};
        StaticArray<math::Vector3, CORNER_COUNT> positionsMesh{};
        StaticArray<math::Vector2, CORNER_COUNT> coordinatesUv{};
        for (usize corner = 0; corner < CORNER_COUNT; ++corner)
        {
            const uint32 vertex = mesh.Indices[triangle * CORNER_COUNT + corner];
            positionsMesh[corner] = mesh.Positions[vertex];
            positionsWorld[corner] = math::TransformPoint(meshToWorld, positionsMesh[corner]);
            coordinatesUv[corner] = mesh.Texcoords == nullptr ? math::Vector2{} : mesh.Texcoords[vertex];
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
            const Status status =
                appendRecord({positionsPatchWorld.GetData(), positionsPatchWorld.GetSize()}, normalWorld, emission);
            if (status != Status::Ready)
            {
                return status;
            }
        }
    }
    if (inoutWords.TryAddRange({candidate.GetData(), candidate.GetSize()}) == false)
    {
        return Status::OutOfMemory;
    }
    inoutRecordCount = recordCount;
    return Status::Ready;
}
usize GetRasterMeshLightRecordWords(BorrowedView<const math::Vector4> words, usize offset) noexcept
{
    if (offset >= words.GetSize() || words.GetSize() - offset < MESH_LIGHT_POLYGON_HEADER_WORDS)
    {
        return 0;
    }
    const float32 cornerWord = words[offset + EMISSION_WORD].W;
    if ((cornerWord >= static_cast<float32>(CORNER_COUNT) &&
         cornerWord <= static_cast<float32>(MAX_MESH_LIGHT_POLYGON_CORNERS)) == false)
    {
        return 0;
    }
    const usize recordWords = MESH_LIGHT_POLYGON_HEADER_WORDS + static_cast<usize>(cornerWord);
    return words.GetSize() - offset >= recordWords ? recordWords : 0;
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
    if (words.IsEmpty() == true || GetRasterMeshLightRecordWords(words, 0) != words.GetSize() ||
        math::Dot(normalWorld, viewWorld) <= 0)
    {
        return {};
    }
    const usize cornerCount = words.GetSize() - MESH_LIGHT_POLYGON_HEADER_WORDS;
    const math::Vector3 emitterNormalWorld = GetPosition(words[NORMAL_WORD]);
    const math::Vector3 originWorld = GetPosition(words[MESH_LIGHT_POLYGON_HEADER_WORDS]);
    const float32 facing = math::Dot(emitterNormalWorld, positionWorld - originWorld);
    const float32 planeTolerance =
        PLANE_EPSILON_METERS +
        PLANE_RELATIVE_EPSILON * math::Max(ComputeMagnitude(positionWorld), ComputeMagnitude(originWorld));
    if (math::Abs(facing) <= planeTolerance || (words[NORMAL_WORD].W == 0 && facing < 0))
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
    const math::Vector3 f0 =
        math::Vector3{DIELECTRIC_F0, DIELECTRIC_F0, DIELECTRIC_F0} * (1 - metallic) + baseColor * metallic;
    const float32 complement = 1 - math::Max(math::Dot(normalWorld, viewWorld), 0.0F);
    const float32 fifth = complement * complement * complement * complement * complement;
    const math::Vector3 fresnel = f0 + (math::Vector3{1, 1, 1} - f0) * fifth;
    const math::Vector3 diffuseAlbedo = math::Hadamard(baseColor, math::Vector3{1, 1, 1} - fresnel) * (1 - metallic);
    const math::Vector3 viewTangent{math::Dot(viewWorld, tangentWorld),
                                    math::Dot(viewWorld, bitangentWorld),
                                    math::Dot(viewWorld, normalWorld)};
    const SpecularLtc specular = LookupSpecularLtc(roughness, viewTangent, f0);
    math::LinearlyTransformedCosine distribution;
    const bool hasSpecular =
        math::TryMakeLinearlyTransformedCosine(specular.InverseTransform, distribution) == math::MathStatus::Success;
    const math::Vector3 specularAlbedo = ComputeSpecularLtcAlbedo(specular, f0);
    // Fan from the first corner; each piece is clipped and integrated with the sampling preparation.
    math::Vector3 result{};
    for (usize start = 1; start + 1 < cornerCount;)
    {
        const usize last = math::Min(start + MAX_CHECK_PIECE_CORNERS - 2, cornerCount - 1);
        math::ProjectedPolygonInput polygon;
        polygon.VertexCount = static_cast<uint32>(last - start + 2);
        for (usize piece = 0; piece < polygon.VertexCount; ++piece)
        {
            const usize corner = piece == 0 ? 0 : start + piece - 1;
            const math::Vector3 deltaWorld =
                GetPosition(words[MESH_LIGHT_POLYGON_HEADER_WORDS + corner]) - positionWorld;
            polygon.PositionsTangent[piece] = {math::Dot(deltaWorld, tangentWorld),
                                               math::Dot(deltaWorld, bitangentWorld),
                                               math::Dot(deltaWorld, normalWorld)};
        }
        start = last;
        math::ProjectedPolygonInput clipped;
        if (math::TryClipProjectedPolygonToUpperHemisphere(polygon, clipped) != math::MathStatus::Success ||
            clipped.VertexCount < CORNER_COUNT)
        {
            continue;
        }
        math::ProjectedSolidAnglePolygon diffuse;
        if (math::TryPrepareProjectedSolidAnglePolygon(clipped, diffuse) != math::MathStatus::Success)
        {
            continue;
        }
        result = result + diffuseAlbedo * (diffuse.ProjectedSolidAngle / math::kPiF);
        math::LinearlyTransformedCosinePolygon specularPolygon;
        if (hasSpecular == true &&
            math::TryPrepareLinearlyTransformedCosinePolygon(clipped, distribution, specularPolygon) ==
                math::MathStatus::Success)
        {
            result = result + specularAlbedo * math::GetLinearlyTransformedCosinePolygonIntegral(specularPolygon);
        }
    }
    return math::Hadamard(GetPosition(words[EMISSION_WORD]), result);
}
} // namespace ludus::graphics::renderer::internal
