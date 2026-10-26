#include "../src/internal/gpu_preview_ray.h"
#include "../src/internal/render_graph_ray.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_gpu_preview.h"
#include "renderer_gpu_preview_image.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/measurement.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
using Status = rhi::RasterStatus;
constexpr uint64 TIMEOUT = 30 * time::NANOSECONDS_PER_SECOND;
constexpr usize BYTE_BUDGET = usize{64} * 1024 * 1024;
constexpr usize COLOR_CHANNELS = 4;
constexpr uint32 SCENE_COUNT = 4;
constexpr uint32 GRID_CELLS = 32;
constexpr usize CORNER_COUNT = 3;
constexpr usize TRIANGLES_PER_CELL = 2;
constexpr uint32 TEST_EXTENT = 64;
constexpr uint32 BENCHMARK_EXTENT = 512;
constexpr uint32 TEST_SAMPLES = 16;
constexpr uint32 BENCHMARK_SAMPLES = 48;
constexpr uint32 WARMUP_SAMPLES = 8;
constexpr usize READBACK_ROWS = 16;
constexpr float32 DISPLAY_TOLERANCE = 2;
constexpr float32 LINEAR_ABSOLUTE_TOLERANCE = .001F;
constexpr float32 LINEAR_RELATIVE_TOLERANCE = .005F;
constexpr usize ACCUMULATION_WORDS_PER_PIXEL = 2;
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr float32 SRGB_THRESHOLD = .0031308F;
constexpr float32 SRGB_LINEAR_SCALE = 12.92F;
constexpr float32 SRGB_POWER_SCALE = 1.055F;
constexpr float32 SRGB_POWER_OFFSET = .055F;
constexpr float32 SRGB_GAMMA = 2.4F;
constexpr float32 BYTE_MAXIMUM = 255;

bool IsAccepted(Status status)
{
    return status == Status::Ready || status == Status::Pending;
}
template <typename Handle>
void WaitReady(rr::Renderer& inoutRenderer, Handle handle)
{
    const uint64 start = time::NowTicks();
    Status status = inoutRenderer.GetStatus(handle);
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        status = inoutRenderer.GetStatus(handle);
    }
    REQUIRE(status == Status::Ready);
}
void WaitSamples(rr::Renderer& inoutRenderer, rr::GpuPreview preview, uint32 samples)
{
    const uint64 start = time::NowTicks();
    rr::GpuPreviewInfo information;
    Status status = Status::Pending;
    do
    {
        status = inoutRenderer.GetGpuPreviewInfo(preview, information);
        REQUIRE(IsAccepted(status) == true);
    } while ((status == Status::Pending || information.CompletedSamples < samples) &&
             time::NowTicks() - start < TIMEOUT);
    REQUIRE(information.CompletedSamples == samples);
}
void WriteArtifact(const char* directory, const char* sceneName, const char* suffix, const void* data, usize bytes)
{
    const std::string path = std::string(directory) + "/" + sceneName + suffix;
    std::FILE* fileOrNull = std::fopen(path.c_str(), "wb");
    REQUIRE(fileOrNull != nullptr);
    CHECK(std::fwrite(data, 1, bytes, fileOrNull) == bytes);
    CHECK(std::fclose(fileOrNull) == 0);
}
uint8 EncodeDisplay(float32 value)
{
    value /= 1 + value;
    const float32 encoded = value <= SRGB_THRESHOLD
                                ? value * SRGB_LINEAR_SCALE
                                : SRGB_POWER_SCALE * std::pow(value, 1 / SRGB_GAMMA) - SRGB_POWER_OFFSET;
    return static_cast<uint8>(std::lround(encoded * BYTE_MAXIMUM));
}
void ReadImage(rhi::DeviceHandle device, rr::GpuPreviewImage image, Array<uint8>& outBytes)
{
    const usize rowBytes = usize(image.Width) * COLOR_CHANNELS;
    REQUIRE(outBytes.TryResize(rowBytes * image.Height) == true);
    for (uint32 y = 0; y < image.Height; y += READBACK_ROWS)
    {
        const uint32 rows = math::Min(uint32(READBACK_ROWS), image.Height - y);
        rhi::ReadbackTicket ticket;
        REQUIRE(
            IsAccepted(
                rhi::RequestTextureReadback(device, image.Texture, {0, 0, y, image.Width, rows}, rowBytes, ticket)) ==
            true);
        const uint64 start = time::NowTicks();
        Status status = rhi::GetStatus(device, ticket);
        while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
        {
            status = rhi::GetStatus(device, ticket);
        }
        REQUIRE(status == Status::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, outBytes.GetData() + usize(y) * rowBytes, rows * rowBytes) ==
                Status::Ready);
        REQUIRE(rhi::Release(device, ticket) == Status::Ready);
    }
}
} // namespace

TEST_CASE("Opacity traversal preserves rendered scenes and CPU/GPU reference agreement", "[renderer][gpu][opacity]")
{
    const char* directoryOrNull = std::getenv("LUDUS_OPACITY_BENCHMARK_DIRECTORY");
    const bool isBenchmark = directoryOrNull != nullptr;
    const bool shouldPartition = std::getenv("LUDUS_OPACITY_BASELINE") == nullptr;
    rr::internal::SetGpuPreviewOpacityPartitioningForTesting(shouldPartition);
    constexpr StaticArray<const char*, SCENE_COUNT> SCENE_NAMES{{"empty", "opaque", "masked", "mixed"}};
    StaticArray<Array<rr::ReferencePixel>, SCENE_COUNT> cpuReferences{};
    const usize traversalCount = isBenchmark == true ? 1 : 2;
    for (usize traversal = 0; traversal < traversalCount; ++traversal)
    {
        const bool shouldUseReferenceCompute = traversal != 0;
        for (uint32 sceneKind = 0; sceneKind < SCENE_COUNT; ++sceneKind)
        {
            const char* selectedSceneOrNull = std::getenv("LUDUS_OPACITY_SCENE");
            if (selectedSceneOrNull != nullptr && std::string_view(selectedSceneOrNull) != SCENE_NAMES[sceneKind])
            {
                continue;
            }
            CAPTURE(sceneKind, shouldUseReferenceCompute);
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
            deviceDescription.Required.PortableRaster = true;
            deviceDescription.Required.Compute = true;
            deviceDescription.Preferred.RayQueries = true;
            deviceDescription.EnableGpuTiming = isBenchmark;
            const rhi::DeviceStatus started = rhi::CreateDevice({},
                                                                { .Width = TEST_EXTENT, .Height = TEST_EXTENT },
                                                                deviceDescription,
                                                                device,
                                                                surface);
            if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
            {
                SKIP("No native GPU adapter");
            }
            REQUIRE(started == rhi::DeviceStatus::Ready);
            rhi::RayCapabilities capabilities;
            if (rhi::GetRayCapabilities(device, capabilities) == Status::Unsupported)
            {
                SKIP("Hardware ray queries unavailable");
            }
            rr::Renderer renderer;
            REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
                    Status::Ready);
            if (shouldUseReferenceCompute == true)
            {
                rr::ReferenceRenderingOptions options;
                options.Traversal = rr::ReferenceTraversal::GpuCompute;
                options.CpuByteBudget = BYTE_BUDGET;
                options.GpuByteBudget = BYTE_BUDGET;
                options.TilePixels = TEST_EXTENT;
                REQUIRE(renderer.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                          shaders::renderer_reference_reduce::Compute(),
                                                          options) == Status::Ready);
            }
            else
            {
                REQUIRE(renderer.EnableGpuPreview(shaders::renderer_gpu_preview::Compute(),
                                                  shaders::renderer_gpu_preview_image::Vertex(),
                                                  shaders::renderer_gpu_preview_image::Fragment(),
                                                  {BYTE_BUDGET, BYTE_BUDGET, 1}) == Status::Ready);
            }
            REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(),
                                             shaders::renderer_composite::Fragment()) == Status::Ready);
            REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                                shaders::renderer_output::Fragment(),
                                                shaders::renderer_shadow::Fragment()) == Status::Ready);
            rr::MaterialProgram program;
            REQUIRE(IsAccepted(renderer.PrewarmLitProgram(shaders::renderer_lit::Vertex(),
                                                          shaders::renderer_lit::Fragment(),
                                                          shaders::renderer_lit_shadow::Fragment(),
                                                          program)) == true);
            WaitReady(renderer, program);
            constexpr usize VERTEX_COUNT = usize(GRID_CELLS) * GRID_CELLS * TRIANGLES_PER_CELL * CORNER_COUNT;
            Array<math::Vector3> positionsMesh;
            Array<math::Vector3> normalsMesh;
            Array<uint32> indices;
            REQUIRE(positionsMesh.TryResize(VERTEX_COUNT) == true);
            REQUIRE(normalsMesh.TryResize(VERTEX_COUNT) == true);
            REQUIRE(indices.TryResize(VERTEX_COUNT) == true);
            usize vertex = 0;
            for (uint32 y = 0; y < GRID_CELLS; ++y)
            {
                for (uint32 x = 0; x < GRID_CELLS; ++x)
                {
                    const float32 left = float32(x) * 2 / float32(GRID_CELLS) - 1;
                    const float32 right = float32(x + 1) * 2 / float32(GRID_CELLS) - 1;
                    const float32 bottom = float32(y) * 2 / float32(GRID_CELLS) - 1;
                    const float32 top = float32(y + 1) * 2 / float32(GRID_CELLS) - 1;
                    const StaticArray<math::Vector3, TRIANGLES_PER_CELL * CORNER_COUNT> cornersMesh{{{left, bottom, 0},
                                                                                                     {right, top, 0},
                                                                                                     {right, bottom, 0},
                                                                                                     {left, bottom, 0},
                                                                                                     {left, top, 0},
                                                                                                     {right, top, 0}}};
                    for (const math::Vector3& corner : cornersMesh)
                    {
                        positionsMesh[vertex] = corner;
                        normalsMesh[vertex] = {0, 0, -1};
                        indices[vertex] = static_cast<uint32>(vertex);
                        ++vertex;
                    }
                }
            }
            rr::Mesh mesh;
            rr::MeshDescription meshDescription{positionsMesh.GetData(),
                                                positionsMesh.GetSize(),
                                                indices.GetData(),
                                                indices.GetSize()};
            meshDescription.Normals = normalsMesh.GetData();
            REQUIRE(IsAccepted(renderer.CreateMesh(meshDescription, mesh)) == true);
            WaitReady(renderer, mesh);
            constexpr usize MATERIAL_COUNT = 3;
            StaticArray<rr::MaterialVersion, MATERIAL_COUNT> materials{};
            for (usize i = 0; i < MATERIAL_COUNT; ++i)
            {
                rr::LitMaterialDescription material;
                material.Surface.Tint = {1, 1, 1, i == 0 ? 0.0F : 1.0F};
                material.Surface.Alpha =
                    i == 2 && sceneKind != 2 ? rr::MaterialAlpha::Opaque : rr::MaterialAlpha::Masked;
                material.Surface.Tint.X = material.Surface.Tint.Y = material.Surface.Tint.Z = .5F;
                material.Roughness = 1;
                material.Emission = i == 0   ? math::Vector3{1, 0, 0}
                                    : i == 1 ? math::Vector3{0, .25F, 1}
                                             : math::Vector3{0, .5F, .25F};
                rr::LightSelection selection;
                REQUIRE(IsAccepted(renderer.CreateLitMaterial(program, material, materials[i], selection)) == true);
                WaitReady(renderer, materials[i]);
            }
            constexpr usize ITEM_COUNT = 3;
            StaticArray<rr::SceneItem, ITEM_COUNT> items{};
            for (usize i = 0; i < ITEM_COUNT; ++i)
            {
                items[i].SourceId = i + 1;
                items[i].Geometry = mesh;
                items[i].Surface = sceneKind == 1 ? materials[2] : materials[i];
                items[i].Transform.Translation.Z = float32(i + 1) / float32(ITEM_COUNT + 1);
            }
            // The middle masked patch accepts while the first rejects. Opaque geometry remains
            // visible outside the patch, exercising both native partitions in the same image.
            items[1].Transform.Columns[0].X = .5F;
            items[1].Transform.Columns[1].Y = .5F;
            rr::Snapshot snapshot;
            REQUIRE(renderer.CreateSnapshot(items.GetData(), sceneKind == 0 ? 0 : items.GetSize(), snapshot) ==
                    Status::Ready);
            rr::DirectLight light;
            light.SourceId = 1;
            light.Direction = {.6F, 0, .8F};
            light.Intensity = 1;
            rr::ReferenceCaptureDescription capture;
            capture.Width = capture.Height = isBenchmark ? BENCHMARK_EXTENT : TEST_EXTENT;
            capture.TargetSamples = isBenchmark ? BENCHMARK_SAMPLES : TEST_SAMPLES;
            capture.MaxDepth = 2;
            capture.RouletteDepth = 2;
            capture.OrthographicCamera = true;
            capture.Seed = 1;
            capture.Environment = {.25F, .5F, 1};
            capture.Lights = &light;
            capture.LightCount = 1;
            if (shouldUseReferenceCompute == true)
            {
                rr::ReferenceCapture computeCapture;
                REQUIRE(IsAccepted(renderer.BeginReferenceCapture(snapshot, capture, computeCapture)) == true);
                const uint64 start = time::NowTicks();
                Status status = Status::Pending;
                while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
                    status = renderer.AdvanceReferenceCapture(computeCapture, surface);
                REQUIRE(status == Status::Ready);
                const Array<rr::ReferencePixel>& expected = cpuReferences[sceneKind];
                REQUIRE(expected.GetSize() == usize(capture.Width) * capture.Height);
                Array<rr::ReferencePixel> actual;
                REQUIRE(actual.TryResize(expected.GetSize()) == true);
                REQUIRE(renderer.CopyReferencePixels(computeCapture, actual.GetData(), actual.GetSize()) ==
                        Status::Ready);
                for (usize pixel = 0; pixel < expected.GetSize(); ++pixel)
                {
                    CAPTURE(pixel);
                    CHECK(actual[pixel].Samples == capture.TargetSamples);
                    const math::Vector3 difference = actual[pixel].Radiance - expected[pixel].Radiance;
                    CHECK(math::Abs(difference.X) <=
                          LINEAR_ABSOLUTE_TOLERANCE +
                              LINEAR_RELATIVE_TOLERANCE * math::Abs(expected[pixel].Radiance.X));
                    CHECK(math::Abs(difference.Y) <=
                          LINEAR_ABSOLUTE_TOLERANCE +
                              LINEAR_RELATIVE_TOLERANCE * math::Abs(expected[pixel].Radiance.Y));
                    CHECK(math::Abs(difference.Z) <=
                          LINEAR_ABSOLUTE_TOLERANCE +
                              LINEAR_RELATIVE_TOLERANCE * math::Abs(expected[pixel].Radiance.Z));
                }
                REQUIRE(renderer.Release(computeCapture) == Status::Ready);
                rhi::RayResourceStats rayResources;
                REQUIRE(rhi::GetRayResourceStats(device, rayResources) == Status::Ready);
                CHECK(rayResources.Geometries == 0);
                CHECK(rayResources.Scenes == 0);
                continue;
            }
            rr::GpuPreview preview;
            const uint64 setupStart = time::NowTicks();
            REQUIRE(IsAccepted(renderer.BeginGpuPreview(snapshot, capture, preview)) == true);
            WaitSamples(renderer, preview, 0);
            const uint64 setupNanoseconds = time::NowTicks() - setupStart;
            rhi::RayResourceStats rayResources;
            REQUIRE(rhi::GetRayResourceStats(device, rayResources) == Status::Ready);
            CHECK(rayResources.Geometries == (sceneKind == 0 ? 0 : 1));
            Array<float64> gpuTimes;
            Array<uint64> wallTimes;
            for (uint32 sample = 1; sample <= capture.TargetSamples; ++sample)
            {
                if (isBenchmark == true)
                    REQUIRE(rhi::ArmGpuTiming(device) == Status::Ready);
                const uint64 start = time::NowTicks();
                rhi::SubmissionToken token;
                REQUIRE(renderer.AdvanceGpuPreview(preview, surface, token) == Status::Ready);
                WaitSamples(renderer, preview, sample);
                const uint64 elapsed = time::NowTicks() - start;
                if (isBenchmark == true)
                {
                    rhi::GpuTiming timing;
                    REQUIRE(rhi::GetGpuTiming(device, timing) == Status::Ready);
                    REQUIRE(timing.State == rhi::GpuTimingState::Available);
                    if (sample > WARMUP_SAMPLES)
                    {
                        REQUIRE(gpuTimes.TryAdd(timing.DurationNanoseconds) == true);
                        REQUIRE(wallTimes.TryAdd(elapsed) == true);
                    }
                }
            }
            rr::GpuPreviewFrame frame;
            rr::GpuPreviewImage image;
            REQUIRE(renderer.AcquireGpuPreviewFrame(preview, frame, image) == Status::Ready);
            Array<uint8> pixels;
            ReadImage(device, image, pixels);
            rhi::BufferHandle accumulation;
            REQUIRE(rr::internal::RenderGraphRayPass::GetAccumulationForTesting(renderer, preview, accumulation) ==
                    Status::Ready);
            const usize pixelCount = usize(capture.Width) * capture.Height;
            Array<math::Vector4> linear;
            REQUIRE(linear.TryResize(pixelCount * ACCUMULATION_WORDS_PER_PIXEL) == true);
            const usize linearBytes = linear.GetSize() * sizeof(math::Vector4);
            for (usize offset = 0; offset < linearBytes; offset += READBACK_CHUNK_BYTES)
            {
                const usize bytes = math::Min(READBACK_CHUNK_BYTES, linearBytes - offset);
                rhi::ReadbackTicket ticket;
                Status status = rhi::RequestBufferReadback(device, accumulation, offset, bytes, ticket);
                const uint64 start = time::NowTicks();
                while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
                {
                    status = rhi::GetStatus(device, ticket);
                }
                REQUIRE(status == Status::Ready);
                REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(linear.GetData()) + offset, bytes) ==
                        Status::Ready);
                REQUIRE(rhi::Release(device, ticket) == Status::Ready);
            }
            for (usize pixel = 0; pixel < pixelCount; ++pixel)
            {
                math::Vector4& sum = linear[pixel * ACCUMULATION_WORDS_PER_PIXEL];
                REQUIRE(sum.W == float32(capture.TargetSamples));
                sum.X /= sum.W;
                sum.Y /= sum.W;
                sum.Z /= sum.W;
            }
            if (isBenchmark == true)
            {
                WriteArtifact(directoryOrNull, SCENE_NAMES[sceneKind], ".linear.bin", linear.GetData(), linearBytes);
                WriteArtifact(directoryOrNull, SCENE_NAMES[sceneKind], ".rgba", pixels.GetData(), pixels.GetSize());
                WriteArtifact(directoryOrNull,
                              SCENE_NAMES[sceneKind],
                              ".gpu.bin",
                              gpuTimes.GetData(),
                              gpuTimes.GetSize() * sizeof(float64));
                WriteArtifact(directoryOrNull,
                              SCENE_NAMES[sceneKind],
                              ".wall.bin",
                              wallTimes.GetData(),
                              wallTimes.GetSize() * sizeof(uint64));
                WriteArtifact(directoryOrNull,
                              SCENE_NAMES[sceneKind],
                              ".setup.bin",
                              &setupNanoseconds,
                              sizeof(setupNanoseconds));
            }
            else
            {
                rr::ReferenceCapture oracle;
                REQUIRE(renderer.BeginReferenceCapture(snapshot, capture, oracle) == Status::Ready);
                const uint64 start = time::NowTicks();
                Status status = Status::Pending;
                while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
                    status = renderer.AdvanceReferenceCapture(oracle);
                REQUIRE(status == Status::Ready);
                Array<rr::ReferencePixel>& expected = cpuReferences[sceneKind];
                REQUIRE(expected.TryResize(usize(TEST_EXTENT) * TEST_EXTENT) == true);
                REQUIRE(renderer.CopyReferencePixels(oracle, expected.GetData(), expected.GetSize()) == Status::Ready);
                for (usize pixel = 0; pixel < expected.GetSize(); ++pixel)
                {
                    const math::Vector3 radiance = expected[pixel].Radiance;
                    const StaticArray<float32, CORNER_COUNT> channels{{radiance.X, radiance.Y, radiance.Z}};
                    for (usize channel = 0; channel < channels.GetSize(); ++channel)
                    {
                        CAPTURE(pixel, channel);
                        CHECK(math::Abs(float32(pixels[pixel * COLOR_CHANNELS + channel]) -
                                        float32(EncodeDisplay(channels[channel]))) <= DISPLAY_TOLERANCE);
                        const math::Vector4& actual = linear[pixel * ACCUMULATION_WORDS_PER_PIXEL];
                        const StaticArray<float32, CORNER_COUNT> actualChannels{{actual.X, actual.Y, actual.Z}};
                        CHECK(math::Abs(actualChannels[channel] - channels[channel]) <=
                              LINEAR_ABSOLUTE_TOLERANCE + LINEAR_RELATIVE_TOLERANCE * math::Abs(channels[channel]));
                    }
                }
                REQUIRE(renderer.Release(oracle) == Status::Ready);
            }
            REQUIRE(renderer.Release(frame) == Status::Ready);
            REQUIRE(renderer.Release(preview) == Status::Ready);
            const uint64 retirementStart = time::NowTicks();
            do
            {
                REQUIRE(rhi::PollLifetime(device) == Status::Ready);
                REQUIRE(renderer.PollGpuPreview() == Status::Ready);
                REQUIRE(rhi::GetRayResourceStats(device, rayResources) == Status::Ready);
            } while (rayResources.Geometries != 0 && time::NowTicks() - retirementStart < TIMEOUT);
            CHECK(rayResources.Geometries == 0);
            CHECK(rayResources.Scenes == 0);
        }
    }
}

TEST_CASE("Opacity range build respects the combined allocation budget", "[renderer][gpu][opacity-budget]")
{
    rr::internal::SetGpuPreviewOpacityPartitioningForTesting(true);
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
    rhi::DeviceDescription description;
    description.Preferred.RayQueries = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = TEST_EXTENT, .Height = TEST_EXTENT }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No native GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(device, capabilities) == Status::Unsupported)
    {
        SKIP("Hardware ray queries unavailable");
    }
    namespace internal = rr::internal;
    constexpr usize TRIANGLE_COUNT = 2;
    internal::ReferenceSceneData scene;
    StaticArray<internal::ReferenceSurfaceData, TRIANGLE_COUNT> surfaces{};
    surfaces[1].Alpha = rr::MaterialAlpha::Masked;
    REQUIRE(scene.Instances.TryResize(TRIANGLE_COUNT) == true);
    REQUIRE(scene.Triangles.TryResize(TRIANGLE_COUNT) == true);
    REQUIRE(scene.Order.TryAdd(0) == true);
    for (usize i = 0; i < TRIANGLE_COUNT; ++i)
    {
        scene.Instances[i].Surface = &surfaces[i];
        internal::ReferenceTriangle& triangle = scene.Triangles[i];
        triangle.Instance = static_cast<uint32>(i);
        triangle.Positions[0] = {-1, -1, 0};
        triangle.Positions[1] = {1, -1, 0};
        triangle.Positions[2] = {0, 1, 0};
    }
    REQUIRE(scene.Order.TryAdd(1) == true);
    rhi::RayGeometryHandle geometry;
    rhi::RayAllocation allocation;
    REQUIRE(IsAccepted(
                internal::CreateGpuPreviewRayGeometry(device, scene, BYTE_BUDGET, BYTE_BUDGET, geometry, allocation)) ==
            true);
    rhi::RayGeometryHandle rejected;
    rhi::RayAllocation rejectedAllocation;
    const usize buildBytes = allocation.ResidentBytes + allocation.ScratchBytes;
    CHECK(internal::CreateGpuPreviewRayGeometry(device,
                                                scene,
                                                BYTE_BUDGET,
                                                buildBytes - 1,
                                                rejected,
                                                rejectedAllocation) == Status::CapacityExceeded);
    CHECK(rhi::GetStatus(device, rejected) == Status::InvalidHandle);
    REQUIRE(rhi::Destroy(device, geometry) == Status::Ready);
    rhi::RayResourceStats resources;
    const uint64 start = time::NowTicks();
    do
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        REQUIRE(rhi::GetRayResourceStats(device, resources) == Status::Ready);
    } while (resources.Geometries != 0 && time::NowTicks() - start < TIMEOUT);
    CHECK(resources.Geometries == 0);
    CHECK(resources.ResidentBytes == 0);
    CHECK(resources.BuildScratchBytes == 0);
}
