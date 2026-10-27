#pragma once

#include <ludus/content/render_graph_format/scene_data.h>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "../../shaders/renderer_gpu_preview_layout.h"
#include "../../shaders/renderer_reference_layout.h"
#include "environment_data.h"

namespace ludus::graphics::renderer::internal
{
/// Shared addressed-random domain for reference captures and authored triangle-ray graph jobs.
inline constexpr foundation::uint32 REFERENCE_RANDOM_DOMAIN = 0x52543031;
/// Largest absolute world-space coordinate, in metres, that reference and preview rays accept.
inline constexpr foundation::float32 MAX_WORLD_COORDINATE = 1000000;
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
    uint32 ShaderInstance = 0;
    uint32 ShaderDraw = 0;
    uint32 ShaderMaterial = DEFAULT_SCENE_MATERIAL;
    uint32 ShaderSlot = 0;
    uint32 ShaderMaterialKind = 0;
    uint32 ShaderEmitter = DEFAULT_SCENE_MATERIAL;
    uint64 ShaderSourceId = 0;
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
/// Largest triangle count a BVH leaf may hold; larger ranges always split.
inline constexpr uint32 REFERENCE_BVH_MAX_LEAF_TRIANGLES = 4;
/// Deepest BVH leaf. Traversal keeps at most one deferred sibling per level, so this
/// bound keeps both the CPU oracle's and the GPU kernel's 64-entry stacks sufficient.
inline constexpr uint32 REFERENCE_BVH_MAX_DEPTH = 48;
static_assert(REFERENCE_BVH_MAX_DEPTH + 2 < LUDUS_REFERENCE_STACK_SIZE, "traversal stack covers the deepest leaf");
struct ReferenceBvhNode final
{
    math::Aabb3 Bounds;
    uint32 Left = 0, Right = 0, First = 0, Count = 0;
    /// Interior nodes: axis (0-2) whose lower centroids went to Left. Traversal visits
    /// Left first when the ray direction along it is nonnegative. Zero for leaves.
    uint32 SplitAxis = 0;
};
/// Per-triangle uint32 arrays of ReferenceSceneData (Order, Emitters and EmitterCdf)
/// that scene memory estimates reserve before the BVH and emitter CDF are built.
inline constexpr usize REFERENCE_TRIANGLE_INDEX_ARRAYS = 3;
struct ReferenceSceneData final
{
    math::Aabb3 Bounds;
    Array<ReferenceTriangle> Triangles;
    Array<uint32> Order, Emitters;
    /// Inclusive cumulative integer selection weights, one per ascending `Emitters` entry.
    /// BuildReferenceEmitterSelection derives them from emitted luminance times area.
    Array<uint32> EmitterCdf;
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
    // GpuCompute: submit one sample per pixel, read its reduced status, then
    // periodically read the committed sums back for copy/export.
    Dispatch,
    WaitDispatch,
    ReadStatus,
    ReadPixels,
};
/// Most simultaneous readback slices one GPU-compute capture uses; the RHI allows four.
inline constexpr usize REFERENCE_COMPUTE_READBACKS = 4;
/// Committed-sum bytes read back per pixel: emission/count, direct/status, indirect.
inline constexpr usize REFERENCE_COMPUTE_SUM_BYTES = LUDUS_REFERENCE_SUM_WORDS * sizeof(math::Vector4);
/// Accumulation bytes per pixel on the device: committed sums and their compensation.
inline constexpr usize REFERENCE_COMPUTE_PIXEL_BYTES = LUDUS_REFERENCE_PIXEL_WORDS * sizeof(math::Vector4);
/// Reduced dispatch status bytes read back after every sample.
inline constexpr usize REFERENCE_COMPUTE_STATUS_BYTES = LUDUS_REFERENCE_STATUS_WORDS * sizeof(math::Vector4);
/// Apply a completed dispatch status without publishing pixels; preserves the addressed
/// failing sample and returns Failed or CapacityExceeded when any pixel was rejected.
rhi::RasterStatus ApplyReferenceComputeStatus(const StaticArray<uint8, REFERENCE_COMPUTE_STATUS_BYTES>& bytes,
                                              ReferenceCaptureInfo& inoutInfo) noexcept;
/// Shared GPU-compute kernels and layout, created by EnableReferenceRendering.
struct ReferenceComputePipelines final
{
    rhi::RasterShaderHandle TraceShader{};
    rhi::RasterShaderHandle ReduceShader{};
    rhi::BindingLayoutHandle Layout{};
    rhi::ComputePipelineHandle TracePipeline{};
    rhi::ComputePipelineHandle ReducePipeline{};
    uint32 Phase = 0;
    rhi::RasterStatus Setup = rhi::RasterStatus::Pending;
};
/// One GPU-compute capture's device buffers and dispatch/readback progress.
/// Destruction releases tickets and destroys handles; the RHI retains accepted
/// GPU uses through their completion.
struct ReferenceComputeJob final
{
    rhi::DeviceHandle Device;
    rhi::BufferHandle Parameters{};
    rhi::BufferHandle Scene{};
    rhi::BufferHandle Texels{};
    rhi::BufferHandle Accumulation{};
    rhi::BufferHandle Status{};
    rhi::BindingSetHandle Bindings{};
    bool IsBindingsCreated = false;
    usize SceneBytes = 0;
    usize TexelBytes = 0;
    usize GpuBytes = 0;
    rhi::SubmissionToken Dispatch{};
    rhi::ReadbackTicket StatusReadback{};
    StaticArray<rhi::ReadbackTicket, REFERENCE_COMPUTE_READBACKS> PixelReadbacks{};
    StaticArray<usize, REFERENCE_COMPUTE_READBACKS> ReadbackFirstPixels{};
    StaticArray<usize, REFERENCE_COMPUTE_READBACKS> ReadbackPixelCounts{};
    /// Pixels per readback slice, bounded by the device transfer size.
    usize ChunkPixels = 0;
    usize NextReadPixel = 0;
    usize ReadPixels = 0;
    /// Failure observed by the last status readback, reported once pixels are read.
    rhi::RasterStatus Failure = rhi::RasterStatus::Ready;
    Array<uint8> Chunk;
    ~ReferenceComputeJob() noexcept;
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
    uint64 SourceGeneration = 0;
    uint32 SceneData = ::ludus::content::render_graph_format::SCENE_DATA_ALL;
    Array<math::Vector4> ShaderEmitters;
    Array<uint8> ShaderTextureBytes;
    Array<ReferenceAccumulator> Pixels;
    Array<ReferencePath> Paths;
    Array<rhi::RayInput> Rays;
    Array<rhi::RayResult> Hits;
    rhi::RayQueryBatch Batch{};
    core::UniquePtr<ReferenceComputeJob> Compute;
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
    ReferenceComputePipelines Compute;
    ReferenceCaptureRecord Captures[4];
    usize CpuBytes = 0;
    usize GpuBytes = 0;
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
/// Integer total that emitter selection weights are quantized against before rounding.
/// Positive weights keep at least one unit, so every sampleable emitter stays reachable.
inline constexpr uint32 REFERENCE_EMITTER_WEIGHT_TOTAL = uint32{1} << 24;
/// Builds `EmitterCdf` from every emitter's luminance times area after `Emitters` and
/// instance surfaces are final. Leaves `EmitterCdf` unchanged and fails when emitter
/// indices are not strictly ascending triangle indices or the integer total exceeds 32 bits.
[[nodiscard]] rhi::RasterStatus BuildReferenceEmitterSelection(ReferenceSceneData& inoutScene) noexcept;
/// Returns the emitter whose CDF interval contains `value`; `value` must be below the CDF total.
[[nodiscard]] usize SelectReferenceEmitter(const ReferenceSceneData& scene, uint32 value) noexcept;
/// Finds the emitter entry of `triangle`; false leaves `outEmitter` unchanged.
[[nodiscard]] bool FindReferenceEmitter(const ReferenceSceneData& scene, uint32 triangle, usize& outEmitter) noexcept;
/// Probability that the emitter CDF selects `emitter`, or zero for an empty total.
[[nodiscard]] float32 GetReferenceEmitterProbability(const ReferenceSceneData& scene, usize emitter) noexcept;
[[nodiscard]] rhi::RasterStatus
TraceReferenceOracle(const ReferenceSceneData&, const rhi::RayInput&, rhi::RayResult&) noexcept;
[[nodiscard]] math::Vector4 SampleReferenceTexture(const ReferenceInstance&, math::Vector2) noexcept;
[[nodiscard]] math::Vector3 SampleReferenceEmission(const ReferenceInstance&, math::Vector2) noexcept;
/// Interpolates admitted [0,1] vertex multipliers, bounding finite rounding/extrapolation error.
/// Nonfinite arithmetic remains visible to the reference path's validation.
[[nodiscard]] math::Vector4 InterpolateReferenceColor(const ReferenceTriangle& triangle, float32 u, float32 v) noexcept;
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
/// Private GPU-compute implementations in reference_compute.cpp.
/// Validate both reflected kernels against the frozen ABI and create their shaders.
[[nodiscard]] rhi::RasterStatus CreateReferenceComputePipelines(rhi::DeviceHandle device,
                                                                const rhi::RasterShaderDescription& trace,
                                                                const rhi::RasterShaderDescription& reduce,
                                                                ReferenceComputePipelines& outPipelines) noexcept;
/// Advance layout/pipeline creation without waiting; Ready once both pipelines are.
[[nodiscard]] rhi::RasterStatus PollReferenceComputePipelines(rhi::DeviceHandle device,
                                                              ReferenceComputePipelines& inoutPipelines) noexcept;
void DestroyReferenceComputePipelines(rhi::DeviceHandle device, ReferenceComputePipelines& inoutPipelines) noexcept;
/// Packed scene inputs of one capture; borrowed until CreateReferenceComputeJob returns.
struct ReferenceComputeInputs final
{
    BorrowedView<const math::Vector4> Scene;
    BorrowedView<const math::Vector4> Texels;
    BorrowedView<const uint8> Parameters;
    /// Zero bytes covering the accumulation and status buffers' initial contents.
    BorrowedView<const uint8> Zeros;
    /// Remaining renderer CPU bytes for the job's own state and readback scratch.
    usize CpuByteLimit = 0;
    /// Remaining GpuByteBudget bytes for the job's buffers.
    usize GpuByteLimit = 0;
};
/// Create the device buffers of inoutJob.Compute and enter Setup. Failure leaves the
/// job without compute resources.
[[nodiscard]] rhi::RasterStatus CreateReferenceComputeJob(const ReferenceComputeInputs& inputs,
                                                          ReferenceJob& inoutJob) noexcept;
/// Advance one GPU-compute stage of inoutJob, updating its read-back pixels and info.
[[nodiscard]] rhi::RasterStatus AdvanceReferenceCompute(const ReferenceComputePipelines& pipelines,
                                                        rhi::SurfaceHandle surface,
                                                        ReferenceJob& inoutJob) noexcept;
/// Resume a GPU-compute job after only its target changed; returns Ready when complete.
[[nodiscard]] rhi::RasterStatus ResumeReferenceCompute(ReferenceJob& inoutJob) noexcept;
[[nodiscard]] rhi::RasterStatus GenerateReferencePrimary(const ReferenceJob&, ReferencePath&, rhi::RayInput&) noexcept;
// Writes path diagnostics only to inoutInfo; the job itself is read-only.
[[nodiscard]] rhi::RasterStatus PrepareReferenceVertex(const ReferenceJob& job,
                                                       ReferenceCaptureInfo& inoutInfo,
                                                       ReferencePath& inoutPath,
                                                       const rhi::RayResult& hit) noexcept;
} // namespace ludus::graphics::renderer::internal
