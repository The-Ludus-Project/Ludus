#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/ray.h>

#include "graph_ray_query.h"
#include "ray_query.h"

#include <cstring>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
template <typename T>
rhi::RasterStatus Poll(rhi::DeviceHandle device, T handle) noexcept
{
    const auto started = time::NowTicks();
    while (time::NowTicks() - started < 10 * time::NANOSECONDS_PER_SECOND)
    {
        const auto status = rhi::GetStatus(device, handle);
        if (status != rhi::RasterStatus::Pending)
        {
            return status;
        }
    }
    return rhi::RasterStatus::Pending;
}
} // namespace
// Runs on every adapter, so a device without the optional ray profile (CI's llvmpipe) still
// validates this binary under Vulkan validation instead of skipping every case.
TEST_CASE("Ray capability reports match the adapter and leave raster usable", "[rhi][gpu][ray]")
{
    rhi::Shutdown();
    logging::LogConfig loggingConfig;
    loggingConfig.Mode = logging::LogMode::Synchronous;
    loggingConfig.EnableFile = false;
    loggingConfig.EnableDebugger = false;
    static_cast<void>(logging::LogSystem::Initialize(loggingConfig));
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
            logging::LogSystem::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    const rhi::RasterStatus status = rhi::GetRayCapabilities(device, capabilities);
    // The RT0 profile is exactly static triangles with inline queries; Unsupported reports lack it.
    const bool hasProfile = capabilities.StaticTriangles == true && capabilities.InlineQueries == true;
    if (status == rhi::RasterStatus::Unsupported)
    {
        CHECK(hasProfile == false);
    }
    else
    {
        REQUIRE(status == rhi::RasterStatus::Ready);
        CHECK(hasProfile == true);
    }
    // RT0 never negotiates procedural intersections; R16b refit and compaction accompany the profile.
    CHECK(capabilities.ProceduralIntersections == false);
    CHECK(capabilities.Updates == hasProfile);
    CHECK(capabilities.CanCompact == hasProfile);
    CHECK(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    CHECK(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
}

TEST_CASE("Native RT0 filters opacity, traces offscreen instances and retains detached resources", "[rhi][gpu][ray]")
{
    rhi::Shutdown();
    logging::LogConfig loggingConfig;
    loggingConfig.Mode = logging::LogMode::Synchronous;
    loggingConfig.EnableFile = loggingConfig.EnableDebugger = false;
    static_cast<void>(logging::LogSystem::Initialize(loggingConfig));
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
            logging::LogSystem::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    const auto started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Unsupported)
    {
        // The capability case above covers the unsupported report and the raster frame.
        SKIP("Native adapter does not implement the optional ray profile; raster remains usable");
    }
    REQUIRE(capabilities.InlineQueries == true);
    // Inline queries also run on devices that expose optional native ray pipelines.
    CHECK(sizeof(rhi::RayInput) == 48);
    CHECK(sizeof(rhi::RayResult) == 32);
    rhi::RayQueryProgram program;
    REQUIRE(rhi::CreateRayQueryProgram(device, ludus::shaders::ray_query::Query(), program) ==
            rhi::RasterStatus::Ready);
    const float32 positions[]{-1, -1, 0, 1, -1, 0, 0, 1, 0};
    const float32 uv[]{0, 1, 1, 1, 0.5F, 0};
    const uint32 indices[]{0, 1, 2};
    rhi::RayGeometryDescription mesh{positions, 3, indices, 3, uv, usize{16} * 1024 * 1024};
    rhi::RayGeometryHandle geometry;
    auto undersized = mesh;
    undersized.ByteBudget = 1;
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        CHECK(rhi::CreateRayGeometry(device, undersized, geometry) == rhi::RasterStatus::CapacityExceeded);
        CHECK(rhi::GetStatus(device, geometry) == rhi::RasterStatus::InvalidHandle);
    }
    rhi::RayResourceStats stats;
    REQUIRE(rhi::GetRayResourceStats(device, stats) == rhi::RasterStatus::Ready);
    CHECK(stats.Geometries == 0);
    CHECK(stats.ResidentBytes == 0);
    REQUIRE(rhi::CreateRayGeometry(device, mesh, geometry) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, geometry) == rhi::RasterStatus::Ready);
    rhi::RayAllocation bytes;
    REQUIRE(rhi::GetRayAllocation(device, geometry, bytes) == rhi::RasterStatus::Ready);
    CHECK(bytes.ResidentBytes > sizeof(positions) + sizeof(indices));
    const float32 alpha[]{0, 1, 0, 1};
    rhi::RayInstanceDescription instances[3];
    for (auto& instance : instances)
    {
        instance.Geometry = geometry;
    }
    instances[0].Transform[11] = 0.5F;
    instances[0].AlphaMasked = true;
    instances[0].Alpha = alpha;
    instances[0].Width = instances[0].Height = 2;
    instances[2].Transform[0] = -2;
    instances[2].Transform[1] = .3F;
    instances[2].Transform[5] = .5F;
    instances[2].Transform[3] = 3;
    instances[2].Mask = 2;
    rhi::RaySceneHandle scene;
    REQUIRE(rhi::CreateRayScene(device, {instances, 3, usize{16} * 1024 * 1024}, scene) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, scene) == rhi::RasterStatus::Ready);
    const auto detachedGeometry = geometry;
    REQUIRE(rhi::Destroy(device, geometry) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(device, detachedGeometry) == rhi::RasterStatus::InvalidHandle);
    rhi::RayInput rays[7];
    rays[0].Origin[0] = -0.5F;
    rays[0].Origin[1] = -0.5F;
    rays[0].Origin[2] = 1;
    rays[1] = rays[0];
    rays[1].Origin[0] = 0.5F;
    rays[2].Origin[0] = 3;
    rays[2].Origin[2] = 1;
    rays[2].Mask = 2;
    rays[3] = rays[0];
    rays[3].Origin[0] = 8;
    rays[4] = rays[1];
    rays[4].VisibilityOnly = 1;
    rays[5] = rays[2];
    rays[5].Mask = 1;
    rays[6] = rays[1];
    rays[6].MaxDistance = .25F;
    rhi::RayQueryBatch batch;
    const auto saved = rays[0];
    rays[0].Direction[2] = -2;
    CHECK(rhi::SubmitRayQueries(device, program, scene, rays, 7, batch) == rhi::RasterStatus::InvalidDescription);
    CHECK(rhi::GetStatus(device, batch) == rhi::RasterStatus::InvalidHandle);
    rays[0] = saved;
    REQUIRE(rhi::SubmitRayQueries(device, program, scene, rays, 7, batch) == rhi::RasterStatus::Pending);
    const auto detachedScene = scene;
    const auto detachedProgram = program;
    REQUIRE(rhi::Destroy(device, scene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, program) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(device, detachedScene) == rhi::RasterStatus::InvalidHandle);
    CHECK(rhi::GetStatus(device, detachedProgram) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(Poll(device, batch) == rhi::RasterStatus::Ready);
    rhi::RayResult results[7];
    results[0].Hit = 77;
    CHECK(rhi::ReadRayResults(device, batch, results, 6) == rhi::RasterStatus::InvalidDescription);
    CHECK(results[0].Hit == 77);
    REQUIRE(rhi::ReadRayResults(device, batch, results, 7) == rhi::RasterStatus::Ready);
    CHECK(results[0].Hit == 1);
    CHECK(results[0].Instance == 1);
    CHECK(results[0].Distance == 1);
    CHECK(results[1].Hit == 1);
    CHECK(results[1].Instance == 0);
    CHECK(results[1].Distance == 0.5F);
    CHECK(results[2].Hit == 1);
    CHECK(results[2].Instance == 2);
    CHECK(results[2].Distance == 1);
    CHECK(results[3].Hit == 0);
    CHECK(results[4].Hit == 1);
    CHECK(results[5].Hit == 0);
    CHECK(results[6].Hit == 0);
    const auto stale = batch;
    REQUIRE(rhi::Destroy(device, batch) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(device, stale) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::GetRayResourceStats(device, stats) == rhi::RasterStatus::Ready);
    CHECK(stats.Geometries == 0);
    CHECK(stats.Scenes == 0);
    CHECK(stats.Batches == 0);
    CHECK(stats.ResidentBytes == 0);
    CHECK(stats.BuildScratchBytes == 0);
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
}

TEST_CASE("RT0 budgeted records and instances exceed legacy ceilings with retained identities",
          "[rhi][gpu][ray][capacity]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    description.Storage.RayResourcesPerKind = 72;
    description.Storage.RayInstances = 513;
    REQUIRE(rhi::CreateDevice({},
                              {
                                  .Width = 8,
                                  .Height = 8,
                              },
                              description,
                              device,
                              surface) == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Unsupported)
    {
        SKIP("Optional native ray queries unavailable");
    }
    REQUIRE(capabilities.MaxInstances == 513);
    const float32 positions[]{-1, -1, 0, 1, -1, 0, 0, 1, 0};
    const uint32 indices[]{0, 1, 2};
    const rhi::RayGeometryDescription mesh{positions, 3, indices, 3, nullptr, usize{16} * 1024 * 1024};
    rhi::RayGeometryHandle geometries[72];
    for (auto& geometry : geometries)
    {
        REQUIRE(rhi::CreateRayGeometry(device, mesh, geometry) == rhi::RasterStatus::Pending);
        REQUIRE(Poll(device, geometry) == rhi::RasterStatus::Ready);
    }
    rhi::RayGeometryHandle excess;
    CHECK(rhi::CreateRayGeometry(device, mesh, excess) == rhi::RasterStatus::CapacityExceeded);
    CHECK(rhi::GetStatus(device, excess) == rhi::RasterStatus::InvalidHandle);
    rhi::RayInstanceDescription instances[514];
    for (usize i = 0; i < 514; ++i)
    {
        instances[i].Geometry = geometries[i % 72];
        instances[i].Transform[3] = static_cast<float32>(i * 3);
    }
    rhi::RaySceneHandle scene;
    CHECK(rhi::CreateRayScene(device, {instances, 514, usize{16} * 1024 * 1024}, scene) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(rhi::GetStatus(device, scene) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::CreateRayScene(device, {instances, 513, usize{16} * 1024 * 1024}, scene) ==
            rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, scene) == rhi::RasterStatus::Ready);
    const auto staleGeometry = geometries[71];
    for (auto& geometry : geometries)
    {
        REQUIRE(rhi::Destroy(device, geometry) == rhi::RasterStatus::Ready);
    }
    CHECK(rhi::CreateRayGeometry(device, mesh, excess) == rhi::RasterStatus::CapacityExceeded);
    rhi::RayQueryProgram program;
    REQUIRE(rhi::CreateRayQueryProgram(device, ludus::shaders::ray_query::Query(), program) ==
            rhi::RasterStatus::Ready);
    const uint32 identities[]{0, 64, 512};
    rhi::RayInput rays[3];
    for (usize i = 0; i < 3; ++i)
    {
        rays[i].Origin[0] = static_cast<float32>(identities[i] * 3);
        rays[i].Origin[2] = 1;
    }
    rhi::RayQueryBatch batch;
    REQUIRE(rhi::SubmitRayQueries(device, program, scene, rays, 3, batch) == rhi::RasterStatus::Pending);
    REQUIRE(rhi::Destroy(device, scene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, program) == rhi::RasterStatus::Ready);
    REQUIRE(Poll(device, batch) == rhi::RasterStatus::Ready);
    CHECK(rhi::CreateRayGeometry(device, mesh, excess) == rhi::RasterStatus::CapacityExceeded);
    rhi::RayResult results[3];
    REQUIRE(rhi::ReadRayResults(device, batch, results, 3) == rhi::RasterStatus::Ready);
    for (usize i = 0; i < 3; ++i)
    {
        CHECK(results[i].Hit == 1);
        CHECK(results[i].Instance == identities[i]);
        CHECK(results[i].Distance == 1);
    }
    REQUIRE(rhi::Destroy(device, batch) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRayGeometry(device, mesh, excess) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, excess) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(device, staleGeometry) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::Destroy(device, excess) == rhi::RasterStatus::Ready);
}

TEST_CASE("Opaque geometry prefixes preserve full primitive identities and masked suffix acceptance",
          "[rhi][gpu][ray][opacity]")
{
    rhi::Shutdown();
    struct Guard final
    {
        LUDUS_INLINE ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Preferred.RayQueries = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, deviceDescription, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No native GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Unsupported)
    {
        SKIP("Native ray queries unavailable");
    }
    constexpr usize TRIANGLE_COUNT = 2;
    constexpr usize TRIANGLE_CORNERS = 3;
    constexpr usize POSITION_COMPONENTS = 3;
    constexpr usize VERTEX_COUNT = TRIANGLE_COUNT * TRIANGLE_CORNERS;
    constexpr usize BYTE_BUDGET = usize{16} * 1024 * 1024;
    const StaticArray<float32, VERTEX_COUNT * POSITION_COMPONENTS> positions{
        {-1, -1, 0, 1, -1, 0, 0, 1, 0, -1, -1, .5F, 1, -1, .5F, 0, 1, .5F}};
    const StaticArray<uint32, VERTEX_COUNT> indices{{0, 1, 2, 3, 4, 5}};
    rhi::RayGeometryDescription description
    {
        .Positions = positions.GetData(),
        .VertexCount = VERTEX_COUNT,
        .Indices = indices.GetData(),
        .IndexCount = indices.GetSize(),
        .ByteBudget = BYTE_BUDGET,
        .OpaqueTriangleCount = TRIANGLE_COUNT + 1,
    };
    rhi::RayGeometryHandle geometry;
    CHECK(rhi::CreateRayGeometry(device, description, geometry) == rhi::RasterStatus::InvalidDescription);
    CHECK(rhi::GetStatus(device, geometry) == rhi::RasterStatus::InvalidHandle);
    description.OpaqueTriangleCount = 1;
    REQUIRE(rhi::CreateRayGeometry(device, description, geometry) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, geometry) == rhi::RasterStatus::Ready);
    rhi::RayQueryProgram program;
    REQUIRE(rhi::CreateRayQueryProgram(device, ludus::shaders::ray_query::Query(), program) ==
            rhi::RasterStatus::Ready);
    constexpr uint32 CONFIGURATION_COUNT = 3;
    for (uint32 configuration = 0; configuration < CONFIGURATION_COUNT; ++configuration)
    {
        const uint32 expectedPrimitive = configuration == 0 ? 0 : 1;
        rhi::RayInstanceDescription instance;
        instance.Geometry = geometry;
        instance.AlphaMasked = configuration != 2;
        instance.Opacity = configuration == 1 ? 1 : 0;
        rhi::RaySceneHandle scene;
        REQUIRE(rhi::CreateRayScene(device, {&instance, 1, BYTE_BUDGET}, scene) == rhi::RasterStatus::Pending);
        REQUIRE(Poll(device, scene) == rhi::RasterStatus::Ready);
        constexpr usize RAY_COUNT = 2;
        StaticArray<rhi::RayInput, RAY_COUNT> rays{};
        rays[0].Origin[2] = 1;
        rays[1] = rays[0];
        rays[1].VisibilityOnly = 1;
        rhi::RayQueryBatch batch;
        REQUIRE(rhi::SubmitRayQueries(device, program, scene, rays.GetData(), rays.GetSize(), batch) ==
                rhi::RasterStatus::Pending);
        REQUIRE(Poll(device, batch) == rhi::RasterStatus::Ready);
        StaticArray<rhi::RayResult, RAY_COUNT> results{};
        REQUIRE(rhi::ReadRayResults(device, batch, results.GetData(), results.GetSize()) == rhi::RasterStatus::Ready);
        CHECK(results[0].Hit == 1);
        CHECK(results[0].Primitive == expectedPrimitive);
        CHECK(results[0].Instance == 0);
        CHECK(results[0].Reserved == 0);
        CHECK(results[0].Distance == (expectedPrimitive == 0 ? 1.0F : .5F));
        CHECK(results[1].Hit == 1);
        REQUIRE(rhi::Destroy(device, batch) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, scene) == rhi::RasterStatus::Ready);
    }
    REQUIRE(rhi::Destroy(device, geometry) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, program) == rhi::RasterStatus::Ready);
}

TEST_CASE("Native ray geometry uses the device buffer range beyond the legacy 64 MiB ceiling",
          "[rhi][gpu][ray][capacity]")
{
    rhi::Shutdown();
    struct DeviceGuard final
    {
        ~DeviceGuard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Unsupported)
    {
        SKIP("Native ray queries are unavailable");
    }
    rhi::ComputeCapabilities compute;
    REQUIRE(rhi::GetComputeCapabilities(device, compute) == rhi::RasterStatus::Ready);
    CHECK(capabilities.MaxBufferBytes == compute.MaxStorageRange);
    constexpr usize LEGACY_BUFFER_BYTES = usize{64} * 1024 * 1024;
    constexpr usize POSITION_COMPONENTS = 3;
    constexpr usize TRIANGLE_CORNERS = 3;
    constexpr usize VERTEX_COUNT = LEGACY_BUFFER_BYTES / (POSITION_COMPONENTS * sizeof(float32)) + 1;
    constexpr usize VERTEX_BYTES = VERTEX_COUNT * POSITION_COMPONENTS * sizeof(float32);
    if (compute.MaxStorageRange < VERTEX_BYTES)
    {
        SKIP("The device's native buffer range does not exceed the legacy ceiling");
    }
    // Only the first three vertices are referenced. The complete input buffer must
    // nevertheless be admitted, without requiring a large acceleration structure.
    Array<float32> positionsMesh;
    REQUIRE(positionsMesh.TryResize(VERTEX_COUNT * POSITION_COMPONENTS) == true);
    positionsMesh[POSITION_COMPONENTS] = 1;
    positionsMesh[2 * POSITION_COMPONENTS + 1] = 1;
    const StaticArray<uint32, TRIANGLE_CORNERS> indices{{0, 1, 2}};
    rhi::RayGeometryDescription geometryDescription;
    geometryDescription.Positions = positionsMesh.GetData();
    geometryDescription.VertexCount = VERTEX_COUNT;
    geometryDescription.Indices = indices.GetData();
    geometryDescription.IndexCount = indices.GetSize();
    geometryDescription.ByteBudget = 2 * VERTEX_BYTES;
    rhi::RayGeometryHandle geometry;
    rhi::RayGeometryDescription rejected = geometryDescription;
    rejected.ByteBudget = VERTEX_BYTES - 1;
    CHECK(rhi::CreateRayGeometry(device, rejected, geometry) == rhi::RasterStatus::CapacityExceeded);
    CHECK(rhi::GetStatus(device, geometry) == rhi::RasterStatus::InvalidHandle);
    REQUIRE(rhi::CreateRayGeometry(device, geometryDescription, geometry) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, geometry) == rhi::RasterStatus::Ready);
    rhi::RayAllocation allocation;
    REQUIRE(rhi::GetRayAllocation(device, geometry, allocation) == rhi::RasterStatus::Ready);
    CHECK(allocation.ResidentBytes >= VERTEX_BYTES);
    REQUIRE(rhi::Destroy(device, geometry) == rhi::RasterStatus::Ready);
}

TEST_CASE("Native ray opacity queries preserve aligned offsets beyond 64 MiB", "[rhi][gpu][ray][capacity]")
{
    rhi::Shutdown();
    struct DeviceGuard final
    {
        ~DeviceGuard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    description.Storage.HostBytes = usize{768} * 1024 * 1024;
    description.Storage.ResourceBytes = uint64{1} * 1024 * 1024 * 1024;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Unsupported)
    {
        SKIP("Native ray queries are unavailable");
    }
    constexpr usize BYTE_BUDGET = usize{512} * 1024 * 1024;
    constexpr uint32 ALPHA_EXTENT = 2048;
    constexpr usize ALPHA_TEXELS = usize{ALPHA_EXTENT} * ALPHA_EXTENT;
    constexpr usize ACCEPTED_INSTANCE_COUNT = 9;
    constexpr usize OVERFLOW_INSTANCE_COUNT = 16;
    if (capabilities.MaxBufferBytes < ALPHA_TEXELS * sizeof(float32) * OVERFLOW_INSTANCE_COUNT)
    {
        SKIP("The device buffer range cannot exercise the complete aligned opacity-table bound");
    }
    constexpr usize COMPONENT_COUNT = 9;
    constexpr usize CORNER_COUNT = 3;
    const StaticArray<float32, COMPONENT_COUNT> positionsMesh{{-1, -1, 0, 1, -1, 0, 0, 1, 0}};
    const StaticArray<uint32, CORNER_COUNT> indices{{0, 1, 2}};
    rhi::RayGeometryHandle geometry;
    REQUIRE(rhi::CreateRayGeometry(
                device,
                {positionsMesh.GetData(), CORNER_COUNT, indices.GetData(), indices.GetSize(), nullptr, BYTE_BUDGET},
                geometry) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, geometry) == rhi::RasterStatus::Ready);
    Array<float32> alpha;
    REQUIRE(alpha.TryResize(ALPHA_TEXELS) == true);
    for (float32& value : alpha)
    {
        value = 0;
    }
    // All earlier instances reject; only the final instance accepts the query.
    // Its alpha offset exceeds 2^25 scalars, where exact float32 addresses need
    // the four-scalar alignment guaranteed by the table layout.
    StaticArray<rhi::RayInstanceDescription, OVERFLOW_INSTANCE_COUNT> instances{};
    for (rhi::RayInstanceDescription& instance : instances)
    {
        instance.Geometry = geometry;
        instance.AlphaMasked = true;
        instance.Alpha = alpha.GetData();
        instance.Width = ALPHA_EXTENT;
        instance.Height = ALPHA_EXTENT;
    }
    rhi::RaySceneHandle rejected;
    CHECK(rhi::CreateRayScene(device, {instances.GetData(), instances.GetSize(), BYTE_BUDGET}, rejected) ==
          rhi::RasterStatus::CapacityExceeded);
    CHECK(rhi::GetStatus(device, rejected) == rhi::RasterStatus::InvalidHandle);
    // Keep a real alpha table after the old ceiling, rather than accepting a constant.
    Array<float32> opaqueAlpha;
    REQUIRE(opaqueAlpha.TryResize(ALPHA_TEXELS) == true);
    for (float32& value : opaqueAlpha)
    {
        value = 1;
    }
    instances[ACCEPTED_INSTANCE_COUNT - 1].Alpha = opaqueAlpha.GetData();
    instances[ACCEPTED_INSTANCE_COUNT - 1].Width = ALPHA_EXTENT;
    instances[ACCEPTED_INSTANCE_COUNT - 1].Height = ALPHA_EXTENT;
    rhi::RaySceneHandle scene;
    REQUIRE(rhi::CreateRayScene(device, {instances.GetData(), ACCEPTED_INSTANCE_COUNT, BYTE_BUDGET}, scene) ==
            rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, scene) == rhi::RasterStatus::Ready);
    rhi::RayQueryProgram program;
    REQUIRE(rhi::CreateRayQueryProgram(device, ludus::shaders::ray_query::Query(), program) ==
            rhi::RasterStatus::Ready);
    rhi::RayInput ray;
    ray.Origin[2] = 1;
    rhi::RayQueryBatch batch;
    REQUIRE(rhi::SubmitRayQueries(device, program, scene, &ray, 1, batch) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, batch) == rhi::RasterStatus::Ready);
    rhi::RayResult result;
    REQUIRE(rhi::ReadRayResults(device, batch, &result, 1) == rhi::RasterStatus::Ready);
    CHECK(result.Hit == 1);
    CHECK(result.Instance == ACCEPTED_INSTANCE_COUNT - 1);
    CHECK(result.Distance == 1);
}

// R16a/R17a: an ordinary compute pipeline binds the scene and an ordered graph declares the read.
// Independent triangle fixtures check hit/miss, ray intervals, instance transforms and identities;
// the import keeps the scene and BLAS alive after their public handles are destroyed.
TEST_CASE("Ordered graphs read declared native scenes in compute ray queries", "[rhi][gpu][ray][graph]")
{
    rhi::Shutdown();
    logging::LogConfig loggingConfig;
    loggingConfig.Mode = logging::LogMode::Synchronous;
    loggingConfig.EnableFile = loggingConfig.EnableDebugger = false;
    static_cast<void>(logging::LogSystem::Initialize(loggingConfig));
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
            logging::LogSystem::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.Compute = true;
    description.Preferred.RayQueries = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    rhi::DeviceInfo info;
    REQUIRE(rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready);
    if (rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Unsupported ||
        info.Enabled.Compute == false)
    {
        // The reference graph cases cover unsupported-target admission deterministically.
        SKIP("Native adapter does not implement compute ray queries; raster remains usable");
    }
    constexpr usize RAY_COUNT = 4;
    constexpr usize RAY_VECTORS = 2;
    constexpr usize VECTOR_COMPONENTS = 4;
    constexpr usize RAY_BYTES = RAY_COUNT * RAY_VECTORS * VECTOR_COMPONENTS * sizeof(float32);
    constexpr usize HIT_BYTES = RAY_COUNT * VECTOR_COMPONENTS * sizeof(uint32);
    constexpr usize VECTOR_BYTES = VECTOR_COMPONENTS * sizeof(float32);
    constexpr usize BYTE_BUDGET = usize{16} * 1024 * 1024;
    constexpr float32 INSTANCE_OFFSET = 10;
    constexpr float32 RAY_HEIGHT = 5;
    constexpr float32 MISS_X = 5;
    constexpr float32 SHORT_INTERVAL = 4;
    constexpr float32 FAR_INTERVAL = 100;
    constexpr uint32 TRANSLATION_X = 3;
    const StaticArray<float32, 9> positions{-1, -1, 0, 1, -1, 0, 0, 1, 0};
    const StaticArray<uint32, 3> indices{0, 1, 2};
    const rhi::RayGeometryDescription mesh{positions.GetData(),
                                           3,
                                           indices.GetData(),
                                           indices.GetSize(),
                                           nullptr,
                                           BYTE_BUDGET};
    rhi::RayGeometryHandle geometry;
    REQUIRE(rhi::CreateRayGeometry(device, mesh, geometry) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, geometry) == rhi::RasterStatus::Ready);
    StaticArray<rhi::RayInstanceDescription, 2> instances{};
    instances[0].Geometry = geometry;
    instances[1].Geometry = geometry;
    instances[1].Transform[TRANSLATION_X] = INSTANCE_OFFSET;
    rhi::RaySceneHandle scene;
    REQUIRE(rhi::CreateRayScene(device, {instances.GetData(), instances.GetSize(), BYTE_BUDGET}, scene) ==
            rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, scene) == rhi::RasterStatus::Ready);

    // Downward rays: instance 0, translated instance 1, a gap between them, and an interval
    // ending before the first triangle.
    const StaticArray<float32, RAY_COUNT * RAY_VECTORS * VECTOR_COMPONENTS> rays{
        0,
        0,
        RAY_HEIGHT,
        0,
        0,
        0,
        -1,
        FAR_INTERVAL,
        INSTANCE_OFFSET,
        0,
        RAY_HEIGHT,
        0,
        0,
        0,
        -1,
        FAR_INTERVAL,
        MISS_X,
        0,
        RAY_HEIGHT,
        0,
        0,
        0,
        -1,
        FAR_INTERVAL,
        0,
        0,
        RAY_HEIGHT,
        0,
        0,
        0,
        -1,
        SHORT_INTERVAL,
    };
    rhi::BufferHandle rayBuffer;
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::Storage, RAY_BYTES},
                              {reinterpret_cast<const uint8*>(rays.GetData()), RAY_BYTES},
                              rayBuffer) == rhi::RasterStatus::Ready);
    const StaticArray<uint8, HIT_BYTES> zeros{};
    rhi::BufferHandle hitBuffer;
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::Storage, HIT_BYTES},
                              {zeros.GetData(), zeros.GetSize()},
                              hitBuffer) == rhi::RasterStatus::Ready);
    const rhi::RasterShaderDescription shaderDescription = ludus::shaders::graph_ray_query::Compute();
    rhi::RasterShaderHandle shader;
    REQUIRE(rhi::CreateRasterShader(device, shaderDescription, shader) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::RasterBinding, 3> bindings{{
        {0, rhi::RasterBindingKind::RayScene, rhi::RasterVisibility::Compute, 0},
        {1, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, VECTOR_BYTES},
        {2, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, VECTOR_BYTES},
    }};
    rhi::BindingLayoutHandle layout;
    REQUIRE(rhi::CreateBindingLayout(device, {bindings.GetData(), bindings.GetSize()}, layout) ==
            rhi::RasterStatus::Ready);
    const StaticArray<rhi::RasterBindingResource, 3> resources{{
        { .Binding = 0, .Scene = scene },
        { .Binding = 1, .Buffer = rayBuffer, .Size = RAY_BYTES },
        { .Binding = 2, .Buffer = hitBuffer, .Size = HIT_BYTES },
    }};
    rhi::BindingSetHandle set;
    REQUIRE(rhi::CreateBindingSet(device, layout, {resources.GetData(), resources.GetSize()}, set) ==
            rhi::RasterStatus::Ready);
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(rhi::CreateComputePipeline(device, {shader, layout}, pipeline) == rhi::RasterStatus::Ready);
    if (rhi::GetStatus(device, pipeline) == rhi::RasterStatus::Pending)
    {
        REQUIRE(Poll(device, pipeline) == rhi::RasterStatus::Ready);
    }

    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == rhi::RasterStatus::Ready);
    rhi::GraphVersion rayVersion;
    rhi::GraphVersion hitVersion;
    rhi::GraphVersion hitWritten;
    rhi::GraphVersion sceneVersion;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, rayBuffer, rayVersion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, hitBuffer, hitVersion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, hitVersion, hitWritten) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphRayScene(device, graph, scene, sceneVersion) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, 3> uses{{
        {rayVersion, rhi::GraphAccessMode::StorageRead, 0, RAY_BYTES, rhi::RasterVisibility::Compute},
        {hitWritten, rhi::GraphAccessMode::StorageReadWrite, 0, HIT_BYTES, rhi::RasterVisibility::Compute},
        {sceneVersion, rhi::GraphAccessMode::AccelerationStructureRead, 0, 0, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch{pipeline, set};
    rhi::GraphPassDescription pass;
    pass.Name = "graph ray query";
    pass.Uses = uses.GetData();
    pass.UseCount = uses.GetSize();
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::AddGraphRoot(device, graph, hitWritten, rhi::GraphRoot::Readback) == rhi::RasterStatus::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == rhi::RasterStatus::Ready);
    bool hasBuildDependency = false;
    for (uint32 i = 0; i < report.ReportedDependencies; ++i)
    {
        const rhi::GraphDependency& edge = report.Dependencies[i];
        hasBuildDependency =
            hasBuildDependency == true || (edge.Before == rhi::GRAPH_EXTERNAL_INDEX && edge.After == 0 &&
                                           edge.Source == rhi::GraphAccessMode::AccelerationStructureBuild &&
                                           edge.Destination == rhi::GraphAccessMode::AccelerationStructureRead);
    }
    CHECK(hasBuildDependency == true);
    // The graph and its retained binding snapshot keep the scene and BLAS through execution.
    REQUIRE(rhi::Destroy(device, scene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, geometry) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, set) == rhi::RasterStatus::Ready);
    rhi::RayResourceStats stats;
    REQUIRE(rhi::GetRayResourceStats(device, stats) == rhi::RasterStatus::Ready);
    CHECK(stats.Scenes == 1);
    CHECK(stats.Geometries == 1);
    CHECK(stats.DetachedBytes == stats.ResidentBytes);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::ExecuteOrderedGraph(device, surface, graph, completion) == rhi::RasterStatus::Ready);

    rhi::ReadbackTicket ticket;
    REQUIRE(rhi::RequestBufferReadback(device, hitBuffer, 0, HIT_BYTES, ticket) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, ticket) == rhi::RasterStatus::Ready);
    StaticArray<uint32, RAY_COUNT * VECTOR_COMPONENTS> hits{};
    REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(hits.GetData()), HIT_BYTES) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(device, ticket) == rhi::RasterStatus::Ready);
    const auto distance = [&hits](usize ray) noexcept {
        float32 value = 0;
        std::memcpy(&value, &hits[ray * VECTOR_COMPONENTS + 3], sizeof(value));
        return value;
    };
    // Hit, instance, primitive and distance along the normalized world ray.
    CHECK(hits[0] == 1);
    CHECK(hits[1] == 0);
    CHECK(hits[2] == 0);
    CHECK(distance(0) == RAY_HEIGHT);
    CHECK(hits[VECTOR_COMPONENTS] == 1);
    CHECK(hits[VECTOR_COMPONENTS + 1] == 1);
    CHECK(distance(1) == RAY_HEIGHT);
    for (usize component = 0; component < VECTOR_COMPONENTS; ++component)
    {
        CAPTURE(component);
        CHECK(hits[2 * VECTOR_COMPONENTS + component] == 0);
        CHECK(hits[3 * VECTOR_COMPONENTS + component] == 0);
    }
    REQUIRE(Poll(device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::PollLifetime(device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetRayResourceStats(device, stats) == rhi::RasterStatus::Ready);
    CHECK(stats.Scenes == 0);
    CHECK(stats.Geometries == 0);
    REQUIRE(rhi::Destroy(device, pipeline) == rhi::RasterStatus::Ready);
}

namespace
{
// R16b parity fixture: a deformable grid of quads traced by a fixed grid of downward rays.
constexpr usize GRID_QUADS = 8;
constexpr usize GRID_VERTICES_PER_SIDE = GRID_QUADS + 1;
constexpr usize GRID_VERTEX_COUNT = GRID_VERTICES_PER_SIDE * GRID_VERTICES_PER_SIDE;
constexpr usize GRID_INDEX_COUNT = GRID_QUADS * GRID_QUADS * 6;
constexpr usize GRID_POSITION_COUNT = GRID_VERTEX_COUNT * 3;
constexpr usize GRID_RAYS_PER_SIDE = 12;
constexpr usize GRID_RAY_COUNT = GRID_RAYS_PER_SIDE * GRID_RAYS_PER_SIDE;
constexpr usize GRID_BYTE_BUDGET = usize{16} * 1024 * 1024;
// Refit and rebuilt structures intersect the same triangles; traversal order alone differs.
constexpr float32 PARITY_TOLERANCE = 1e-5F;
using GridPositions = StaticArray<float32, GRID_POSITION_COUNT>;
using GridResults = StaticArray<rhi::RayResult, GRID_RAY_COUNT>;
// Grid over [-1,1]^2; a nonzero amplitude bends each vertex height without folding a triangle.
GridPositions MakeGridPositions(float32 amplitude) noexcept
{
    GridPositions positions{};
    for (usize row = 0; row < GRID_VERTICES_PER_SIDE; ++row)
    {
        for (usize column = 0; column < GRID_VERTICES_PER_SIDE; ++column)
        {
            const usize vertex = row * GRID_VERTICES_PER_SIDE + column;
            positions[vertex * 3] = -1 + 2 * static_cast<float32>(column) / GRID_QUADS;
            positions[vertex * 3 + 1] = -1 + 2 * static_cast<float32>(row) / GRID_QUADS;
            positions[vertex * 3 + 2] = amplitude * static_cast<float32>((row * 3 + column * 5) % 7) / 7;
        }
    }
    return positions;
}
StaticArray<uint32, GRID_INDEX_COUNT> MakeGridIndices() noexcept
{
    StaticArray<uint32, GRID_INDEX_COUNT> indices{};
    usize next = 0;
    for (usize row = 0; row < GRID_QUADS; ++row)
    {
        for (usize column = 0; column < GRID_QUADS; ++column)
        {
            const uint32 corner = static_cast<uint32>(row * GRID_VERTICES_PER_SIDE + column);
            const uint32 above = corner + static_cast<uint32>(GRID_VERTICES_PER_SIDE);
            const StaticArray<uint32, 6> quad{{corner, corner + 1, above + 1, corner, above + 1, above}};
            for (const uint32 index : quad)
            {
                indices[next] = index;
                ++next;
            }
        }
    }
    return indices;
}
// Downward rays over a slightly larger square, so the outer ring misses the grid.
StaticArray<rhi::RayInput, GRID_RAY_COUNT> MakeGridRays() noexcept
{
    StaticArray<rhi::RayInput, GRID_RAY_COUNT> rays{};
    for (usize row = 0; row < GRID_RAYS_PER_SIDE; ++row)
    {
        for (usize column = 0; column < GRID_RAYS_PER_SIDE; ++column)
        {
            rhi::RayInput& ray = rays[row * GRID_RAYS_PER_SIDE + column];
            ray.Origin[0] = -1.3F + 2.6F * (static_cast<float32>(column) + 0.5F) / GRID_RAYS_PER_SIDE;
            ray.Origin[1] = -1.3F + 2.6F * (static_cast<float32>(row) + 0.5F) / GRID_RAYS_PER_SIDE;
            ray.Origin[2] = 2;
        }
    }
    return rays;
}
bool IsNear(float32 a, float32 b) noexcept
{
    const float32 difference = a > b ? a - b : b - a;
    return difference <= PARITY_TOLERANCE;
}
// One trace of the grid rays.
struct GridTrace final
{
    GridResults Results{};
    // Count hits whose identity or geometry differs from another trace of the same rays.
    [[nodiscard]] usize CountMismatches(const GridTrace& other) const noexcept
    {
        usize mismatches = 0;
        for (usize i = 0; i < GRID_RAY_COUNT; ++i)
        {
            const rhi::RayResult& a = Results[i];
            const rhi::RayResult& b = other.Results[i];
            if (a.Hit != b.Hit || a.Primitive != b.Primitive || a.Instance != b.Instance ||
                IsNear(a.Distance, b.Distance) == false || IsNear(a.U, b.U) == false || IsNear(a.V, b.V) == false)
            {
                ++mismatches;
            }
        }
        return mismatches;
    }
};
} // namespace

TEST_CASE("R16b refits and compacted copies trace like rebuilt structures", "[rhi][gpu][ray][update]")
{
    rhi::Shutdown();
    struct Guard final
    {
        LUDUS_INLINE ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Preferred.RayQueries = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, deviceDescription, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No native GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) == rhi::RasterStatus::Unsupported)
    {
        SKIP("Native ray queries unavailable");
    }
    REQUIRE(capabilities.Updates == true);
    REQUIRE(capabilities.CanCompact == true);
    rhi::RayQueryProgram program;
    REQUIRE(rhi::CreateRayQueryProgram(device, ludus::shaders::ray_query::Query(), program) ==
            rhi::RasterStatus::Ready);
    const StaticArray<uint32, GRID_INDEX_COUNT> indices = MakeGridIndices();
    const StaticArray<rhi::RayInput, GRID_RAY_COUNT> rays = MakeGridRays();
    const GridPositions flat = MakeGridPositions(0);
    const GridPositions bent = MakeGridPositions(0.5F);
    const auto describe = [&indices](const GridPositions& positions, bool isDynamic) noexcept {
        rhi::RayGeometryDescription description;
        description.Positions = positions.GetData();
        description.VertexCount = GRID_VERTEX_COUNT;
        description.Indices = indices.GetData();
        description.IndexCount = indices.GetSize();
        description.ByteBudget = GRID_BYTE_BUDGET;
        description.ShouldAllowUpdate = isDynamic;
        description.ShouldAllowCompaction = isDynamic;
        return description;
    };
    const auto build = [device](const rhi::RayGeometryDescription& description) {
        rhi::RayGeometryHandle geometry;
        REQUIRE(rhi::CreateRayGeometry(device, description, geometry) == rhi::RasterStatus::Pending);
        REQUIRE(Poll(device, geometry) == rhi::RasterStatus::Ready);
        return geometry;
    };
    const auto place = [device](rhi::RayGeometryHandle geometry, float32 offset, bool shouldAllowUpdate) {
        rhi::RayInstanceDescription instance;
        instance.Geometry = geometry;
        instance.Transform[3] = offset;
        rhi::RaySceneHandle scene;
        REQUIRE(rhi::CreateRayScene(device, {&instance, 1, GRID_BYTE_BUDGET, shouldAllowUpdate}, scene) ==
                rhi::RasterStatus::Pending);
        REQUIRE(Poll(device, scene) == rhi::RasterStatus::Ready);
        return scene;
    };
    const auto trace = [device, program, &rays](rhi::RaySceneHandle scene) {
        rhi::RayQueryBatch batch;
        REQUIRE(rhi::SubmitRayQueries(device, program, scene, rays.GetData(), rays.GetSize(), batch) ==
                rhi::RasterStatus::Pending);
        REQUIRE(Poll(device, batch) == rhi::RasterStatus::Ready);
        GridTrace traced;
        REQUIRE(rhi::ReadRayResults(device, batch, traced.Results.GetData(), traced.Results.GetSize()) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, batch) == rhi::RasterStatus::Ready);
        return traced;
    };

    rhi::RayGeometryHandle source = build(describe(flat, true));
    rhi::RaySceneHandle sourceScene = place(source, 0, true);
    const GridTrace flatResults = trace(sourceScene);
    usize hits = 0;
    for (const rhi::RayResult& result : flatResults.Results)
    {
        hits += result.Hit;
    }
    // The outer ring of rays misses; every inner ray hits the grid.
    CHECK(hits > 0);
    CHECK(hits < GRID_RAY_COUNT);

    // A topology change is rejected natively before any work is submitted.
    StaticArray<uint32, GRID_INDEX_COUNT> rewound = indices;
    rewound[1] = indices[2];
    rewound[2] = indices[1];
    rhi::RayGeometryDescription changed = describe(bent, true);
    changed.Indices = rewound.GetData();
    rhi::RayGeometryHandle rejected;
    CHECK(rhi::UpdateRayGeometry(device, source, changed, rejected) == rhi::RasterStatus::InvalidDescription);
    CHECK(rhi::GetStatus(device, rejected) == rhi::RasterStatus::InvalidHandle);

    // Geometry refit against a rebuild from the same deformed positions.
    rhi::RayGeometryHandle refit;
    REQUIRE(rhi::UpdateRayGeometry(device, source, describe(bent, true), refit) == rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, refit) == rhi::RasterStatus::Ready);
    rhi::RayGeometryHandle rebuilt = build(describe(bent, false));
    rhi::RaySceneHandle refitScene = place(refit, 0, false);
    rhi::RaySceneHandle rebuiltScene = place(rebuilt, 0, false);
    const GridTrace bentResults = trace(rebuiltScene);
    const GridTrace refitResults = trace(refitScene);
    CHECK(bentResults.CountMismatches(refitResults) == 0);
    CHECK(flatResults.CountMismatches(bentResults) > 0);
    // The source incarnation is not mutated by its refit.
    const GridTrace sourceAfterRefit = trace(sourceScene);
    CHECK(flatResults.CountMismatches(sourceAfterRefit) == 0);

    // Scene refit against a rebuild with the same moved instance.
    constexpr float32 MOVED_OFFSET = 0.3F;
    rhi::RayInstanceDescription moved;
    moved.Geometry = source;
    moved.Transform[3] = MOVED_OFFSET;
    rhi::RaySceneHandle refitSourceScene;
    REQUIRE(rhi::UpdateRayScene(device, sourceScene, {&moved, 1, GRID_BYTE_BUDGET, true}, refitSourceScene) ==
            rhi::RasterStatus::Pending);
    REQUIRE(Poll(device, refitSourceScene) == rhi::RasterStatus::Ready);
    rhi::RaySceneHandle movedScene = place(source, MOVED_OFFSET, false);
    const GridTrace movedResults = trace(movedScene);
    const GridTrace refitMovedResults = trace(refitSourceScene);
    CHECK(movedResults.CountMismatches(refitMovedResults) == 0);
    CHECK(flatResults.CountMismatches(movedResults) > 0);
    const GridTrace sourceAfterSceneRefit = trace(sourceScene);
    CHECK(flatResults.CountMismatches(sourceAfterSceneRefit) == 0);

    // Compaction keeps the traced result and owns fewer resident bytes than its source.
    rhi::RayAllocation sourceBytes;
    REQUIRE(rhi::GetRayAllocation(device, source, sourceBytes) == rhi::RasterStatus::Ready);
    rhi::RayGeometryHandle compacted;
    REQUIRE(rhi::CompactRayGeometry(device, {source, GRID_BYTE_BUDGET}, compacted) == rhi::RasterStatus::Pending);
    REQUIRE(rhi::Destroy(device, movedScene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, refitSourceScene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, sourceScene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, source) == rhi::RasterStatus::Ready);
    REQUIRE(Poll(device, compacted) == rhi::RasterStatus::Ready);
    rhi::RayAllocation compactedBytes;
    REQUIRE(rhi::GetRayAllocation(device, compacted, compactedBytes) == rhi::RasterStatus::Ready);
    CHECK(compactedBytes.ResidentBytes > 0);
    CHECK(compactedBytes.ResidentBytes < sourceBytes.ResidentBytes);
    CHECK(compactedBytes.ScratchBytes == 0);
    rhi::RaySceneHandle compactedScene = place(compacted, 0, false);
    const GridTrace compactedResults = trace(compactedScene);
    CHECK(flatResults.CountMismatches(compactedResults) == 0);
    // Once the copy completed, the detached source and its scenes retired.
    rhi::RayResourceStats stats;
    REQUIRE(rhi::GetRayResourceStats(device, stats) == rhi::RasterStatus::Ready);
    CHECK(stats.Geometries == 3);
    CHECK(stats.Scenes == 3);
    CHECK(stats.DetachedBytes == 0);
    CHECK(stats.BuildScratchBytes == 0);
    rhi::RayGeometryHandle immutable;
    CHECK(rhi::UpdateRayGeometry(device, compacted, describe(bent, true), immutable) ==
          rhi::RasterStatus::InvalidState);

    REQUIRE(rhi::Destroy(device, compactedScene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, refitScene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, rebuiltScene) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, compacted) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, refit) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, rebuilt) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, program) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetRayResourceStats(device, stats) == rhi::RasterStatus::Ready);
    CHECK(stats.Geometries == 0);
    CHECK(stats.Scenes == 0);
    CHECK(stats.ResidentBytes == 0);
}
