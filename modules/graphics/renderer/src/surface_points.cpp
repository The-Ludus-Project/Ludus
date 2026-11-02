#include <ludus/graphics/renderer/surface_points.hpp>

#include "internal/render_graph_ray_scene.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/graphics/renderer/materials.hpp>

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, section 3.2, https://arxiv.org/abs/2510.18189:
// geometry/material/emission point features decouple lighting resolution from mesh
// vertices. This is reusable input preparation, not their trained encoder/decoder.
namespace ludus::graphics::renderer
{
using namespace foundation;
namespace
{
constexpr usize SURFACE_POINT_BYTES = 64;
constexpr uint32 ID_HIGH_SHIFT = 32;
static_assert(sizeof(SurfacePoint) == SURFACE_POINT_BYTES);
using Status = rhi::RasterStatus;
} // namespace
Status PrepareSurfacePointCloud(const RenderGraphSceneDescription& scene,
                                const SurfacePointSampling& sampling,
                                SurfacePointCloud& outCloud) noexcept
{
    if (scene.HasMaterialInterface == false || sampling.Count == 0 || sampling.MaximumAttempts < sampling.Count)
    {
        return Status::InvalidDescription;
    }
    usize pointBytes = 0;
    if (TryMultiply(usize{sampling.Count}, sizeof(SurfacePoint), pointBytes) == false ||
        pointBytes >= sampling.MaximumBytes)
    {
        return Status::CapacityExceeded;
    }
    ReferenceCaptureDescription preparation;
    preparation.Width = 1;
    preparation.Height = 1;
    preparation.Seed = sampling.Seed;
    // The shared scene freezer also admits a camera. A fixed orthographic identity
    // satisfies that contract without making surface features depend on a view.
    preparation.OrthographicCamera = true;
    core::UniquePtr<internal::ReferenceJob> job;
    RenderGraphSceneDescription strictScene = scene;
    strictScene.AreErrorsAllowed = false;
    const Status prepared =
        internal::BuildMaterialGraphRayScene(strictScene, preparation, sampling.MaximumBytes - pointBytes, job);
    if (prepared != Status::Ready)
    {
        return prepared;
    }
    const internal::ReferenceSceneData& data = job->Scene;
    if (data.Triangles.IsEmpty() == true)
    {
        return Status::InvalidDescription;
    }
    usize scratchBytes = 0;
    usize totalBytes = 0;
    if (TryMultiply(data.Triangles.GetSize(), sizeof(float64), scratchBytes) == false ||
        TryAdd(job->Info.CpuBytes, pointBytes, totalBytes) == false ||
        TryAdd(totalBytes, scratchBytes, totalBytes) == false || totalBytes > sampling.MaximumBytes)
    {
        return Status::CapacityExceeded;
    }
    Array<float64> cumulativeAreas;
    SurfacePointCloud candidate;
    if (cumulativeAreas.TryResize(data.Triangles.GetSize()) == false ||
        candidate.Points.TryResize(sampling.Count) == false)
    {
        return Status::OutOfMemory;
    }
    float64 area = 0;
    for (usize i = 0; i < data.Triangles.GetSize(); ++i)
    {
        area += float64(data.Triangles[i].Area);
        cumulativeAreas[i] = area;
    }
    if (math::IsFinite(area) == false || area <= 0)
    {
        return Status::InvalidDescription;
    }
    math::RandomStream random;
    static_cast<void>(random.TryReseed(sampling.Seed, 0));
    usize written = 0;
    for (uint64 attempt = 0; attempt < sampling.MaximumAttempts && written < sampling.Count; ++attempt)
    {
        const float64 selectedArea = float64(random.NextFloat01()) * area;
        usize first = 0;
        usize end = cumulativeAreas.GetSize();
        while (first < end)
        {
            const usize middle = first + (end - first) / 2;
            if (selectedArea < cumulativeAreas[middle])
            {
                end = middle;
            }
            else
            {
                first = middle + 1;
            }
        }
        const internal::ReferenceTriangle& triangle = data.Triangles[first];
        const internal::ReferenceInstance& instance = data.Instances[triangle.Instance];
        const internal::ReferenceSurfaceData& surface = *instance.Surface;
        const float32 root = math::Sqrt(random.NextFloat01());
        const float32 u = 1 - root;
        const float32 v = root * random.NextFloat01();
        const math::Vector2 texcoordUv = internal::InterpolateReferenceUv(triangle, u, v);
        const math::Vector4 color = internal::InterpolateReferenceColor(triangle, u, v);
        if (internal::ReferenceOpacity(instance, texcoordUv, color.W) == false)
        {
            continue;
        }
        const math::Vector4 texel = math::Hadamard(internal::SampleReferenceTexture(instance, texcoordUv), color);
        SurfacePoint point;
        point.PositionWorld = internal::InterpolateReferencePosition(triangle, u, v);
        if (math::TryNormalize(triangle.Normals[0] * (1 - u - v) + triangle.Normals[1] * u + triangle.Normals[2] * v,
                               point.NormalWorld) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        if (surface.HasSurfaceMap == true)
        {
            SurfaceParameters parameters;
            const Status decoded = DecodeSurfaceParameters(internal::SampleReferenceSurfaceMap(instance, texcoordUv),
                                                           surface.Bsdf.Metallic,
                                                           surface.Bsdf.Roughness,
                                                           surface.NormalScale,
                                                           parameters);
            if (decoded != Status::Ready)
            {
                return decoded;
            }
            point.NormalWorld =
                internal::ApplyReferenceNormal(triangle, surface, point.NormalWorld, parameters.NormalTangent, u, v);
        }
        point.TriangleIndex = static_cast<uint32>(first);
        point.MaterialIndex = triangle.ShaderMaterial;
        point.SourceIdLow = static_cast<uint32>(triangle.ShaderSourceId);
        point.SourceIdHigh = static_cast<uint32>(triangle.ShaderSourceId >> ID_HIGH_SHIFT);
        point.Albedo = math::Hadamard(surface.Bsdf.BaseColor, {texel.X, texel.Y, texel.Z});
        point.Emission = internal::SampleReferenceEmission(instance, texcoordUv);
        if (math::IsFinite(point.PositionWorld) == false || math::IsFinite(point.NormalWorld) == false ||
            math::IsFinite(point.Albedo) == false || math::IsFinite(point.Emission) == false)
        {
            return Status::InvalidDescription;
        }
        candidate.Points[written++] = point;
    }
    if (written != sampling.Count)
    {
        return Status::CapacityExceeded;
    }
    candidate.BoundsWorld = data.Bounds;
    candidate.ProposalAreaWorld = area;
    candidate.SourceGeneration = scene.SourceGeneration;
    outCloud = Move(candidate);
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
