#include <ludus/graphics/renderer/static_lighting.hpp>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/summation.hpp>
#include <ludus/foundation/threading/job_profiling.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/runtime/resources/scene.h>

#include <atomic>
#include <new>

#include "internal/reference_data.h"

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
namespace format = content::scene_format;
using Status = rhi::RasterStatus;
namespace threading = foundation::threading;
namespace
{
constexpr uint32 BAKE_RANDOM_DOMAIN = 0x42414B45; // Independent addressed BAKE stream.
constexpr usize TRIANGLE_CORNERS = 3;
constexpr float32 AFFINE_ERROR_ALLOWANCE = 1e-6F; // Conservative FP32 transformed-position allowance.
// Apple ARM64 cores use 128-byte cache lines; this also separates 64-byte x86 lines.
constexpr usize RANGE_RECORD_ALIGNMENT = 128;
struct ChartRecord final
{
    format::BakeChartLayout Layout;
    Array<uint8> Pixels;
    math::Vector3 Scale{};
    uint32 FirstTriangle = 0;
    uint64 SourceId = 0;
    math::Affine3 Transform;
    uint32 SkippedTriangleCount = 0;
};
/// Zero-area triangles are kept as empty chart tiles but carry no surface.
[[nodiscard]] bool IsCollapsed(const internal::ReferenceTriangle& triangle) noexcept
{
    return triangle.Area <= 0;
}
[[nodiscard]] uint64 GetElapsed(uint64 startTicks, uint64 endTicks) noexcept
{
    return endTicks > startTicks ? endTicks - startTicks : 0;
}
} // namespace
// Thanks to Brad Werth, "Holistic Task Parallelism for Common Game Architecture
// Patterns," Game Engine Gems 1, ch. 22, pp. 381-390, for coarse range
// decomposition over shared read-only input, and to Julien Hamaide, "Thread
// Communication Techniques," Game Engine Gems 2, ch. 29, pp. 459-467, for keeping
// worker outputs disjoint and aggregating them on one owner. Ludus departs by
// fixing the ranges before submission (no recursive splitting or waits) and by
// merging in range order after the JobSystem::Wait lifetime fence. See
// docs/architecture/threading.md#reference-review-and-resulting-changes.
struct StaticLightingBake::State final
{
    /// One triangle range job's results, read by the owner only after the Wait fence.
    struct alignas(RANGE_RECORD_ALIGNMENT) Range final
    {
        State* OwnerOrNull = nullptr;
        uint32 Index = 0;
        Status Result = Status::NotReady;
        bool HasRun = false;
        uint64 StartTicks = 0;
        uint64 EndTicks = 0;
        ReferenceCaptureInfo Info; // Private path diagnostics of this range.
    };
    /// Per-range progress, the only range state read while jobs run.
    struct alignas(RANGE_RECORD_ALIGNMENT) RangeProgress final
    {
        std::atomic<uint64> CompletedSamples{0};
    };
    StaticLightingBakeDescription Description;
    internal::ReferenceJob Job;
    Array<internal::ReferenceSurfaceData> Surfaces;
    Array<internal::ReferenceSum> Irradiance;
    Array<ChartRecord> Charts;
    uint32 Triangle = 0;
    uint32 Sample = 0;
    Status Progress = Status::Pending;
    // Scheduled mode. The prepare job and ranges reach State through context pointers.
    const resources::SceneDefinition* SceneOrNull = nullptr;
    threading::JobSystem* SystemOrNull = nullptr;
    Array<StaticLightingSurfaceDescription> SurfaceOverrides;
    Array<Range> Ranges;
    RangeProgress* ProgressOrNull = nullptr; // Owned; one slot per range.
    Array<threading::JobHandle> Handles;     // Preparation, then ranges in order.
    Status PrepareResult = Status::NotReady;
    uint64 SubmitTicks = 0;
    uint64 PrepareStartTicks = 0;
    uint64 PrepareEndTicks = 0;
    uint64 TotalSamples = 0;
    StaticLightingBakeProfile Profile;
    std::atomic<bool> ShouldStop{false};
    // Published by preparation with release order; read by GetDiagnostics while ranges run.
    std::atomic<bool> IsPrepared{false};
    std::atomic<uint32> SkippedTriangleCount{0};
    std::atomic<uint32> AffectedObjectCount{0};
    bool IsScheduled = false;
    bool IsInFlight = false;
    // Declared last so it is destroyed, and any submission drained, before its contexts.
    threading::JobGraph Graph;

    LUDUS_INLINE State() noexcept
    {
        Job.IsCpuOnly = true;
    }
    ~State() noexcept
    {
        // The owner drains every submission before destroying its contexts.
        LUDUS_REQUIRE(IsInFlight == false);
        delete[] ProgressOrNull;
    }
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    [[nodiscard]] Status Prepare(const resources::SceneDefinition& scene,
                                 const StaticLightingBakeDescription& description) noexcept;
    [[nodiscard]] Status EncodeCharts() noexcept;
    [[nodiscard]] Status Merge() noexcept;
    [[nodiscard]] Status Release(bool shouldHelp) noexcept;
    void RequestStop() noexcept;
    static threading::JobOutcome RunPrepare(void* context) noexcept;
    static threading::JobOutcome RunRange(void* context) noexcept;
};
StaticLightingBake::~StaticLightingBake() noexcept
{
    if (mStateOrNull != nullptr && mStateOrNull->IsInFlight == true)
    {
        mStateOrNull->RequestStop();
        threading::JobOutcome outcome = threading::JobOutcome::Pending;
        const threading::JobStatus waited = mStateOrNull->SystemOrNull->Wait(outcome);
        // A callback stack cannot drain itself; NotInitialized means Shutdown already drained it.
        LUDUS_REQUIRE(waited != threading::JobStatus::InsideJob);
        mStateOrNull->IsInFlight = false;
    }
    delete mStateOrNull;
}
Status StaticLightingBake::Initialize(const resources::SceneDefinition& scene,
                                      const StaticLightingBakeDescription& description) noexcept
{
    if (mStateOrNull != nullptr)
    {
        return Status::InvalidState;
    }
    core::UniquePtr<State> state;
    state.Reset(new (std::nothrow) State);
    if (state.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    const Status status = state->Prepare(scene, description);
    if (status != Status::Pending)
    {
        return status;
    }
    mStateOrNull = state.Release();
    return Status::Pending;
}
Status StaticLightingBake::State::Prepare(const resources::SceneDefinition& scene,
                                          const StaticLightingBakeDescription& description) noexcept
{
    if (description.SceneRevision == 0 || description.DependencyRevision == 0 || description.SamplesPerTriangle == 0 ||
        description.SamplesPerTriangle > 65536 || description.MaxDepth < 2 ||
        description.MaxDepth > LUDUS_REFERENCE_MAX_DEPTH || math::IsFinite(description.SecondaryDistance) == false ||
        description.SecondaryDistance <= 0 || description.SecondaryDistance > 2000000 ||
        scene.Objects.IsEmpty() == true || scene.Objects.GetSize() > format::IMPORT_LIMITS.Objects ||
        scene.Materials.GetSize() > format::IMPORT_LIMITS.Materials || scene.Lights.GetSize() > format::MAX_LIGHTS ||
        description.Surfaces.IsValid() == false ||
        (description.Surfaces.IsEmpty() == false && description.Surfaces.GetSize() != scene.Objects.GetSize()))
    {
        return Status::InvalidDescription;
    }
    usize triangleCount = 0;
    uint64 pixelBytes = 0;
    for (const resources::SceneObject& object : scene.Objects)
    {
        if (object.SourceId == 0 || object.Mesh >= scene.Meshes.GetSize() ||
            object.Material >= scene.Materials.GetSize())
        {
            return Status::InvalidDescription;
        }
        const resources::SceneMesh& mesh = scene.Meshes[object.Mesh];
        if (mesh.Indices.IsEmpty() == true || mesh.Indices.GetSize() > format::MAX_INDICES ||
            mesh.Indices.GetSize() % TRIANGLE_CORNERS != 0 || mesh.Normals.GetSize() != mesh.Positions.GetSize() ||
            mesh.Texcoords.GetSize() != mesh.Positions.GetSize())
        {
            return Status::InvalidDescription;
        }
        format::BakeChartLayout layout;
        const uint32 triangles = static_cast<uint32>(mesh.Indices.GetSize() / TRIANGLE_CORNERS);
        if (format::TryMakeBakeChartLayout(triangles, layout) == false)
        {
            return Status::CapacityExceeded;
        }
        triangleCount += triangles;
        pixelBytes += uint64(layout.Width) * layout.Height * format::TEXEL_BYTES;
    }
    const usize objectCount = scene.Objects.GetSize();
    const usize environmentCount = scene.Environment.Pixels.GetSize();
    // Peak includes temporary BVH scratch, the per-triangle index arrays and retained node capacity.
    const uint64 requiredBytes =
        sizeof(State) + pixelBytes +
        uint64(objectCount) *
            (sizeof(ChartRecord) + sizeof(internal::ReferenceSurfaceData) + sizeof(internal::ReferenceInstance)) +
        uint64(triangleCount) * (sizeof(internal::ReferenceTriangle) + sizeof(internal::ReferenceSum) +
                                 (internal::REFERENCE_TRIANGLE_INDEX_ARRAYS + 1) * sizeof(uint32) +
                                 2 * sizeof(internal::ReferenceBvhNode)) +
        sizeof(internal::EnvironmentData) + uint64(environmentCount) * (sizeof(math::Vector4) + sizeof(float64));
    if (triangleCount > ~uint32{0} / 2 || requiredBytes > description.CpuByteBudget)
    {
        return Status::CapacityExceeded;
    }
    State* state = this;
    state->Description = description;
    state->Description.Surfaces = {}; // All caller overrides are copied during this call, never retained.
    internal::ReferenceJob& job = state->Job;
    job.Description.MaxDepth = description.MaxDepth;
    job.Description.RouletteDepth = 3;
    job.Description.SecondaryDistance = description.SecondaryDistance;
    job.Description.Environment = scene.Environment.Rgb;
    if (math::TryMakeRandomKey(description.Seed, BAKE_RANDOM_DOMAIN, job.Key) != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    for (const resources::SceneLight& source : scene.Lights)
    {
        if (source.IsEnabled == false)
        {
            continue;
        }
        const float32 intensity =
            math::Max(source.IntensityRgb.X, math::Max(source.IntensityRgb.Y, source.IntensityRgb.Z));
        DirectLight& light = job.Lights[job.Description.LightCount++];
        light.SourceId = source.SourceId;
        light.Kind = static_cast<LightKind>(source.Kind);
        light.Color = intensity > 0 ? source.IntensityRgb / intensity : math::Vector3{};
        light.Intensity = intensity;
        light.Position = source.PositionWorld;
        light.Direction = source.DirectionWorld;
        light.Range = source.Range;
        light.InnerCos = source.InnerCos;
        light.OuterCos = source.OuterCos;
    }
    LightSelection normalized;
    const Status lightStatus = NormalizeDirectLights(job.Lights, job.Description.LightCount, normalized);
    if (lightStatus != Status::Ready)
    {
        return lightStatus;
    }
    for (usize i = 0; i < normalized.Count; ++i)
    {
        job.Lights[i] = normalized.Lights[i];
    }
    if (environmentCount != 0)
    {
        internal::EnvironmentData* environmentOrNull = new (std::nothrow) internal::EnvironmentData;
        if (environmentOrNull == nullptr)
        {
            return Status::OutOfMemory;
        }
        job.EnvironmentOrNull = environmentOrNull;
        environmentOrNull->IsCpuOnly = true;
        environmentOrNull->Width = scene.Environment.Width;
        environmentOrNull->Height = scene.Environment.Height;
        if (environmentOrNull->Width == 0 || environmentOrNull->Height == 0 ||
            usize(environmentOrNull->Width) * environmentOrNull->Height != environmentCount)
        {
            return Status::InvalidDescription;
        }
        if (environmentOrNull->Pixels.TryResize(environmentCount) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize i = 0; i < environmentCount; ++i)
        {
            const math::Vector3 radiance = math::Hadamard(scene.Environment.Pixels[i], scene.Environment.Rgb);
            if (internal::ReferenceColor(radiance, 1e12F) == false)
            {
                return Status::InvalidDescription;
            }
            environmentOrNull->Pixels[i] = {radiance.X, radiance.Y, radiance.Z, 1};
        }
        const Status environmentStatus = internal::BuildEnvironmentDistribution(*environmentOrNull);
        if (environmentStatus != Status::Ready)
        {
            return environmentStatus;
        }
    }
    if (state->Surfaces.TryResize(objectCount) == false || state->Charts.TryResize(objectCount) == false ||
        state->Irradiance.TryResize(triangleCount) == false || job.Scene.Instances.TryResize(objectCount) == false ||
        job.Scene.Triangles.TryResize(triangleCount) == false ||
        job.Scene.Emitters.TryEnsureCapacity(triangleCount) == false)
    {
        return Status::OutOfMemory;
    }
    job.Scene.InstanceCount = objectCount;
    uint32 triangleIndex = 0;
    for (usize objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        const resources::SceneObject& object = scene.Objects[objectIndex];
        const resources::SceneMaterial& material = scene.Materials[object.Material];
        const resources::SceneMesh& mesh = scene.Meshes[object.Mesh];
        internal::ReferenceInstance& instance = job.Scene.Instances[objectIndex];
        internal::ReferenceSurfaceData& surface = state->Surfaces[objectIndex];
        ChartRecord& chart = state->Charts[objectIndex];
        instance.Surface = &surface;
        instance.SourceId = object.SourceId;
        instance.FirstTriangle = triangleIndex;
        instance.TriangleCount = mesh.Indices.GetSize() / TRIANGLE_CORNERS;
        chart.FirstTriangle = triangleIndex;
        chart.SourceId = object.SourceId;
        chart.Transform = object.Transform;
        static_cast<void>(format::TryMakeBakeChartLayout(static_cast<uint32>(instance.TriangleCount), chart.Layout));
        if (chart.Pixels.TryResize(usize(chart.Layout.Width) * chart.Layout.Height * format::TEXEL_BYTES) == false)
        {
            return Status::OutOfMemory;
        }
        const bool hasOverride = description.Surfaces.IsEmpty() == false;
        const uint32 textureIndex = hasOverride == true ? description.Surfaces[objectIndex].Texture : material.Texture;
        surface.Bsdf = {{1, 1, 1}, material.Metallic, material.Roughness};
        surface.Emission = material.Emission;
        surface.IsEmissionTwoSided = material.IsEmissionTwoSided;
        surface.Sampler.Filter = rhi::RasterFilter::Linear;
        surface.Sampler.U = surface.Sampler.V =
            material.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        if (hasOverride == true)
        {
            const StaticLightingSurfaceDescription& override = description.Surfaces[objectIndex];
            if (internal::ReferenceColor({override.Tint.X, override.Tint.Y, override.Tint.Z}, 1) == false ||
                math::IsFinite(override.Tint.W) == false || override.Tint.W < 0 || override.Tint.W > 1 ||
                math::IsFinite(override.UvTransform) == false || math::Abs(override.UvTransform.X) > 1000000 ||
                math::Abs(override.UvTransform.Y) > 1000000 || math::Abs(override.UvTransform.Z) > 1000000 ||
                math::Abs(override.UvTransform.W) > 1000000 || math::IsFinite(override.Metallic) == false ||
                override.Metallic < 0 || override.Metallic > 1 || math::IsFinite(override.Roughness) == false ||
                override.Roughness < .045F || override.Roughness > 1 ||
                internal::ReferenceColor(override.Emission, 10000) == false ||
                math::IsFinite(override.AlphaCutoff) == false || override.AlphaCutoff < 0 || override.AlphaCutoff > 1 ||
                static_cast<uint8>(override.Alpha) > static_cast<uint8>(MaterialAlpha::Painter))
            {
                return Status::InvalidDescription;
            }
            if (override.Alpha == MaterialAlpha::Painter)
            {
                return Status::Unsupported;
            }
            surface.Bsdf = {{override.Tint.X, override.Tint.Y, override.Tint.Z}, override.Metallic, override.Roughness};
            surface.Tint = override.Tint;
            surface.UvTransform = override.UvTransform;
            surface.Emission = override.Emission;
            surface.IsEmissionTwoSided = override.IsEmissionTwoSided;
            surface.Alpha = override.Alpha;
            surface.AlphaCutoff = override.AlphaCutoff;
            surface.Sampler.U = surface.Sampler.V =
                override.ShouldRepeatTexture == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        }
        if (textureIndex != format::NO_TEXTURE)
        {
            if (textureIndex >= scene.Textures.GetSize())
            {
                return Status::InvalidDescription;
            }
            const resources::SceneTexture& texture = scene.Textures[textureIndex];
            instance.Texture = {texture.Pixels.GetData(),
                                texture.Pixels.GetSize(),
                                usize(texture.Width) * format::TEXEL_BYTES,
                                texture.Width,
                                texture.Height,
                                rhi::RasterFormat::Rgba8Srgb};
            surface.HasTexture = true;
        }
        else if (hasOverride == false)
        {
            surface.Bsdf.BaseColor = {};
        }
        math::Matrix3 normalMeshToWorld;
        if (math::TryNormalMatrix(object.Transform, {}, normalMeshToWorld) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        math::CompensatedSum objectArea;
        for (usize primitive = 0; primitive < instance.TriangleCount; ++primitive)
        {
            internal::ReferenceTriangle& triangle = job.Scene.Triangles[triangleIndex];
            triangle.Instance = static_cast<uint32>(objectIndex);
            triangle.Primitive = static_cast<uint32>(primitive);
            for (usize corner = 0; corner < TRIANGLE_CORNERS; ++corner)
            {
                const uint32 vertex = mesh.Indices[primitive * TRIANGLE_CORNERS + corner];
                if (vertex >= mesh.Positions.GetSize())
                {
                    return Status::InvalidDescription;
                }
                triangle.Positions[corner] = math::TransformPoint(object.Transform, mesh.Positions[vertex]);
                triangle.Normals[corner] = internal::ReferenceNormalize(normalMeshToWorld * mesh.Normals[vertex]);
                triangle.Uvs[corner] = mesh.Texcoords[vertex];
                if (mesh.BakeTexcoords.IsEmpty() == false)
                {
                    float32 expectedU = 0;
                    float32 expectedV = 0;
                    if (mesh.BakeTexcoords.GetSize() != mesh.Positions.GetSize() ||
                        format::TryGetBakeChartUv(chart.Layout,
                                                  static_cast<uint32>(primitive),
                                                  static_cast<uint32>(corner),
                                                  expectedU,
                                                  expectedV) == false ||
                        math::Abs(mesh.BakeTexcoords[vertex].X - expectedU) > 1e-6F ||
                        math::Abs(mesh.BakeTexcoords[vertex].Y - expectedV) > 1e-6F)
                    {
                        return Status::Unsupported;
                    }
                }
                triangle.Colors[corner] = {1, 1, 1, 1};
                const math::Vector3 positionWorld = triangle.Positions[corner];
                if (math::IsFinite(positionWorld) == false)
                {
                    return Status::InvalidDescription;
                }
                const math::Vector3 errorWorld =
                    internal::ComputeReferencePositionError(object.Transform, mesh.Positions[vertex]);
                triangle.Error = {math::Max(triangle.Error.X, errorWorld.X),
                                  math::Max(triangle.Error.Y, errorWorld.Y),
                                  math::Max(triangle.Error.Z, errorWorld.Z)};
            }
            const math::Vector3 edgeWorld1 = triangle.Positions[1] - triangle.Positions[0];
            const math::Vector3 edgeWorld2 = triangle.Positions[2] - triangle.Positions[0];
            triangle.Area = math::Length(math::Cross(edgeWorld1, edgeWorld2)) * .5F;
            const math::Vector3 normalMesh =
                math::Cross(mesh.Positions[mesh.Indices[primitive * TRIANGLE_CORNERS + 1]] -
                                mesh.Positions[mesh.Indices[primitive * TRIANGLE_CORNERS]],
                            mesh.Positions[mesh.Indices[primitive * TRIANGLE_CORNERS + 2]] -
                                mesh.Positions[mesh.Indices[primitive * TRIANGLE_CORNERS]]);
            triangle.GeometricNormal = internal::ReferenceNormalize(normalMeshToWorld * normalMesh);
            const float32 extent = math::Max(math::Length(edgeWorld1), math::Length(edgeWorld2));
            triangle.Error = triangle.Error + math::Vector3{extent, extent, extent} * (2 * AFFINE_ERROR_ALLOWANCE);
            if (math::IsFinite(triangle.Area) == false || math::IsFinite(triangle.GeometricNormal) == false)
            {
                return Status::InvalidDescription;
            }
            if (triangle.Area <= 0 || math::Length(triangle.GeometricNormal) == 0)
            {
                // Collapsed: no surface, power or normal. Keep its chart tile (zero
                // irradiance) so layout and bake coordinates stay valid, and leave it
                // out of emitter selection; the BVH and probes skip it by area.
                triangle.Area = 0;
                ++chart.SkippedTriangleCount;
                ++triangleIndex;
                continue;
            }
            objectArea.Add(float64(triangle.Area));
            if ((surface.Emission.X > 0 || surface.Emission.Y > 0 || surface.Emission.Z > 0) &&
                job.Scene.Emitters.TryAdd(triangleIndex) == false)
            {
                return Status::OutOfMemory;
            }
            ++triangleIndex;
        }
        if (hasOverride == false && material.HasEmissionPower == true)
        {
            if (math::IsFinite(objectArea.GetResult()) == false)
            {
                return Status::CapacityExceeded;
            }
            const float32 luminance = math::Dot(material.Emission, math::Vector3{.2126F, .7152F, .0722F});
            if (objectArea.GetResult() <= 0)
            {
                // Every triangle collapsed: there is no surface to emit the authored power.
                surface.Emission = {};
            }
            else if (luminance > 0)
            {
                surface.Emission =
                    material.Emission *
                    float32(float64(material.EmissionPower) / (float64(luminance) * math::kPi * objectArea.GetResult() *
                                                               (material.IsEmissionTwoSided == true ? 2.0 : 1.0)));
            }
        }
        if (internal::ReferenceColor(surface.Emission, 10000) == false)
        {
            return Status::CapacityExceeded;
        }
    }
    const Status bvhStatus = internal::BuildReferenceBvh(job.Scene);
    if (bvhStatus != Status::Ready)
    {
        return bvhStatus;
    }
    const Status selectionStatus = internal::BuildReferenceEmitterSelection(job.Scene);
    if (selectionStatus != Status::Ready)
    {
        return selectionStatus;
    }
    uint32 skippedTriangles = 0;
    uint32 affectedObjects = 0;
    for (const ChartRecord& chart : state->Charts)
    {
        skippedTriangles += chart.SkippedTriangleCount;
        affectedObjects += chart.SkippedTriangleCount != 0 ? 1U : 0U;
    }
    SkippedTriangleCount.store(skippedTriangles, std::memory_order_relaxed);
    AffectedObjectCount.store(affectedObjects, std::memory_order_relaxed);
    IsPrepared.store(true, std::memory_order_release);
    return Status::Pending;
}
Status StaticLightingBake::Step(uint32 maxPathSamples) noexcept
{
    if (mStateOrNull == nullptr || maxPathSamples == 0)
    {
        return Status::InvalidState;
    }
    State& state = *mStateOrNull;
    if (state.IsScheduled == true)
    {
        return Status::InvalidState;
    }
    if (state.Progress != Status::Pending)
    {
        return state.Progress;
    }
    for (uint32 i = 0; i < maxPathSamples && state.Triangle < state.Job.Scene.Triangles.GetSize(); ++i)
    {
        if (IsCollapsed(state.Job.Scene.Triangles[state.Triangle]) == true)
        {
            // No surface to gather about; its tile keeps zero irradiance.
            state.Sample = 0;
            ++state.Triangle;
            continue;
        }
        math::Vector3 irradiance;
        const Status status =
            internal::EvaluateReferenceDiffuseProbe(state.Job,
                                                    state.Job.Info,
                                                    { .TriangleIndex = state.Triangle, .SampleIndex = state.Sample },
                                                    irradiance);
        if (status != Status::Ready)
        {
            state.Progress = status;
            return status;
        }
        state.Irradiance[state.Triangle].Add(irradiance);
        ++state.Sample;
        if (state.Sample == state.Description.SamplesPerTriangle)
        {
            state.Sample = 0;
            ++state.Triangle;
        }
    }
    if (state.Triangle < state.Job.Scene.Triangles.GetSize())
    {
        return Status::Pending;
    }
    state.Progress = state.EncodeCharts();
    return state.Progress;
}
Status StaticLightingBake::State::EncodeCharts() noexcept
{
    State& state = *this;
    for (ChartRecord& chart : state.Charts)
    {
        for (usize i = 0; i < chart.Layout.TriangleCount; ++i)
        {
            const math::Vector3 mean =
                state.Irradiance[chart.FirstTriangle + i].Value / float32(state.Description.SamplesPerTriangle);
            if (internal::ReferenceColor(mean, 10000) == false)
            {
                return math::IsFinite(mean) == true ? Status::CapacityExceeded : Status::Failed;
            }
            chart.Scale = {math::Max(chart.Scale.X, mean.X),
                           math::Max(chart.Scale.Y, mean.Y),
                           math::Max(chart.Scale.Z, mean.Z)};
        }
        if (internal::ReferenceColor(chart.Scale, 10000) == false)
        {
            return Status::CapacityExceeded;
        }
        constexpr float32 MIN_IRRADIANCE_SCALE =
            1e-6F; // Avoid zero-channel division without losing weak illumination to a unit-scale floor.
        chart.Scale = {math::Max(chart.Scale.X, MIN_IRRADIANCE_SCALE),
                       math::Max(chart.Scale.Y, MIN_IRRADIANCE_SCALE),
                       math::Max(chart.Scale.Z, MIN_IRRADIANCE_SCALE)};
        const uint32 columns = chart.Layout.Width / format::BAKE_CHART_TILE_PIXELS;
        for (usize i = 0; i < chart.Layout.TriangleCount; ++i)
        {
            const math::Vector3 mean =
                state.Irradiance[chart.FirstTriangle + i].Value / float32(state.Description.SamplesPerTriangle);
            const StaticArray<uint8, format::TEXEL_BYTES> rgba{
                static_cast<uint8>(math::Clamp(mean.X / chart.Scale.X * 255.F + .5F, 0.F, 255.F)),
                static_cast<uint8>(math::Clamp(mean.Y / chart.Scale.Y * 255.F + .5F, 0.F, 255.F)),
                static_cast<uint8>(math::Clamp(mean.Z / chart.Scale.Z * 255.F + .5F, 0.F, 255.F)),
                255};
            const usize tileX = (i % columns) * format::BAKE_CHART_TILE_PIXELS;
            const usize tileY = (i / columns) * format::BAKE_CHART_TILE_PIXELS;
            for (usize y = 0; y < format::BAKE_CHART_TILE_PIXELS; ++y)
            {
                for (usize x = 0; x < format::BAKE_CHART_TILE_PIXELS; ++x)
                {
                    const usize offset = ((tileY + y) * chart.Layout.Width + tileX + x) * format::TEXEL_BYTES;
                    for (usize channel = 0; channel < format::TEXEL_BYTES; ++channel)
                    {
                        chart.Pixels[offset + channel] = rgba[channel];
                    }
                }
            }
        }
    }
    return Status::Ready;
}
Status StaticLightingBake::GetChart(usize objectIndex, StaticLightingChart& outChart) const noexcept
{
    if (mStateOrNull == nullptr)
    {
        return Status::InvalidState;
    }
    if (mStateOrNull->Progress != Status::Ready)
    {
        return mStateOrNull->Progress == Status::Pending ? Status::NotReady : mStateOrNull->Progress;
    }
    if (objectIndex >= mStateOrNull->Charts.GetSize())
    {
        return Status::InvalidDescription;
    }
    const ChartRecord& chart = mStateOrNull->Charts[objectIndex];
    outChart =
    {
        .Width = chart.Layout.Width,
        .Height = chart.Layout.Height,
        .Pixels = {chart.Pixels.GetData(), chart.Pixels.GetSize()},
        .Scale = chart.Scale,
        .UvTransform = {1, 1, 0, 0},
        .SourceId = chart.SourceId,
        .SceneRevision = mStateOrNull->Description.SceneRevision,
        .DependencyRevision = mStateOrNull->Description.DependencyRevision,
        .Transform = chart.Transform,
        .SkippedTriangleCount = chart.SkippedTriangleCount,
    };
    return Status::Ready;
}
Status StaticLightingBake::Submit(threading::JobSystem& inoutSystem,
                                  const resources::SceneDefinition& scene,
                                  const StaticLightingBakeDescription& description,
                                  const StaticLightingScheduleDescription& schedule) noexcept
{
    if (mStateOrNull != nullptr)
    {
        return Status::InvalidState;
    }
    const uint32 rangeCount = schedule.RangeCount;
    if (rangeCount == 0 || rangeCount > MAX_STATIC_LIGHTING_RANGES || description.Surfaces.IsValid() == false)
    {
        return Status::InvalidDescription;
    }
    const uint64 submitTicks = time::NowTicks();
    core::UniquePtr<State> state;
    state.Reset(new (std::nothrow) State);
    if (state.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    state->ProgressOrNull = new (std::nothrow) State::RangeProgress[rangeCount];
    if (state->ProgressOrNull == nullptr || state->Ranges.TryResize(rangeCount) == false ||
        state->Handles.TryEnsureCapacity(usize{rangeCount} + 1) == false ||
        (description.Surfaces.IsEmpty() == false &&
         state->SurfaceOverrides.TryAddRange({description.Surfaces.GetDataOrNull(), description.Surfaces.GetSize()}) ==
             false))
    {
        return Status::OutOfMemory;
    }
    state->Description = description;
    state->Description.Surfaces = {state->SurfaceOverrides.GetData(), state->SurfaceOverrides.GetSize()};
    state->SceneOrNull = &scene;
    state->SystemOrNull = &inoutSystem;
    state->SubmitTicks = submitTicks;
    state->IsScheduled = true;
    uint64 triangleCount = 0;
    for (const resources::SceneObject& object : scene.Objects)
    {
        if (object.Mesh < scene.Meshes.GetSize())
        {
            triangleCount += scene.Meshes[object.Mesh].Indices.GetSize() / TRIANGLE_CORNERS;
        }
    }
    state->TotalSamples = triangleCount * description.SamplesPerTriangle;
    threading::JobGraph& graph = state->Graph;
    threading::JobHandle prepare;
    // Labels name the jobs in profiler traces of the system (see job_profiling.hpp).
    const threading::JobLabel prepareLabel = LUDUS_JOB_LABEL(StaticLightingPrepare);
    const threading::JobLabel rangeLabel = LUDUS_JOB_LABEL(StaticLightingRange);
    if (graph.Initialize({ .MaxJobs = rangeCount + 1, .MaxDependencies = rangeCount }) != threading::JobStatus::Ok ||
        graph.Add(&State::RunPrepare, state.Get(), prepare, prepareLabel) != threading::JobStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    LUDUS_REQUIRE(state->Handles.TryAdd(prepare) == true);
    for (uint32 i = 0; i < rangeCount; ++i)
    {
        State::Range& range = state->Ranges[i];
        range.OwnerOrNull = state.Get();
        range.Index = i;
        threading::JobHandle handle;
        // Capacity was reserved above, so neither call can fail for a valid graph.
        LUDUS_REQUIRE(graph.Add(&State::RunRange, &range, handle, rangeLabel) == threading::JobStatus::Ok);
        LUDUS_REQUIRE(graph.DependsOn(handle, prepare) == threading::JobStatus::Ok);
        LUDUS_REQUIRE(state->Handles.TryAdd(handle) == true);
    }
    LUDUS_REQUIRE(graph.Seal() == threading::JobStatus::Ok);
    // Mark in flight first: the serial executor runs every job inside Submit.
    state->IsInFlight = true;
    if (inoutSystem.Submit(graph) != threading::JobStatus::Ok)
    {
        state->IsInFlight = false;
        return Status::InvalidState;
    }
    mStateOrNull = state.Release();
    return Status::Pending;
}
Status StaticLightingBake::Poll() noexcept
{
    if (mStateOrNull == nullptr || mStateOrNull->IsScheduled == false)
    {
        return Status::InvalidState;
    }
    State& state = *mStateOrNull;
    if (state.IsInFlight == false)
    {
        return state.Progress;
    }
    // GetOutcome reads only this graph, so another system state cannot be mistaken
    // for it. Once every job is resolved, Wait releases the graph without blocking.
    for (const threading::JobHandle handle : state.Handles)
    {
        threading::JobOutcome outcome = threading::JobOutcome::Pending;
        LUDUS_REQUIRE(state.Graph.GetOutcome(handle, outcome) == threading::JobStatus::Ok);
        if (outcome == threading::JobOutcome::Pending || outcome == threading::JobOutcome::Running)
        {
            return Status::Pending;
        }
    }
    return state.Release(false);
}
Status StaticLightingBake::Finish() noexcept
{
    if (mStateOrNull == nullptr || mStateOrNull->IsScheduled == false)
    {
        return Status::InvalidState;
    }
    if (mStateOrNull->IsInFlight == false)
    {
        return mStateOrNull->Progress;
    }
    return mStateOrNull->Release(true);
}
void StaticLightingBake::Cancel() noexcept
{
    if (mStateOrNull != nullptr && mStateOrNull->IsInFlight == true)
    {
        mStateOrNull->RequestStop();
    }
}
void StaticLightingBake::GetProgress(StaticLightingBakeProgress& outProgress) const noexcept
{
    outProgress = {};
    if (mStateOrNull == nullptr)
    {
        return;
    }
    const State& state = *mStateOrNull;
    if (state.IsScheduled == true)
    {
        for (usize i = 0; i < state.Ranges.GetSize(); ++i)
        {
            outProgress.CompletedSamples += state.ProgressOrNull[i].CompletedSamples.load(std::memory_order_relaxed);
        }
        outProgress.TotalSamples = state.TotalSamples;
        return;
    }
    const uint64 samples = state.Description.SamplesPerTriangle;
    outProgress.CompletedSamples = uint64(state.Triangle) * samples + state.Sample;
    outProgress.TotalSamples = uint64(state.Job.Scene.Triangles.GetSize()) * samples;
}
Status StaticLightingBake::GetProfile(StaticLightingBakeProfile& outProfile) const noexcept
{
    if (mStateOrNull == nullptr || mStateOrNull->IsScheduled == false)
    {
        return Status::InvalidState;
    }
    if (mStateOrNull->IsInFlight == true)
    {
        return Status::Pending;
    }
    outProfile = mStateOrNull->Profile;
    return Status::Ready;
}
Status StaticLightingBake::GetDiagnostics(StaticLightingBakeDiagnostics& outDiagnostics) const noexcept
{
    if (mStateOrNull == nullptr)
    {
        return Status::InvalidState;
    }
    const State& state = *mStateOrNull;
    if (state.IsPrepared.load(std::memory_order_acquire) == false)
    {
        // Serial preparation completes inside Initialize, so only a scheduled one can be unresolved.
        return state.IsInFlight == true ? Status::Pending : Status::NotReady;
    }
    outDiagnostics =
    {
        .SkippedTriangleCount = state.SkippedTriangleCount.load(std::memory_order_relaxed),
        .AffectedObjectCount = state.AffectedObjectCount.load(std::memory_order_relaxed),
    };
    return Status::Ready;
}
Status StaticLightingBake::GetSkippedTriangleCount(usize objectIndex, uint32& outSkippedTriangleCount) const noexcept
{
    StaticLightingBakeDiagnostics diagnostics;
    const Status prepared = GetDiagnostics(diagnostics);
    if (prepared != Status::Ready)
    {
        return prepared;
    }
    // Preparation wrote the per-object counts before its release store; ranges never touch them.
    if (objectIndex >= mStateOrNull->Charts.GetSize())
    {
        return Status::InvalidDescription;
    }
    outSkippedTriangleCount = mStateOrNull->Charts[objectIndex].SkippedTriangleCount;
    return Status::Ready;
}
void StaticLightingBake::State::RequestStop() noexcept
{
    ShouldStop.store(true, std::memory_order_relaxed);
    // Skips jobs that have not started; NotInitialized after Shutdown drained the graph.
    static_cast<void>(SystemOrNull->Cancel());
}
threading::JobOutcome StaticLightingBake::State::RunPrepare(void* context) noexcept
{
    State& state = *static_cast<State*>(context);
    state.PrepareStartTicks = time::NowTicks();
    if (state.ShouldStop.load(std::memory_order_relaxed) == true)
    {
        state.PrepareEndTicks = state.PrepareStartTicks;
        return threading::JobOutcome::Cancelled;
    }
    // Prepare overwrites Description, so pass a copy that still views the owned overrides.
    const StaticLightingBakeDescription description = state.Description;
    state.PrepareResult = state.Prepare(*state.SceneOrNull, description);
    state.PrepareEndTicks = time::NowTicks();
    return state.PrepareResult == Status::Pending ? threading::JobOutcome::Succeeded : threading::JobOutcome::Failed;
}
threading::JobOutcome StaticLightingBake::State::RunRange(void* context) noexcept
{
    Range& range = *static_cast<Range*>(context);
    State& state = *range.OwnerOrNull;
    range.HasRun = true;
    range.StartTicks = time::NowTicks();
    // Preparation published the immutable job through the graph edge. Each range
    // reads it concurrently and writes only Irradiance[first, last).
    const uint64 triangleCount = state.Job.Scene.Triangles.GetSize();
    const uint64 rangeCount = state.Ranges.GetSize();
    const uint32 first = static_cast<uint32>(triangleCount * range.Index / rangeCount);
    const uint32 last = static_cast<uint32>(triangleCount * (uint64(range.Index) + 1) / rangeCount);
    const uint32 samples = state.Description.SamplesPerTriangle;
    std::atomic<uint64>& completed = state.ProgressOrNull[range.Index].CompletedSamples;
    threading::JobOutcome outcome = threading::JobOutcome::Succeeded;
    range.Result = Status::Ready;
    for (uint32 triangle = first; triangle < last && outcome == threading::JobOutcome::Succeeded; ++triangle)
    {
        if (IsCollapsed(state.Job.Scene.Triangles[triangle]) == true)
        {
            // Skipped as in Step; count its samples so progress still reaches the total.
            completed.store(completed.load(std::memory_order_relaxed) + samples, std::memory_order_relaxed);
            continue;
        }
        // Same per-triangle accumulation order as Step, so results are bit-identical.
        internal::ReferenceSum sum;
        for (uint32 sample = 0; sample < samples; ++sample)
        {
            if (state.ShouldStop.load(std::memory_order_relaxed) == true)
            {
                range.Result = Status::NotReady;
                outcome = threading::JobOutcome::Cancelled;
                break;
            }
            math::Vector3 irradiance;
            const Status status =
                internal::EvaluateReferenceDiffuseProbe(state.Job,
                                                        range.Info,
                                                        { .TriangleIndex = triangle, .SampleIndex = sample },
                                                        irradiance);
            if (status != Status::Ready)
            {
                // Stop sibling ranges early; Merge reports this failure over their cancellation.
                range.Result = status;
                outcome = threading::JobOutcome::Failed;
                state.RequestStop();
                break;
            }
            sum.Add(irradiance);
        }
        if (outcome == threading::JobOutcome::Succeeded)
        {
            state.Irradiance[triangle] = sum;
            completed.store(completed.load(std::memory_order_relaxed) + samples, std::memory_order_relaxed);
        }
    }
    range.EndTicks = time::NowTicks();
    return outcome;
}
Status StaticLightingBake::State::Release(bool shouldHelp) noexcept
{
    threading::JobOutcome outcome = threading::JobOutcome::Pending;
    const threading::JobStatus waited = SystemOrNull->Wait(outcome, shouldHelp);
    if (waited == threading::JobStatus::InsideJob)
    {
        return Status::InvalidState;
    }
    // Ok releases the graph. NotInitialized or InvalidArgument means Shutdown already
    // drained and released it. Either way every callback write is now visible.
    IsInFlight = false;
    const uint64 mergeStartTicks = time::NowTicks();
    Progress = Merge();
    Profile.MergeNanoseconds = GetElapsed(mergeStartTicks, time::NowTicks());
    return Progress;
}
Status StaticLightingBake::State::Merge() noexcept
{
    Profile = {};
    Profile.RangeCount = static_cast<uint32>(Ranges.GetSize());
    Profile.PrepareNanoseconds = GetElapsed(PrepareStartTicks, PrepareEndTicks);
    uint64 lastEndTicks = PrepareEndTicks;
    uint64 firstStartTicks = ~uint64{0};
    bool isCancelled = PrepareResult == Status::NotReady;
    Status failure =
        PrepareResult == Status::Pending || PrepareResult == Status::NotReady ? Status::Ready : PrepareResult;
    // Stable range order: the first failure wins, and failure dominates cancellation.
    for (const Range& range : Ranges)
    {
        if (range.HasRun == true)
        {
            const uint64 elapsed = GetElapsed(range.StartTicks, range.EndTicks);
            ++Profile.ExecutedRangeCount;
            Profile.RangeNanoseconds += elapsed;
            Profile.LongestRangeNanoseconds = math::Max(Profile.LongestRangeNanoseconds, elapsed);
            firstStartTicks = math::Min(firstStartTicks, range.StartTicks);
            lastEndTicks = math::Max(lastEndTicks, range.EndTicks);
        }
        if (range.Result == Status::NotReady)
        {
            isCancelled = true;
        }
        else if (range.Result != Status::Ready && failure == Status::Ready)
        {
            failure = range.Result;
        }
    }
    Profile.DispatchNanoseconds = Profile.ExecutedRangeCount == 0 ? 0 : GetElapsed(PrepareEndTicks, firstStartTicks);
    Profile.ExecutionNanoseconds = GetElapsed(SubmitTicks, lastEndTicks);
    if (failure != Status::Ready)
    {
        return failure;
    }
    if (isCancelled == true)
    {
        return Status::NotReady;
    }
    return EncodeCharts();
}
} // namespace ludus::graphics::renderer
