// The source build and the relocated SDK consumer exercise this same public-API replay.
#include "../../../../tests/sdk_consumer/directional_near/doorway.hpp"
#include "renderer_composite.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdio>
#include <cstdlib>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/measurement.h>

#include <fstream>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
using Status = rhi::RasterStatus;
using namespace ludus::testing::directional;
template <class Poll>
Status Wait(Status status, Poll poll)
{
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < time::Seconds(30).Nanoseconds)
    {
        threading::SleepFor(time::Milliseconds(1));
        status = poll();
    }
    return status;
}
// Intersect the rays at adjacent light-map texel centres with the flat receiver, z=0.
math::Vector2 ProjectTexelOntoGround(const renderer::DirectionalCascadeReport& map, math::Vector3 lightWorld)
{
    const math::Matrix4 rows = math::Transpose(map.WorldToClip);
    const auto project = [&](math::Vector4 row, float32 metres) {
        math::Vector3 axis;
        REQUIRE(math::TryNormalize({row.X, row.Y, row.Z}, axis) == math::MathStatus::Success);
        return metres * math::Length(axis - lightWorld * (axis.Z / lightWorld.Z));
    };
    return {project(rows.Columns[0], map.MetresPerTexel.X), project(rows.Columns[1], map.MetresPerTexel.Y)};
}
struct DeviceSession final
{
    ~DeviceSession()
    {
        rhi::Shutdown();
    }
};
} // namespace

TEST_CASE("Near directional shadows preserve the doorway across camera updates",
          "[render-graph][directional-near-replay][directional-shadows][gpu]")
{
    enum class ReplayMode
    {
        Near,
        WholeScene,
        Constrained
    };
    const ReplayMode mode = GENERATE(ReplayMode::Near, ReplayMode::WholeScene, ReplayMode::Constrained);
    const bool isWholeSceneControl = mode != ReplayMode::Near;
    const bool shouldMeasure =
        std::getenv("LUDUS_MEASURE_DIRECTIONAL_REPLAY") != nullptr && mode != ReplayMode::Constrained;
    const char* capturePathOrNull = std::getenv("LUDUS_DIRECTIONAL_CAPTURE");
    std::ofstream capture;
    if (capturePathOrNull != nullptr && isWholeSceneControl == false)
    {
        capture.open(capturePathOrNull, std::ios::binary);
        REQUIRE(capture.good() == true);
    }
    DeviceSession session;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    if (mode == ReplayMode::Constrained)
    {
        // Fits the original 32 MiB attachments and the remaining fixture, but not the 40 MiB atlas.
        description.Storage.ResourceBytes = uint64{38} * 1024 * 1024;
    }
    description.EnableGpuTiming = shouldMeasure;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, device, surface, &failure);
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No GPU adapter on this host");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    filesystem::Directory directory;
    REQUIRE(directory.Open(LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead("rg2_clustered_Default.lurgrt", file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    const filesystem::ReadResult read = file.ReadAt(0, MakeSpan(bytes));
    REQUIRE(read.Outcome.Succeeded() == true);
    REQUIRE(read.BytesRead == bytes.GetSize());
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    renderer::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                 ludus::shaders::renderer_composite::Fragment());
    REQUIRE(programs.HasDirectionalNearCascades == true);
    programs.HasDirectionalNearCascades = mode != ReplayMode::WholeScene;
    renderer::RenderGraph graph;
    REQUIRE(Wait(graph.Initialize(device, package.GetPlan(), programs, nullptr, WIDTH, HEIGHT),
                 [&] { return graph.Poll(); }) == Status::Ready);
    StaticArray<uint8, WIDTH * HEIGHT * CHANNELS> pixels{};

    uint64 revision = 0;
    uint32 width = WIDTH;
    uint32 height = HEIGHT;
    float64 gpuNanoseconds = 0;
    float64 resolveNanoseconds = 0;
    uint64 submissionNanoseconds = 0;
    const auto render = [&] {
        if (shouldMeasure == true)
        {
            REQUIRE(rhi::ArmGpuTiming(device) == Status::Ready);
            REQUIRE(rhi::ArmGpuPassTimings(device) == Status::Ready);
        }
        const uint64 start = time::NowTicks();
        rhi::SubmissionToken completion;
        REQUIRE(graph.Render(surface, WIDTH, HEIGHT, completion) == Status::Ready);
        REQUIRE(Wait(rhi::GetStatus(device, completion), [&] { return rhi::GetStatus(device, completion); }) ==
                Status::Ready);
        submissionNanoseconds = time::NowTicks() - start;
        if (shouldMeasure == true)
        {
            rhi::GpuTiming timing;
            REQUIRE(rhi::GetGpuTiming(device, timing) == Status::Ready);
            REQUIRE(timing.State == rhi::GpuTimingState::Available);
            // The RHI reports float64 nanoseconds; the CSV writer truncates to whole nanoseconds.
            gpuNanoseconds = timing.DurationNanoseconds;
            rhi::GpuPassTimings passes;
            REQUIRE(rhi::GetGpuPassTimings(device, passes) == Status::Ready);
            resolveNanoseconds = 0;
            // On a warm frame this linear graph has precisely the G-buffer and fullscreen resolve.
            // Metal labels both physical intervals with the enclosing graph pass.
            if (passes.Count == 2 && passes.DroppedPasses == 0 &&
                passes.Passes[1].Timing.State == rhi::GpuTimingState::Available)
            {
                resolveNanoseconds = passes.Passes[1].Timing.DurationNanoseconds;
            }
        }
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect;
        REQUIRE(graph.GetExport(0, texture, aspect) == Status::Ready);
        rhi::ReadbackTicket ticket;
        REQUIRE(Wait(rhi::RequestTextureReadback(device,
                                                 texture,
                                                 { .Width = width, .Height = height },
                                                 WIDTH * CHANNELS,
                                                 ticket),
                     [&] { return rhi::GetStatus(device, ticket); }) == Status::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, pixels.GetData(), pixels.GetSize()) == Status::Ready);
        REQUIRE(rhi::Release(device, ticket) == Status::Ready);
    };
    renderer::ViewDescription camera;
    StaticArray<renderer::RenderGraphMesh, 2> meshes{};
    StaticArray<renderer::RenderGraphItem, 2> items{};
    const uint32 materialIndex = 0;
    const uint32 casterMaterialIndex = 1;
    StaticArray<renderer::SceneShaderMaterial, 2> materials{};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    DoorwayGeometry geometry;
    geometry.Describe(meshes);
    items[0].SourceId = 1;
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    meshes[0].MaterialBindings = {&materialIndex, 1};
    meshes[1].MaterialBindings = {&casterMaterialIndex, 1};

    StaticArray<renderer::DirectLight, 2> lights{};
    lights[0].SourceId = 9;
    lights[0].Direction = {-1, 0, -1};
    lights[0].Intensity = 2;
    lights[1] = lights[0];
    lights[1].SourceId = 1;
    lights[1].Direction = {1, 0, -1};
    inputs.Lights = {lights.GetData(), lights.GetSize()};
    uint64 contentRevision = 1;
    uint64 previousContent = 0;
    bool isCasterOpaque = true;
    float32 eyeY = -3;
    float32 eyeHeight = 2;
    const auto sample = [&](float32 y, usize light, float32 eyeOffset, bool shouldCheckOracle = true) {
        const usize center = (height / 2 * WIDTH + width / 2) * CHANNELS;
        const math::Vector3 receiverWorld{0, y, 0};
        const math::Vector3 eyeWorld{eyeOffset, y + eyeY, eyeHeight};
        AimCamera(eyeWorld, receiverWorld, camera, width, height);
        camera.LightingView = renderer::DeferredLightingView::ShadowCoverage;
        camera.LightingViewSourceId = lights[light].SourceId;
        renderer::RenderGraphReport report;
        const bool isFirstUpdate = previousContent != contentRevision;
        previousContent = contentRevision;
        const uint64 start = time::NowTicks();
        const uint64 previousDrawable = graph.GetDrawableSceneRevision();
        const Status updated = graph.Update({meshes.GetData(), isFirstUpdate ? 2U : 0U},
                                            {items.GetData(), isFirstUpdate ? 2U : 0U},
                                            {&camera, 1},
                                            ++revision,
                                            renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                            report,
                                            isFirstUpdate == true ? &inputs : nullptr,
                                            contentRevision);
        if (updated == Status::Pending)
        {
            CHECK(graph.GetDrawableSceneRevision() == previousDrawable);
        }
        if (shouldMeasure == true)
        {
            rhi::AllocationTelemetry pending;
            REQUIRE(rhi::GetAllocationTelemetry(device, pending) == Status::Ready);
            std::printf("stage,%d,%llu,%llu,%llu\n",
                        int(isWholeSceneControl),
                        static_cast<unsigned long long>(revision),
                        static_cast<unsigned long long>(pending.ApiBytes),
                        static_cast<unsigned long long>(pending.PeakObservedApiBytes));
        }
        REQUIRE(Wait(updated, [&] { return graph.Poll(); }) == Status::Ready);
        CHECK(graph.GetDrawableSceneRevision() == revision);
        const uint64 staged = time::NowTicks() - start;
        render();
        const renderer::RenderGraphSceneResourceReport resources = graph.GetReport().Resources;
        REQUIRE(resources.DirectionalNearRegions == (isWholeSceneControl ? 0 : inputs.Lights.GetSize()));
        if (revision == 1)
        {
            const renderer::SkyVisibilityReport sky = graph.GetSkyVisibilityReport();
            CHECK(sky.WholeWorldMaps == renderer::SKY_VISIBILITY_DIRECTION_COUNT);
            CHECK(sky.LocalMaps == renderer::SKY_VISIBILITY_DIRECTION_COUNT);
            const uint64 expectedMiB = isWholeSceneControl == true ? 32 : 40;
            CHECK(resources.DrawableShadowAttachmentBytes == expectedMiB * 1024 * 1024);
        }
        for (const renderer::DirectionalLightShadowReport& source : resources.DrawableDirectionalShadows)
        {
            if (source.SourceId == 0)
            {
                continue;
            }
            if (shouldMeasure == true && source.SourceId == lights[light].SourceId)
            {
                const math::Vector3 directionWorld = lights[light].Direction;
                const math::Vector2 whole = ProjectTexelOntoGround(source.WholeScene, directionWorld);
                const math::Vector2 near = source.Near.Interior.Width == 0
                                               ? math::Vector2{}
                                               : ProjectTexelOntoGround(source.Near, directionWorld);
                std::printf("footprint,%d,%llu,%llu,%.7f,%.7f,%.7f,%.7f,%u,%u,%llu,%llu\n",
                            int(isWholeSceneControl),
                            static_cast<unsigned long long>(revision),
                            static_cast<unsigned long long>(source.SourceId),
                            double(near.X),
                            double(near.Y),
                            double(whole.X),
                            double(whole.Y),
                            source.Near.Draws,
                            source.WholeScene.Draws,
                            static_cast<unsigned long long>(source.Near.Triangles),
                            static_cast<unsigned long long>(source.WholeScene.Triangles));
            }
            if (isWholeSceneControl == true)
            {
                CHECK(source.Near.Interior.Width == 0);
                continue;
            }
            CHECK(source.Near.MetresPerTexel.X <= 0.10F);
            CHECK(source.Near.MetresPerTexel.Y <= 0.10F);
            CHECK(source.Near.Draws > 0);
            CHECK(math::Max(source.WholeScene.MetresPerTexel.X, source.WholeScene.MetresPerTexel.Y) > 1);
        }
        if (capture.is_open() == true && revision <= 24)
        {
            capture.write(reinterpret_cast<const char*>(pixels.GetData()),
                          static_cast<std::streamsize>(pixels.GetSize()));
            REQUIRE(capture.good() == true);
        }
        CAPTURE(light, y, eyeOffset, int(pixels[center]), int(pixels[center + 1]), int(pixels[center + 2]));
        CHECK(pixels[center] == pixels[center + 1]);
        CHECK(pixels[center] == pixels[center + 2]);
        // Analytical ray/quad intersections in World metres, independent of either map.
        const math::Vector3 towardLightWorld = -lights[light].Direction;
        const math::Vector3 casterTranslation = items[1].MeshToWorld.Translation;
        const float32 wallDepth = (WALL_X + casterTranslation.X) / towardLightWorld.X;
        const float32 hitHeight = wallDepth * towardLightWorld.Z;
        const float32 roofX =
            (WALL_TOP + casterTranslation.Z) * towardLightWorld.X / towardLightWorld.Z - casterTranslation.X;
        const float32 relativeY = y - casterTranslation.Y;
        const bool isWallHit = wallDepth > 0 && hitHeight >= casterTranslation.Z &&
                               hitHeight <= WALL_TOP + casterTranslation.Z && math::Abs(relativeY) > OPENING_HALF_WIDTH;
        const bool isBlocked = isCasterOpaque == true && math::Abs(relativeY) <= 4 &&
                               (isWallHit == true || (roofX >= 1.75F && roofX <= 2.25F));
        if (shouldMeasure == true)
        {
            rhi::AllocationTelemetry allocations;
            REQUIRE(rhi::GetAllocationTelemetry(device, allocations) == Status::Ready);
            std::printf("allocation,%d,%llu,%d,%llu,%llu,%llu\n",
                        int(isWholeSceneControl),
                        static_cast<unsigned long long>(revision),
                        int(allocations.ApiBytesAvailable),
                        static_cast<unsigned long long>(allocations.ApiBytes),
                        static_cast<unsigned long long>(allocations.PeakObservedApiBytes),
                        static_cast<unsigned long long>(allocations.RetiringApiBytes));
            rhi::StorageUsage storage;
            REQUIRE(rhi::GetStorageUsage(device, storage) == Status::Ready);
            std::printf("cascade,%d,%llu,%llu,%zu,%.6f,%.6f,%.6f,%.6f,%d,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                        int(isWholeSceneControl),
                        static_cast<unsigned long long>(revision),
                        static_cast<unsigned long long>(contentRevision),
                        light,
                        double(y),
                        double(eyeOffset),
                        double(eyeY),
                        double(eyeHeight),
                        int(isBlocked),
                        unsigned(pixels[center]),
                        static_cast<unsigned long long>(staged),
                        static_cast<unsigned long long>(gpuNanoseconds),
                        static_cast<unsigned long long>(resolveNanoseconds),
                        static_cast<unsigned long long>(submissionNanoseconds),
                        static_cast<unsigned long long>(resources.PreparedDirectionalNearRegions),
                        static_cast<unsigned long long>(resources.DrawableShadowAttachmentBytes),
                        static_cast<unsigned long long>(storage.ResourceBytes),
                        static_cast<unsigned long long>(storage.PeakResourceBytes));
        }
        if (shouldCheckOracle == false || isWholeSceneControl == true)
        {
            return pixels[center];
        }
        if (isBlocked == true)
        {
            CHECK(pixels[center] <= 12);
        }
        else
        {
            CHECK(pixels[center] >= 243);
        }
        return pixels[center];
    };
    if (mode == ReplayMode::Constrained)
    {
        sample(-1, 0, 0, false);
        sample(0, 1, 0, false);
        const renderer::SkyVisibilityReport sky = graph.GetSkyVisibilityReport();
        CHECK(sky.WholeWorldMaps == renderer::SKY_VISIBILITY_DIRECTION_COUNT);
        CHECK(sky.LocalMaps == renderer::SKY_VISIBILITY_DIRECTION_COUNT);
        CHECK(sky.HasBudgetFallback == false);
        CHECK(graph.GetReport().Resources.DrawableShadowAttachmentBytes == uint64{32} * 1024 * 1024);
        return;
    }
    for (const float32 eyeOffset : {0.F, 0.01F, -3.F, 0.F})
    {
        for (const float32 y : {-1.F, 0.F, 1.F})
        {
            sample(y, 0, eyeOffset);
            sample(y, 1, eyeOffset);
        }
    }
    // Reverse the approach, cross the fixed overlap, and cut back to the same world-space probes.
    constexpr StaticArray<float32, 10> DISTANCES{{3, 9, 10, 11, 12, 16, 12, 10, 9, 3}};
    StaticArray<uint8, DISTANCES.GetSize()> crossing{};
    for (usize pose = 0; pose < DISTANCES.GetSize(); ++pose)
    {
        eyeY = DISTANCES[pose];
        crossing[pose] = sample(-1, 0, 0, eyeY <= 9);
        sample(0, 1, 0);
    }
    if (isWholeSceneControl == false)
    {
        // The near probe is geometrically blocked. At 16 m the whole map supplies its measured endpoint.
        // Intersect the centre pixel ray (half a pixel below the optical axis) with the floor.
        constexpr usize FAR_POSE = 5;
        const float32 halfAngle = VERTICAL_FOV_RADIANS * 0.5F;
        const float32 halfHeightAtUnitDepth = math::Sin(halfAngle) / math::Cos(halfAngle);
        for (usize pose = 0; pose < DISTANCES.GetSize(); ++pose)
        {
            const float32 horizontal = DISTANCES[pose];
            const float32 opticalDistance = math::Sqrt(horizontal * horizontal + eyeHeight * eyeHeight);
            const float32 depth =
                opticalDistance / (1 + horizontal * halfHeightAtUnitDepth / (static_cast<float32>(HEIGHT) * eyeHeight));
            constexpr float32 BLEND_START = static_cast<float32>(renderer::DIRECTIONAL_NEAR_BLEND_METRES);
            constexpr float32 BLEND_WIDTH = static_cast<float32>(renderer::DIRECTIONAL_NEAR_END_METRES) - BLEND_START;
            const float32 t = math::Clamp((depth - BLEND_START) / BLEND_WIDTH, 0.F, 1.F);
            const float32 predicted = static_cast<float32>(crossing[FAR_POSE]) * t * t * (3 - 2 * t);
            CAPTURE(horizontal, depth, predicted, crossing[pose]);
            CHECK(math::Abs(static_cast<float32>(crossing[pose]) - predicted) <= 12);
        }
    }
    eyeY = -3;
    // A grazing Sun, then the roof above the opening, then the Moon alone.
    inputs.Lights = {lights.GetData(), 1};
    for (float32 rise : {0.05F, 1.5F})
    {
        ++contentRevision;
        lights[0].Direction.Z = -rise;
        sample(0, 0, 0);
        sample(-1, 0, 0);
    }
    ++contentRevision;
    inputs.Lights = {&lights[1], 1};
    sample(0, 1, 0);
    sample(-1, 1, 0);
    // A rejected light edit must keep the drawable camera and the previous atlas together.
    const uint64 drawable = graph.GetDrawableSceneRevision();
    lights[1].Direction = {};
    renderer::RenderGraphReport rejected;
    REQUIRE(graph.Update({meshes.GetData(), 2},
                         {items.GetData(), 2},
                         {&camera, 1},
                         ++revision,
                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                         rejected,
                         &inputs,
                         ++contentRevision) == Status::InvalidDescription);
    CHECK(graph.GetDrawableSceneRevision() == drawable);
    lights[1].Direction = {1, 0, -1};
    lights[0].Direction.Z = -1;
    inputs.Lights = {lights.GetData(), 2};
    ++contentRevision;
    sample(0, 0, 0);
    // Locate the half-visibility contour across the doorway from the light's analytic edge at Y=0.5 m.
    if (isWholeSceneControl == false)
    {
        float32 firstEdge = 0;
        for (float32 offset : {0.F, -3.F, 0.01F})
        {
            float32 edge = 0;
            for (uint32 probe = 0; probe <= 16; ++probe)
            {
                const float32 y = 0.3F + static_cast<float32>(probe) * 0.025F;
                if (sample(y, 0, offset, false) < 128 && edge == 0)
                {
                    edge = y - 0.0125F;
                }
            }
            REQUIRE(edge > 0);
            if (firstEdge == 0)
            {
                firstEdge = edge;
            }
            CHECK(math::Abs(edge - OPENING_HALF_WIDTH) <= 0.09375F);
            CHECK(math::Abs(edge - firstEdge) <= 0.09375F);
            if (shouldMeasure == true)
            {
                std::printf("edge,%.6f,%.6f\n", double(offset), double(edge));
            }
        }
    }
    // Move the doorway well beyond the camera's 400 m far plane. At a rise/run of 0.05
    // a ray from the floor crosses the 1,500 m wall at 75 m, between its base and top.
    constexpr float32 RIDGE_DISTANCE_METRES = 1500;
    constexpr float32 RIDGE_BASE_METRES = 74;
    lights[0].Direction.Z = -0.05F;
    lights[1].Direction.Z = -0.05F;
    for (float32 side : {1.F, -1.F})
    {
        const float32 wallPositionX = side * RIDGE_DISTANCE_METRES;
        items[1].MeshToWorld.Translation = {wallPositionX - WALL_X, 0, RIDGE_BASE_METRES};
        ++contentRevision;
        for (usize light = 0; light < lights.GetSize(); ++light)
        {
            const uint8 wallVisibility = sample(-1, light, 0);
            const uint8 openingVisibility = sample(0, light, 0);
            if (shouldMeasure == true)
            {
                std::printf("ridge,%d,%.0f,%llu,%u,%u\n",
                            int(isWholeSceneControl),
                            double(wallPositionX),
                            static_cast<unsigned long long>(lights[light].SourceId),
                            unsigned(wallVisibility),
                            unsigned(openingVisibility));
            }
        }
    }
    items[1].MeshToWorld.Translation = {};
    lights[0].Direction.Z = -1;
    lights[1].Direction.Z = -1;
    Array<math::Vector4> coverage;
    REQUIRE(coverage.TryResize(geometry.Casters.GetSize()) == true);
    meshes[1].Geometry.Colors = {coverage.GetData(), coverage.GetSize()};
    materials[1].IsAlphaMasked = true;
    for (const float32 alpha : {0.F, 1.F})
    {
        for (math::Vector4& colour : coverage)
        {
            colour = {1, 1, 1, alpha};
        }
        isCasterOpaque = alpha == 1;
        ++contentRevision;
        sample(-1, 0, 0);
    }
    items[1].MeshToWorld.Translation.Y = 8;
    ++contentRevision;
    sample(-1, 0, 0);
    items[1].MeshToWorld.Translation.Y = 0;
    geometry.Ground[0].X -= 1000;
    materials[1].IsAlphaMasked = false;
    ++contentRevision;
    sample(-1, 0, 0);
    // A true inclined receiver with a Sun grazing the rising ground. The centre ray crosses
    // the opening below the roof. Both independent rays leave the plane on its lit side.
    math::Vector3 slopeNormalWorld;
    REQUIRE(math::TryNormalize({-0.5F, 0, 1}, slopeNormalWorld) == math::MathStatus::Success);
    for (usize vertex = 0; vertex < geometry.Ground.GetSize(); ++vertex)
    {
        geometry.Ground[vertex].Z = 0.5F * geometry.Ground[vertex].X;
        geometry.GroundNormals[vertex] = slopeNormalWorld;
    }
    lights[0].Direction.Z = -0.55F;
    ++contentRevision;
    const uint8 slopeSunVisibility = sample(0, 0, 0);
    const uint8 slopeMoonVisibility = sample(0, 1, 0);
    if (shouldMeasure == true)
    {
        std::printf("slope,%d,%u,%u\n",
                    int(isWholeSceneControl),
                    unsigned(slopeSunVisibility),
                    unsigned(slopeMoonVisibility));
    }
    for (usize vertex = 0; vertex < geometry.Ground.GetSize(); ++vertex)
    {
        geometry.Ground[vertex].Z = 0;
        geometry.GroundNormals[vertex] = {0, 0, 1};
    }
    lights[0].Direction.Z = -1;
    width = 64;
    height = 48;
    const Status resizing = graph.Resize(width, height);
    REQUIRE((resizing == Status::Pending || resizing == Status::Ready));
    ++contentRevision;
    sample(-1, 0, 0);
    sample(0, 1, 0);
    width = WIDTH;
    height = HEIGHT;
    const Status restored = graph.Resize(width, height);
    REQUIRE((restored == Status::Pending || restored == Status::Ready));
    ++contentRevision;
    sample(0, 0, 0);
    if (shouldMeasure == true)
    {
        for (uint32 frame = 0; frame < 64; ++frame)
        {
            sample(0, 1, 0);
        }
    }
}
