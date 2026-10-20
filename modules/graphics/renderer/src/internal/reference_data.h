#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include "../../shaders/renderer_gpu_preview_layout.h"
#include "environment_data.h"

namespace ludus::graphics::renderer::internal
{
namespace math = foundation::math;
using namespace foundation;
struct ReferenceMeshData final
{
    rhi::DeviceHandle Device;
    Array<math::Vector3> Positions, Normals;
    Array<math::Vector2> Uvs;
    Array<math::Vector2> MaterialUvs1;
    Array<math::Vector4> Tangents;
    Array<math::Vector4> Colors;
    Array<uint32> Indices;
    rhi::RayGeometryHandle Geometry{};
    rhi::RasterStatus NativeStatus = rhi::RasterStatus::NotReady;
    usize CpuBytes = 0;
    ~ReferenceMeshData() noexcept
    {
        static_cast<void>(rhi::Destroy(Device, Geometry));
    }
};
struct ReferenceSurfaceData final
{
    ReferenceBsdf Bsdf;
    math::Vector4 Tint{1, 1, 1, 1}, UvTransform{1, 1, 0, 0};
    math::Vector3 Emission{};
    bool IsEmissionTwoSided = true;
    rhi::SamplerDescription Sampler;
    MaterialAlpha Alpha = MaterialAlpha::Opaque;
    float32 AlphaCutoff = 0.5F;
    bool HasTexture = false;
    bool HasSurfaceMap = false;
    bool HasEmissionMap = false;
    float32 NormalScale = 1;
    bool AreAuthoredInputsEnabled = false;
    uint32 MaterialUvSet = 0;
};
struct ReferenceTriangle final
{
    math::Vector3 Positions[3], Normals[3];
    math::Vector2 Uvs[3];
    math::Vector4 Tangents[3];
    math::Vector4 Colors[3];
    math::Vector3 GeometricNormal{}, Error{};
    float32 Area = 0;
    uint32 Instance = 0, Primitive = 0;
};
struct ReferenceInstance final
{
    const ReferenceSurfaceData* Surface = nullptr;
    rhi::TextureCpuContent Texture;
    rhi::TextureCpuContent SurfaceMap;
    rhi::TextureCpuContent EmissionMap;
    usize FirstTriangle = 0, TriangleCount = 0;
    uint64 SourceId = 0;
};
struct ReferenceBvhNode final
{
    math::Aabb3 Bounds;
    uint32 Left = 0, Right = 0, First = 0, Count = 0;
};
struct ReferenceSceneData final
{
    math::Aabb3 Bounds;
    Array<ReferenceTriangle> Triangles;
    Array<uint32> Order, Emitters;
    Array<ReferenceBvhNode> Nodes;
    Array<ReferenceInstance> Instances;
    usize InstanceCount = 0;
    rhi::RaySceneHandle Native{};
    uint64 FingerprintLow = 0, FingerprintHigh = 0;
    usize CpuBytes = 0;
};
struct ReferenceSum final
{
    math::Vector3 Value{}, Compensation{};
    void Add(math::Vector3 sample) noexcept
    {
        const auto adjusted = sample - Compensation;
        const auto combined = Value + adjusted;
        Compensation = (combined - Value) - adjusted;
        Value = combined;
    }
};
struct ReferenceAccumulator final
{
    ReferenceSum Emission, Direct, Indirect;
    uint32 Samples = 0;
};
struct ReferencePath final
{
    math::Vector3 Emission{}, Direct{}, Indirect{}, Throughput{1, 1, 1};
    math::Vector3 PreviousPoint{}, PendingDirect{};
    rhi::RayInput Ray{}, Shadow{}, Next{};
    uint32 Pixel = 0, Sample = 0, Depth = 0, DirectDepth = 0;
    float32 PreviousPdf = 0;
    usize ShadowQuery = ~usize{0}, NextQuery = ~usize{0};
    bool Active = true, HasShadow = false, HasNext = false;
};
enum class ReferenceStage : uint8
{
    Setup,
    Primary,
    WaitPrimary,
    Bounce,
    WaitBounce,
    Commit,
    Complete,
    Failed,
};
struct ReferenceJob final
{
    rhi::DeviceHandle Device;
    bool IsCpuOnly = false;
    ReferenceCaptureDescription Description;
    DirectLight Lights[128];
    EnvironmentData* EnvironmentOrNull = nullptr;
    math::Matrix4 ClipToWorld;
    math::RandomKey Key;
    ReferenceSceneData Scene;
    Array<ReferenceSurfaceData> FlatSurfaces;
    Array<ReferenceAccumulator> Pixels;
    Array<ReferencePath> Paths;
    Array<rhi::RayInput> Rays;
    Array<rhi::RayResult> Hits;
    rhi::RayQueryBatch Batch{};
    ReferenceStage Stage = ReferenceStage::Setup;
    ReferenceCaptureInfo Info;
    usize FirstPixel = 0, PathCount = 0, QueryCount = 0;
    ~ReferenceJob() noexcept
    {
        ReleaseEnvironment(EnvironmentOrNull);
        if (IsCpuOnly == false)
        {
            static_cast<void>(rhi::Destroy(Device, Batch));
            static_cast<void>(rhi::Destroy(Device, Scene.Native));
        }
    }
};
struct ReferenceCaptureRecord final
{
    uint64 Generation = 1;
    bool Used = false, Owned = false;
    usize SnapshotSlot = 0;
    core::UniquePtr<ReferenceJob> Job;
};
struct ReferenceState final
{
    ReferenceRenderingOptions Options;
    rhi::RayQueryProgram Program{};
    ReferenceCaptureRecord Captures[4];
    usize CpuBytes = 0;
};
[[nodiscard]] inline bool ReferenceColor(math::Vector3 value, float32 maximum) noexcept
{
    return math::IsFinite(value) && value.X >= 0 && value.Y >= 0 && value.Z >= 0 && value.X <= maximum &&
           value.Y <= maximum && value.Z <= maximum;
}
[[nodiscard]] inline float32 ReferencePowerWeight(float32 pdf, float32 other) noexcept
{
    // Ratio form avoids overflow and underflow when converting area PDFs to angle.
    if (pdf <= 0)
    {
        return 0;
    }
    if (other <= 0)
    {
        return 1;
    }
    const auto maximum = math::Max(pdf, other);
    const auto a = pdf / maximum, b = other / maximum;
    return a * a / (a * a + b * b);
}
[[nodiscard]] inline float32
ReferenceRandom(const ReferenceJob& job, const ReferencePath& path, uint16 dimension) noexcept
{
    return math::SampleFloat01(job.Key, {path.Pixel, path.Sample, dimension});
}
static_assert(LUDUS_PATH_RANDOM_FIRST_BOUNCE_DIMENSION +
                      LUDUS_REFERENCE_MAX_DEPTH * LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE - 1 <=
                  math::RandomMaxDimension,
              "every admitted bounce dimension must fit addressed-random layout v1");
static_assert(LUDUS_GPU_PREVIEW_MAX_DEPTH <= LUDUS_REFERENCE_MAX_DEPTH, "GPU preview mirrors reference addresses");
// Dimension `slot` of scattering event `depth`. Admission bounds MaxDepth by
// LUDUS_REFERENCE_MAX_DEPTH, so the result never narrows a wider value.
[[nodiscard]] inline uint16 ReferenceBounceDimension(uint32 depth, uint32 slot) noexcept
{
    LUDUS_ASSERT(depth < LUDUS_REFERENCE_MAX_DEPTH && slot < LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE);
    return static_cast<uint16>(LUDUS_PATH_RANDOM_FIRST_BOUNCE_DIMENSION +
                               depth * LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE + slot);
}
[[nodiscard]] inline math::Vector3 ReferenceNormalize(math::Vector3 v) noexcept
{
    math::Vector3 result;
    static_cast<void>(math::TryNormalize(v, result));
    return result;
}
[[nodiscard]] inline rhi::RayInput
ReferenceRay(math::Vector3 origin, math::Vector3 direction, float32 distance) noexcept
{
    rhi::RayInput result;
    result.Origin[0] = origin.X;
    result.Origin[1] = origin.Y;
    result.Origin[2] = origin.Z;
    result.Direction[0] = direction.X;
    result.Direction[1] = direction.Y;
    result.Direction[2] = direction.Z;
    result.MaxDistance = distance;
    return result;
}
[[nodiscard]] inline math::Vector3 ReferenceDirection(const rhi::RayInput& ray) noexcept
{
    return {ray.Direction[0], ray.Direction[1], ray.Direction[2]};
}
[[nodiscard]] inline math::Vector3 ReferenceOrigin(const rhi::RayInput& ray) noexcept
{
    return {ray.Origin[0], ray.Origin[1], ray.Origin[2]};
}
[[nodiscard]] math::Vector3 ComputeReferencePositionError(const math::Affine3& meshToWorld,
                                                          math::Vector3 positionMesh) noexcept;
/// Private implementations in reference_scene.cpp; no renderer/resource ownership.
[[nodiscard]] rhi::RasterStatus BuildReferenceBvh(ReferenceSceneData&) noexcept;
[[nodiscard]] rhi::RasterStatus
TraceReferenceOracle(const ReferenceSceneData&, const rhi::RayInput&, rhi::RayResult&) noexcept;
[[nodiscard]] math::Vector4 SampleReferenceTexture(const ReferenceInstance&, math::Vector2) noexcept;
[[nodiscard]] math::Vector3 SampleReferenceEmission(const ReferenceInstance&, math::Vector2) noexcept;
[[nodiscard]] math::Vector4 SampleReferenceSurfaceMap(const ReferenceInstance&, math::Vector2) noexcept;
[[nodiscard]] math::Vector3 ApplyReferenceNormal(const ReferenceTriangle&,
                                                 const ReferenceSurfaceData&,
                                                 math::Vector3,
                                                 math::Vector3,
                                                 float32 = 0,
                                                 float32 = 0) noexcept;
[[nodiscard]] bool ReferenceOpacity(const ReferenceInstance&, math::Vector2, float32 = 1) noexcept;
struct ReferenceProbeAddress final
{
    uint32 TriangleIndex = 0;
    uint32 SampleIndex = 0;
};
// Reads the job immutably, so disjoint probes may run concurrently against one
// prepared job. Path diagnostics go to the caller's counters, never job.Info.
[[nodiscard]] rhi::RasterStatus EvaluateReferenceDiffuseProbe(const ReferenceJob& job,
                                                              ReferenceCaptureInfo& inoutInfo,
                                                              const ReferenceProbeAddress& address,
                                                              math::Vector3& outIrradiance) noexcept;
[[nodiscard]] rhi::RasterStatus PrepareReferenceCamera(ReferenceJob&) noexcept;
[[nodiscard]] rhi::RasterStatus GenerateReferencePrimary(const ReferenceJob&, ReferencePath&, rhi::RayInput&) noexcept;
// Writes path diagnostics only to inoutInfo; the job itself is read-only.
[[nodiscard]] rhi::RasterStatus PrepareReferenceVertex(const ReferenceJob& job,
                                                       ReferenceCaptureInfo& inoutInfo,
                                                       ReferencePath& inoutPath,
                                                       const rhi::RayResult& hit) noexcept;
} // namespace ludus::graphics::renderer::internal
