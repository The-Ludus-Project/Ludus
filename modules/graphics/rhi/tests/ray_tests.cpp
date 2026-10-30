#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/ray.h>

#include "ray_query.h"

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
    // RT0 never negotiates procedural intersections or geometry updates.
    CHECK(capabilities.ProceduralIntersections == false);
    CHECK(capabilities.Updates == false);
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
    CHECK_FALSE(capabilities.Updates);
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
