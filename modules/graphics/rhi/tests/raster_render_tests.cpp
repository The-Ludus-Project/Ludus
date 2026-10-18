#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ludus/foundation/base/types.h>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <thread>

#include "cull.h"
#include "internal/readback.h"
#include "merged_frame.h"
#include "ordered_frame.h"
#include "raster.h"

#include <span>

#include <catch2/catch_test_macros.hpp>
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
template <typename T, usize N>
std::span<const uint8> RasterBytes(const T (&data)[N]) noexcept
{
    return {reinterpret_cast<const uint8*>(data), sizeof(data)};
}
} // namespace
TEST_CASE("Portable indexed instances preserve texture origin, depth and detached binding snapshots",
          "[rhi][gpu][raster]")
{
    int32 mode = 0;
    bool forceTiming = false;
    SECTION("Linear RGBA8 sampling and depth") {}
    SECTION("sRGB sampling decodes to linear output")
    {
        mode = 1;
    }
    SECTION("Premultiplied alpha blends over the frame clear")
    {
        mode = 2;
    }
    SECTION("Ordered graph optimizes unobservable passes")
    {
        mode = 3;
    }
    SECTION("Ordered graph reference keeps every legal pass")
    {
        mode = 4;
    }
    SECTION("Compute culling and indexed indirect drawing")
    {
        mode = 5;
    }
    SECTION("Authored CPU culling and direct drawing")
    {
        mode = 6;
    }
    SECTION("R5 merged painter order matches unmerged complete image")
    {
        mode = 7;
    }
    SECTION("R7 instrumented graph frames and query reuse")
    {
        mode = 7;
        forceTiming = true;
    }
    const bool graphMode = mode >= 3;
    const bool computeMode = mode == 5;
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::Shutdown();
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.EnableGpuTiming = forceTiming || std::getenv("LUDUS_RHI_GPU_TIMING") != nullptr;
    description.Required.PortableRaster = true;
    description.Required.Compute = computeMode;
    const auto started = rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        REQUIRE(rhi::GetStartup().State == rhi::StartupState::Idle);
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    const auto vertex = ludus::shaders::raster::Vertex();
    const auto fragment = ludus::shaders::raster::Fragment();
    rhi::RasterShaderHandle vs, fs;
    REQUIRE(rhi::CreateRasterShader(device, vertex, vs) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(device, fragment, fs) == rhi::RasterStatus::Ready);
    rhi::BindingLayoutHandle layout;
    REQUIRE(rhi::CreateBindingLayout(device, fragment.Bindings, layout) == rhi::RasterStatus::Ready);
    const float32 tint[4]{1, 1, 1, 1};
    const float32 dim[4]{mode == 7 ? .25F : .5F, .5F, mode == 7 ? .125F : .5F, mode == 7 ? .5F : 1};
    rhi::BufferHandle uniform, backgroundUniform;
    REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Uniform, sizeof(tint)}, RasterBytes(tint), uniform) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Uniform, sizeof(dim)}, RasterBytes(dim), backgroundUniform) ==
            rhi::RasterStatus::Ready);
    uint8 image[16]{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    if (mode == 1 || mode == 2)
    {
        for (usize i = 0; i < sizeof(image); ++i)
        {
            if (i % 4 == 3)
            {
                image[i] = mode == 2 ? 128 : 255;
            }
            else if (image[i] != 0)
            {
                image[i] = 128;
            }
        }
    }
    rhi::TextureHandle texture;
    rhi::TextureViewHandle view;
    rhi::SamplerHandle sampler;
    REQUIRE(rhi::CreateTexture(device,
                               {2, 2, mode == 1 ? rhi::RasterFormat::Rgba8Srgb : rhi::RasterFormat::Rgba8Unorm},
                               {image, 8},
                               texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateTextureView(device, texture, view) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateSampler(device, {}, sampler) == rhi::RasterStatus::Ready);
    rhi::RasterBindingResource resources[3];
    resources[0].Binding = 0;
    resources[0].Buffer = uniform;
    resources[0].Size = 16;
    resources[1].Binding = 1;
    resources[1].Texture = view;
    resources[2].Binding = 2;
    resources[2].Sampler = sampler;
    rhi::BindingSetHandle set, backgroundSet;
    REQUIRE(rhi::CreateBindingSet(device, layout, resources, set) == rhi::RasterStatus::Ready);
    resources[0].Buffer = backgroundUniform;
    REQUIRE(rhi::CreateBindingSet(device, layout, resources, backgroundSet) == rhi::RasterStatus::Ready);
    if (graphMode == false)
    {
        // Public destruction detaches ownership while snapshots still retain records.
        REQUIRE(rhi::Destroy(device, texture) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, view) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, sampler) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, uniform) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, backgroundUniform) == rhi::RasterStatus::Ready);
    }
    const rhi::RasterVertexStream streams[2]{{20, false}, {8, true}};
    const rhi::RasterVertexAttribute attributes[3]{{0, 0, 0, rhi::RasterVertexFormat::Float3},
                                                   {1, 0, 12, rhi::RasterVertexFormat::Float2},
                                                   {2, 1, 0, rhi::RasterVertexFormat::Float2}};
    rhi::PipelineRequest request;
    REQUIRE(rhi::RequestPipeline(device,
                                 {vs,
                                  fs,
                                  layout,
                                  streams,
                                  attributes,
                                  true,
                                  mode == 2,
                                  graphMode ? rhi::RasterTarget::Rgba8Unorm : rhi::RasterTarget::Surface},
                                 request) == rhi::RasterStatus::Pending);
    REQUIRE(rhi::PollLifetime(device) == rhi::RasterStatus::Ready);
    rhi::RasterPipelineHandle pipeline;
    REQUIRE(rhi::GetRequestedPipeline(device, request, pipeline) == rhi::RasterStatus::Ready);
    const float32 points[20]{-.4F, -.75F, .25F, 0, 1, -.4F, .75F,  .25F, 0, 0,
                             .4F,  .75F,  .25F, 1, 0, .4F,  -.75F, .25F, 1, 1};
    const float32 back[20]{-.95F, -.95F, .75F, 0, 1, -.95F, .95F,  .75F, 0, 0,
                           .95F,  .95F,  .75F, 1, 0, .95F,  -.95F, .75F, 1, 1};
    float32 offsets[4]{};
    REQUIRE(ludus::qa::CullCpuInstances(offsets) == 2);
    const float32 undefinedOffsets[4]{};
    uint8 computePixels[96 * 64 * 4]{};
    const float32 center[2]{0, 0};
    const uint16 indices[6]{0, 1, 2, 0, 2, 3};
    rhi::BufferHandle vertices, background, instances, centerInstance, index;
    REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Vertex, sizeof(points)}, RasterBytes(points), vertices) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Vertex, sizeof(back)}, RasterBytes(back), background) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device,
                              {computeMode ? rhi::BufferRole::StorageVertex : rhi::BufferRole::Vertex, sizeof(offsets)},
                              computeMode ? RasterBytes(undefinedOffsets) : RasterBytes(offsets),
                              instances) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Vertex, sizeof(center)}, RasterBytes(center), centerInstance) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Index16, sizeof(indices)}, RasterBytes(indices), index) ==
            rhi::RasterStatus::Ready);
    rhi::RasterVertexSlice slices[2]{{vertices, 0, sizeof(points)}, {instances, 0, sizeof(offsets)}};
    const rhi::RasterDraw draw{pipeline, set, slices, index, 0, 6, 4, 2};
    const float64 clear = mode == 2 ? .25 : 0;
    REQUIRE(rhi::SetFrameTarget(device, surface, {96, 64, clear, clear, clear, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    if (graphMode)
    {
        rhi::TextureHandle offscreen, unused;
        rhi::TextureViewHandle offscreenView;
        rhi::BindingSetHandle compositeSet;
        rhi::RasterPipelineHandle ui, composite;
        rhi::BufferHandle fullscreen;
        REQUIRE(rhi::CreateTexture(device, {96, 64, rhi::RasterFormat::Rgba8Unorm, true}, {}, offscreen) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateTexture(device, {96, 64, rhi::RasterFormat::Rgba8Unorm, true}, {}, unused) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateTextureView(device, offscreen, offscreenView) == rhi::RasterStatus::Ready);
        resources[0].Buffer = uniform;
        resources[1].Texture = offscreenView;
        REQUIRE(rhi::CreateBindingSet(device, layout, resources, compositeSet) == rhi::RasterStatus::Ready);
        rhi::RasterPipelineDescription
            uiDescription{vs, fs, layout, streams, attributes, true, false, rhi::RasterTarget::Rgba8Unorm, false};
        if (mode == 7)
        {
            uiDescription.Depth = false;
            uiDescription.Blend = true;
        }
        REQUIRE(rhi::CreateRasterPipeline(device, uiDescription, ui) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateRasterPipeline(device, {vs, fs, layout, streams, attributes}, composite) ==
                rhi::RasterStatus::Ready);
        const float32 full[20]{-1, -1, 0, 0, 1, -1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, -1, 0, 1, 1};
        REQUIRE(rhi::CreateBuffer(device, {rhi::BufferRole::Vertex, sizeof(full)}, RasterBytes(full), fullscreen) ==
                rhi::RasterStatus::Ready);
        ludus::qa::OrderedFrameInputs inputs{vertices,
                                             background,
                                             instances,
                                             centerInstance,
                                             index,
                                             uniform,
                                             backgroundUniform,
                                             fullscreen,
                                             texture,
                                             offscreen,
                                             unused,
                                             pipeline,
                                             ui,
                                             composite,
                                             set,
                                             backgroundSet,
                                             compositeSet,
                                             false};
        rhi::ComputeDispatch dispatch;
        rhi::BufferHandle candidates, arguments;
        if (computeMode)
        {
            const auto& authored = ludus::qa::CULL_CANDIDATES;
            const uint32 zero[5]{};
            REQUIRE(rhi::CreateBuffer(device,
                                      {rhi::BufferRole::Storage, sizeof(authored)},
                                      RasterBytes(authored),
                                      candidates) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::CreateBuffer(device,
                                      {rhi::BufferRole::StorageIndirect, sizeof(zero)},
                                      RasterBytes(zero),
                                      arguments) == rhi::RasterStatus::Ready);
            const auto kernel = ludus::shaders::cull::Compute();
            rhi::RasterShaderHandle shader;
            rhi::BindingLayoutHandle computeLayout;
            REQUIRE(rhi::CreateRasterShader(device, kernel, shader) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::CreateBindingLayout(device, kernel.Bindings, computeLayout) == rhi::RasterStatus::Ready);
            const rhi::RasterBindingResource bindings[]{{0, candidates, 0, 32},
                                                        {1, instances, 0, 16},
                                                        {2, arguments, 0, 20}};
            REQUIRE(rhi::CreateBindingSet(device, computeLayout, bindings, dispatch.Bindings) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(rhi::CreateComputePipeline(device, {shader, computeLayout}, dispatch.Pipeline) ==
                    rhi::RasterStatus::Ready);
            inputs.Compute = &dispatch;
            inputs.Candidates = candidates;
            inputs.Arguments = arguments;
        }
        rhi::OrderedGraph graph;
        rhi::GraphReport report;
        if (mode == 7)
        {
            uint8 baseline[96 * 64 * 4]{}, merged[96 * 64 * 4]{};
            // Optional benchmark output is test-only (ADR 0003). CPU setup includes
            // authoring/compile; execute includes acquire/encode/submit. A readback
            // waits for actual completion and checks every image outside those timers.
            const bool benchmark = std::getenv("LUDUS_RHI_BENCHMARK") != nullptr;
            if (benchmark)
            {
                rhi::MeasurementAdapter adapter;
                REQUIRE(rhi::GetMeasurementAdapter(device, adapter) == rhi::RasterStatus::Ready);
                std::printf("R7ADAPTER,");
                for (usize byte = 0; byte < sizeof(adapter.Name) && adapter.Name[byte] != '\0'; ++byte)
                {
                    std::printf("%02x", static_cast<uint32>(static_cast<uint8>(adapter.Name[byte])));
                }
                std::printf(",%u,%u,%u,%u,%u\n",
                            adapter.VendorId,
                            adapter.DeviceId,
                            adapter.DriverVersion,
                            adapter.SoftwareKnown ? 1U : 0U,
                            adapter.Software ? 1U : 0U);
                rhi::DeviceInfo profile;
                REQUIRE(rhi::GetDeviceInfo(device, profile) == rhi::DeviceStatus::Ready);
                std::printf("R7PROFILE,%u,%u,%u,%u,%u\n",
                            profile.Enabled.PortableRaster ? 1U : 0U,
                            profile.Enabled.Compute ? 1U : 0U,
                            profile.Enabled.OrderedRasterGraph ? 1U : 0U,
                            profile.Profile.Storage.ResourcesPerKind,
                            profile.Profile.Storage.DrawsPerFrame);
                std::printf("R5BACKEND,%u\n", static_cast<uint32>(rhi::GetStartup().SelectedBackend));
            }
            const uint32 counts[]{1, 8, 16, 31};
            for (const auto layers : counts)
            {
                const uint32 iterations = benchmark ? 144 : 1;
                for (uint32 iteration = 0; iteration < iterations; ++iteration)
                {
                    for (uint32 turn = 0; turn < 2; ++turn)
                    {
                        const bool merge = (turn + iteration) % 2 != 0;
                        const auto setupStart = std::chrono::steady_clock::now();
                        REQUIRE(ludus::qa::BuildMergedFrame(device, inputs, graph, report, merge, layers) ==
                                rhi::RasterStatus::Ready);
                        const auto armStart = std::chrono::steady_clock::now();
                        const auto armed =
                            description.EnableGpuTiming ? rhi::ArmGpuTiming(device) : rhi::RasterStatus::Unsupported;
                        REQUIRE((armed == rhi::RasterStatus::Ready || armed == rhi::RasterStatus::Unsupported));
                        const auto executeStart = std::chrono::steady_clock::now();
                        completion = {};
                        REQUIRE(rhi::ExecuteOrderedGraph(device, surface, graph, completion) ==
                                rhi::RasterStatus::Ready);
                        const auto executeEnd = std::chrono::steady_clock::now();
                        if (forceTiming)
                        {
                            rhi::GpuTiming submittedTiming;
                            REQUIRE(rhi::GetGpuTiming(device, submittedTiming) == rhi::RasterStatus::Ready);
                            if (armed == rhi::RasterStatus::Ready)
                            {
                                REQUIRE((submittedTiming.State == rhi::GpuTimingState::Pending ||
                                         submittedTiming.State == rhi::GpuTimingState::Available));
                            }
                        }
                        REQUIRE(rhi::backend::ReadHeadlessPixels(merge ? merged : baseline));
                        const auto completionEnd = std::chrono::steady_clock::now();
                        rhi::GpuTiming timing;
                        rhi::AllocationTelemetry memory;
                        REQUIRE(rhi::GetGpuTiming(device, timing) == rhi::RasterStatus::Ready);
                        REQUIRE(rhi::GetAllocationTelemetry(device, memory) == rhi::RasterStatus::Ready);
                        REQUIRE(timing.State != rhi::GpuTimingState::Armed);
                        REQUIRE(timing.State != rhi::GpuTimingState::Pending);
                        if (forceTiming && armed == rhi::RasterStatus::Ready)
                        {
                            REQUIRE(timing.State == rhi::GpuTimingState::Available);
                        }
                        const auto pollEnd = std::chrono::steady_clock::now();
                        CHECK(report.RasterPasses == (merge ? 2 : layers + 1));
                        CHECK(report.MergedPasses == (merge ? layers - 1 : 0));
                        CHECK(report.DrawCount == layers + 2);
                        if (benchmark && iteration >= 16)
                        {
                            const auto ns = [](auto from, auto to) {
                                return static_cast<float64>(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
                            };
                            std::printf("R7,%u,%u,%u,%u,%.0f,%.9g,%u,%.0f,%u,%.0f,%.0f,%.0f,%.0f,%u,%.0f,%.0f\n",
                                        layers,
                                        iteration - 16,
                                        merge ? 1U : 0U,
                                        static_cast<uint32>(timing.State),
                                        timing.DurationNanoseconds,
                                        timing.TickNanoseconds,
                                        timing.ValidBits,
                                        static_cast<float64>(memory.LogicalBytes),
                                        memory.ApiBytesAvailable ? 1U : 0U,
                                        static_cast<float64>(memory.ApiBytes),
                                        static_cast<float64>(memory.PeakObservedApiBytes),
                                        ns(armStart, executeStart),
                                        ns(completionEnd, pollEnd),
                                        memory.AllocatorBlockBytesAvailable ? 1U : 0U,
                                        static_cast<float64>(memory.AllocatorBlockBytes),
                                        static_cast<float64>(memory.PeakObservedAllocatorBlockBytes));
                            std::printf("R5,%u,%u,%u,%.0f,%.0f,%.0f,%u,%u,%.0f,%.0f,%.0f\n",
                                        layers,
                                        iteration - 16,
                                        merge ? 1U : 0U,
                                        ns(setupStart, executeStart),
                                        ns(executeStart, executeEnd),
                                        ns(executeEnd, completionEnd),
                                        report.RasterPasses,
                                        report.DrawCount,
                                        static_cast<float64>(report.PlanStorageBytes),
                                        static_cast<float64>(report.BufferBytes),
                                        static_cast<float64>(report.TextureBytes));
                        }
                    }
                    REQUIRE(std::memcmp(baseline, merged, sizeof(baseline)) == 0);
                    if (layers > 1)
                    {
                        // Different hues make painter-order reversal observable;
                        // equal images alone would allow two identically empty results.
                        const usize top = (usize{16} * 96 + 12) * 4;
                        const usize bottom = (usize{48} * 96 + 12) * 4;
                        CHECK(baseline[top] == (layers % 2 == 0 ? 191 : 255));
                        CHECK(baseline[top + 1] == 0);
                        CHECK(baseline[top + 2] == 0);
                        CHECK(baseline[top + 3] == 255);
                        CHECK(baseline[bottom + 2] == (layers % 2 == 0 ? 159 : 255));
                    }
                }
            }
            return;
        }
        REQUIRE(ludus::qa::BuildOrderedFrame(device, inputs, graph, report, mode == 4) == rhi::RasterStatus::Ready);
        // Plan retention, not public ownership, protects attachments during submit.
        if (computeMode == false)
        {
            REQUIRE(rhi::Destroy(device, offscreen) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::Destroy(device, unused) == rhi::RasterStatus::Ready);
        }
        REQUIRE(rhi::Destroy(device, offscreenView) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::ExecuteOrderedGraph(device, surface, graph, completion) == rhi::RasterStatus::Ready);
        if (computeMode)
        {
            rhi::ReadbackTicket ticket;
            REQUIRE(rhi::RequestBufferReadback(device, arguments, 0, 20, ticket) == rhi::RasterStatus::Pending);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (rhi::GetStatus(device, ticket) == rhi::RasterStatus::Pending &&
                   std::chrono::steady_clock::now() < until)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            uint32 actual[5]{};
            REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(actual), sizeof(actual)) ==
                    rhi::RasterStatus::Ready);
            const uint32 expected[5]{6, 2, 0, 0, 0};
            CHECK(std::memcmp(actual, expected, sizeof(actual)) == 0);
            REQUIRE(rhi::Release(device, ticket) == rhi::RasterStatus::Ready);
            REQUIRE(rhi::backend::ReadHeadlessPixels(computePixels));
            rhi::BufferHandle cpuInstances;
            REQUIRE(rhi::CreateBuffer(device,
                                      {rhi::BufferRole::Vertex, sizeof(offsets)},
                                      RasterBytes(offsets),
                                      cpuInstances) == rhi::RasterStatus::Ready);
            inputs.Compute = nullptr;
            inputs.Instances = cpuInstances;
            REQUIRE(ludus::qa::BuildOrderedFrame(device, inputs, graph, report, false) == rhi::RasterStatus::Ready);
            completion = {};
            REQUIRE(rhi::ExecuteOrderedGraph(device, surface, graph, completion) == rhi::RasterStatus::Ready);
        }
    }
    else
    {
        rhi::CommandBatch batch;
        REQUIRE(rhi::BeginCommands(device, batch) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::RecordDraw(device, batch, draw) == rhi::RasterStatus::Ready);
        const rhi::RasterVertexSlice backgroundSlices[2]{{background, 0, sizeof(back)},
                                                         {centerInstance, 0, sizeof(center)}};
        REQUIRE(rhi::RecordDraw(device, batch, {pipeline, backgroundSet, backgroundSlices, index, 0, 6, 4, 1}) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(device, request) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, set) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, backgroundSet) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, vertices) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, instances) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, index) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::FinishCommands(device, batch) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::SubmitCommands(device, surface, batch, completion) == rhi::RasterStatus::Ready);
    }
    uint8 pixels[96 * 64 * 4]{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
    CHECK(rhi::GetStatus(device, completion) == rhi::RasterStatus::Ready);
    if (computeMode)
    {
        CHECK(std::memcmp(pixels, computePixels, sizeof(pixels)) == 0);
    }
    const uint8 high = mode == 1 ? 55 : mode == 2 ? 160 : 255;
    const uint8 low = mode == 2 ? 32 : 0;
    const uint32 xs[4]{12, 36, 60, 84};
    for (usize i = 0; i < 4; ++i)
    {
        const usize top = (usize{16} * 96 + xs[i]) * 4;
        const usize bottom = (usize{48} * 96 + xs[i]) * 4;
        const bool left = i % 2 == 0;
        CHECK(pixels[top] == (left ? high : low));
        CHECK(pixels[top + 1] == (left ? low : high));
        CHECK(pixels[top + 2] == low);
        CHECK(pixels[top + 3] == 255);
        CHECK(pixels[bottom] == (left ? low : high));
        CHECK(pixels[bottom + 1] == (left ? low : high));
        CHECK(pixels[bottom + 2] == high);
        CHECK(pixels[bottom + 3] == 255);
    }
}

TEST_CASE("Native upload and readback rings preserve bytes across source destruction", "[rhi][gpu][lifetime]")
{
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::Shutdown();
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    const auto started = rhi::CreateDevice({}, { .Width = 16, .Height = 16 }, description, device, surface, &failure);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    const rhi::BufferRole roles[]{rhi::BufferRole::Uniform,
                                  rhi::BufferRole::Vertex,
                                  rhi::BufferRole::Index16,
                                  rhi::BufferRole::Index32};
    for (const auto role : roles)
    {
        uint8 bytes[16]{1, 2, 3, 4, 5, 6, 7, 8};
        rhi::UploadTicket upload;
        REQUIRE(rhi::RequestBufferUpload(device, {role, sizeof(bytes)}, bytes, sizeof(bytes), upload) ==
                rhi::RasterStatus::Pending);
        bytes[0] = 99;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (rhi::GetStatus(device, upload) == rhi::RasterStatus::Pending && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rhi::BufferHandle buffer;
        REQUIRE(rhi::TakeUploadedBuffer(device, upload, buffer) == rhi::RasterStatus::Ready);
        rhi::ReadbackTicket readback;
        REQUIRE(rhi::RequestBufferReadback(device, buffer, 0, sizeof(bytes), readback) == rhi::RasterStatus::Pending);
        REQUIRE(rhi::Destroy(device, buffer) == rhi::RasterStatus::Ready);
        while (rhi::GetStatus(device, readback) == rhi::RasterStatus::Pending &&
               std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        uint8 actual[16]{};
        REQUIRE(rhi::CopyReadback(device, readback, actual, sizeof(actual)) == rhi::RasterStatus::Ready);
        bytes[0] = 1;
        CHECK(std::memcmp(actual, bytes, sizeof(bytes)) == 0);
        REQUIRE(rhi::Release(device, readback) == rhi::RasterStatus::Ready);
    }
}
