#include "conformance_mask.h"
#include "conformance_sample.h"
#include "conformance_write.h"
#include "internal/readback.h"
#include "ordered_advance.h"
#include "ordered_draw.h"
#include "ordered_prepare.h"
#include "raster.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#if defined(_WIN32)
#    include "internal/d3d12_device.h"
#    include <d3d12sdklayers.h>
#    include <ludus/graphics/rhi/gpu_scope.h>
#    include <string_view>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// The same shader, packet authoring and complete-image oracle run on D3D12,
// Vulkan and Metal. No backend-specific expected image or rendering substitute.
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace threading = ludus::foundation::threading;
namespace timing = ludus::foundation::time;
namespace
{
constexpr uint32 WIDTH = 64;
constexpr uint32 HEIGHT = 48;
constexpr usize CHANNELS = 4;
constexpr usize PIXEL_BYTES = usize{WIDTH} * HEIGHT * CHANNELS;
constexpr usize VERTEX_VALUES = 20;
constexpr usize INDEX_COUNT = 6;
constexpr usize STREAM_COUNT = 2;
constexpr usize ATTRIBUTE_COUNT = 3;
constexpr usize BINDING_COUNT = 3;
constexpr usize FLOAT2_VALUES = 2;
constexpr usize SINGLE_STREAM_COUNT = 1;
struct DeviceOwner final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
#if defined(_WIN32)
    rhi::backend::d3d12::ComOwner<ID3D12InfoQueue> Messages;
#endif
    explicit DeviceOwner(bool shouldRequireCompute = false)
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Required.Compute = shouldRequireCompute;
#if defined(_WIN32)
        description.EnableGpuTiming = shouldRequireCompute;
#endif
        rhi::StartupInfo failure;
        const rhi::DeviceStatus status =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
#if defined(LUDUS_TEST_METAL)
        if (status != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            SKIP("No Metal adapter on this host");
        }
#endif
        REQUIRE(status == rhi::DeviceStatus::Ready);
#if defined(_WIN32)
        ID3D12InfoQueue* messagesOrNull = nullptr;
        const HRESULT result = rhi::backend::d3d12::GetDeviceOrNull()->QueryInterface(
            IID_ID3D12InfoQueue,
            rhi::backend::d3d12::GetInterfaceOutput(messagesOrNull));
        if (SUCCEEDED(result) == true)
        {
            Messages = rhi::backend::d3d12::ComOwner<ID3D12InfoQueue>(messagesOrNull);
            Messages->ClearStoredMessages();
        }
        else
        {
            WARN("D3D12 debug layer unavailable; native image conformance still executes");
        }
#endif
    }
    void CheckDebugMessages() const
    {
#if defined(_WIN32)
        REQUIRE(rhi::backend::d3d12::WaitForQueue() == true);
        if (Messages.Get() != nullptr)
        {
            const uint64 count = Messages->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (uint64 index = 0; index < count; ++index)
            {
                SIZE_T size = 0;
                REQUIRE(SUCCEEDED(Messages->GetMessage(index, nullptr, &size)) == true);
                Array<uint8> storage;
                REQUIRE(storage.TryResize(size) == true);
                D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(storage.GetData());
                REQUIRE(SUCCEEDED(Messages->GetMessage(index, message, &size)) == true);
                INFO(message->pDescription);
                CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION);
                CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR);
            }
        }
#endif
    }
    ~DeviceOwner()
    {
        static_cast<void>(rhi::DestroyDevice(Device));
    }
};
template <typename T, usize COUNT>
std::span<const uint8> GetBytes(const StaticArray<T, COUNT>& values)
{
    return {reinterpret_cast<const uint8*>(values.GetData()), sizeof(T) * COUNT};
}
template <typename Handle>
void RequireReady(rhi::DeviceHandle device, Handle handle, rhi::RasterStatus status = rhi::RasterStatus::Pending)
{
    timing::Deadline deadline;
    REQUIRE(deadline.Arm(timing::Now(), timing::Seconds(5)) == timing::TimeStatus::Ok);
    while (status == rhi::RasterStatus::Pending && deadline.IsExpired(timing::Now()) == false)
    {
        threading::YieldThread();
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
}
} // namespace

TEST_CASE("Portable raster complete images cover depth blending indices and clipped rectangles",
          "[rhi][gpu][portable-raster]")
{
    const uint32 mode = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U);
    CAPTURE(mode);
    const bool isSrgb = mode == 1;
    const bool hasBlend = mode == 2;
    const bool hasRectangles = mode == 3;
    const bool hasReverseDepth = mode == 4;
    const bool hasIndex32 = mode == 5;
    const bool hasOffscreen = mode >= 6;
    DeviceOwner owner;
    const rhi::RasterShaderDescription vertex = ludus::shaders::raster::Vertex();
    const rhi::RasterShaderDescription fragment = ludus::shaders::raster::Fragment();
    rhi::RasterShaderHandle vertexShader;
    rhi::RasterShaderHandle fragmentShader;
    rhi::BindingLayoutHandle layout;
    REQUIRE(rhi::CreateRasterShader(owner.Device, vertex, vertexShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(owner.Device, fragment, fragmentShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingLayout(owner.Device, fragment.Bindings, layout) == rhi::RasterStatus::Ready);
    const StaticArray<float32, CHANNELS> tint{1, 1, 1, 1};
    const StaticArray<float32, CHANNELS> dark{0, 0, 0, 1};
    rhi::BufferHandle uniform;
    rhi::BufferHandle darkUniform;
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Uniform, sizeof(tint)}, GetBytes(tint), uniform) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Uniform, sizeof(dark)}, GetBytes(dark), darkUniform) ==
            rhi::RasterStatus::Ready);
    constexpr usize IMAGE_BYTES = 16;
    const uint8 high = isSrgb == true || hasBlend == true ? 128 : 255;
    const uint8 alpha = hasBlend == true ? 128 : 255;
    const StaticArray<uint8, IMAGE_BYTES>
        image{high, 0, 0, alpha, 0, high, 0, alpha, 0, 0, high, alpha, high, high, high, alpha};
    rhi::TextureHandle texture;
    const rhi::RasterStatus textureStatus =
        rhi::CreateTexture(owner.Device,
                           {2, 2, isSrgb == true ? rhi::RasterFormat::Rgba8Srgb : rhi::RasterFormat::Rgba8Unorm},
                           {image.AsSpan(), 8},
                           texture);
    RequireReady(owner.Device, texture, textureStatus);
    rhi::TextureViewHandle view;
    rhi::SamplerHandle sampler;
    REQUIRE(rhi::CreateTextureView(owner.Device, texture, view) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateSampler(owner.Device, {}, sampler) == rhi::RasterStatus::Ready);
    StaticArray<rhi::RasterBindingResource, BINDING_COUNT> bindings{};
    bindings[0] = { .Binding = 0, .Buffer = uniform, .Size = sizeof(tint) };
    bindings[1] = { .Binding = 1, .Texture = view };
    bindings[2] = { .Binding = 2, .Sampler = sampler };
    rhi::BindingSetHandle set;
    rhi::BindingSetHandle darkSet;
    REQUIRE(rhi::CreateBindingSet(owner.Device, layout, bindings.AsSpan(), set) == rhi::RasterStatus::Ready);
    bindings[0].Buffer = darkUniform;
    REQUIRE(rhi::CreateBindingSet(owner.Device, layout, bindings.AsSpan(), darkSet) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::RasterVertexStream, STREAM_COUNT> streams{{{20, false}, {8, true}}};
    const StaticArray<rhi::RasterVertexAttribute, ATTRIBUTE_COUNT> attributes{
        {{0, 0, 0, rhi::RasterVertexFormat::Float3},
         {1, 0, 12, rhi::RasterVertexFormat::Float2},
         {2, 1, 0, rhi::RasterVertexFormat::Float2}}};
    rhi::RasterPipelineDescription pipelineDescription{vertexShader,
                                                       fragmentShader,
                                                       layout,
                                                       streams.AsSpan(),
                                                       attributes.AsSpan(),
                                                       true,
                                                       hasBlend};
    pipelineDescription.DepthCompare =
        hasReverseDepth == true ? rhi::RasterDepthCompare::Greater : rhi::RasterDepthCompare::Less;
    rhi::RasterPipelineHandle pipeline;
    REQUIRE(rhi::CreateRasterPipeline(owner.Device, pipelineDescription, pipeline) == rhi::RasterStatus::Ready);
    rhi::RasterPipelineHandle offscreenPipeline;
    rhi::TextureHandle attachment;
    rhi::TextureViewHandle attachmentView;
    rhi::BindingSetHandle compositeSet;
    if (hasOffscreen == true)
    {
        const rhi::RasterFormat format = mode == 7   ? rhi::RasterFormat::Rgba8Srgb
                                         : mode == 8 ? rhi::RasterFormat::Rgba16Float
                                                     : rhi::RasterFormat::Rgba8Unorm;
        rhi::TextureDescription attachmentDescription{WIDTH, HEIGHT, format, true};
        REQUIRE(rhi::CreateTexture(owner.Device, attachmentDescription, {}, attachment) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateTextureView(owner.Device, attachment, attachmentView) == rhi::RasterStatus::Ready);
        bindings[0].Buffer = uniform;
        bindings[1].Texture = attachmentView;
        REQUIRE(rhi::CreateBindingSet(owner.Device, layout, bindings.AsSpan(), compositeSet) ==
                rhi::RasterStatus::Ready);
        pipelineDescription.Target = mode == 7   ? rhi::RasterTarget::Rgba8Srgb
                                     : mode == 8 ? rhi::RasterTarget::Rgba16Float
                                                 : rhi::RasterTarget::Rgba8Unorm;
        REQUIRE(rhi::CreateRasterPipeline(owner.Device, pipelineDescription, offscreenPipeline) ==
                rhi::RasterStatus::Ready);
    }
    const float32 nearDepth = hasReverseDepth == true ? .75F : .25F;
    const float32 farDepth = hasReverseDepth == true ? .25F : .75F;
    const float32 halfWidth = hasIndex32 == true ? .5F : 1;
    const StaticArray<float32, VERTEX_VALUES> points{-halfWidth, -1, nearDepth, 0, 1, -halfWidth, 1,  nearDepth, 0, 0,
                                                     halfWidth,  1,  nearDepth, 1, 0, halfWidth,  -1, nearDepth, 1, 1};
    const StaticArray<float32, VERTEX_VALUES> back{-1, -1, farDepth, 0, 1, -1, 1,  farDepth, 0, 0,
                                                   1,  1,  farDepth, 1, 0, 1,  -1, farDepth, 1, 1};
    // Instance stream advancement is exercised independently from the vertex stream.
    const StaticArray<float32, FLOAT2_VALUES> center{0, 0};
    const StaticArray<float32, CHANNELS> offsets{hasIndex32 == true ? -.5F : 0, 0, hasIndex32 == true ? .5F : 0, 0};
    const StaticArray<uint16, INDEX_COUNT> index16{0, 1, 2, 0, 2, 3};
    const StaticArray<uint32, INDEX_COUNT> index32{0, 1, 2, 0, 2, 3};
    rhi::BufferHandle vertices;
    rhi::BufferHandle background;
    rhi::BufferHandle instances;
    rhi::BufferHandle centerInstance;
    rhi::BufferHandle indices;
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, sizeof(points)}, GetBytes(points), vertices) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, sizeof(back)}, GetBytes(back), background) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, sizeof(offsets)}, GetBytes(offsets), instances) ==
            rhi::RasterStatus::Ready);
    REQUIRE(
        rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, sizeof(center)}, GetBytes(center), centerInstance) ==
        rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(owner.Device,
                              {hasIndex32 == true ? rhi::BufferRole::Index32 : rhi::BufferRole::Index16,
                               hasIndex32 == true ? sizeof(index32) : sizeof(index16)},
                              hasIndex32 == true ? GetBytes(index32) : GetBytes(index16),
                              indices) == rhi::RasterStatus::Ready);
    // Detach public ownership while immutable snapshots/pipelines retain native dependencies.
    REQUIRE(rhi::Destroy(owner.Device, texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, view) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, uniform) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, darkUniform) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, sampler) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, vertexShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, fragmentShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, layout) == rhi::RasterStatus::Ready);
    const float64 clear = hasBlend == true ? .25 : 0;
    REQUIRE(rhi::SetFrameTarget(owner.Device, owner.Surface, {WIDTH, HEIGHT, clear, clear, clear, 1}) ==
            rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.ColorLoad = rhi::RasterLoad::Load;
    pass.ClearDepth = hasReverseDepth == true ? 0.0f : 1.0f;
    if (hasRectangles == true)
    {
        pass.UseViewport = true;
        pass.Viewport = {8, 6, 48, 32};
        pass.UseScissor = true;
        pass.Scissor = {20, 10, 28, 28};
    }
    const uint32 frames = hasIndex32 == true ? 42 : 1;
    for (uint32 frame = 0; frame < frames; ++frame)
    {
        if (hasIndex32 == true && frame >= 40)
        {
            const uint32 width = frame == 40 ? WIDTH + 16 : WIDTH;
            const uint32 height = frame == 40 ? HEIGHT + 8 : HEIGHT;
            REQUIRE(rhi::SetFrameTarget(owner.Device, owner.Surface, {width, height, clear, clear, clear, 1}) ==
                    rhi::DeviceStatus::Ready);
        }
        REQUIRE(rhi::BeginFrame(owner.Device, owner.Surface) == rhi::DeviceStatus::Ready);
        if (hasOffscreen == true)
        {
            pass.Color = attachment;
            pass.ColorLoad = rhi::RasterLoad::Clear;
        }
        REQUIRE(rhi::BeginRasterPass(owner.Device, pass) == rhi::RasterStatus::Ready);
        StaticArray<rhi::RasterVertexSlice, STREAM_COUNT> slices{
            {{vertices, 0, sizeof(points)}, {instances, 0, sizeof(offsets)}}};
        rhi::RasterDraw draw{hasOffscreen == true ? offscreenPipeline : pipeline,
                             set,
                             slices.AsSpan(),
                             indices,
                             0,
                             INDEX_COUNT,
                             4,
                             1};
        draw.InstanceCount = hasIndex32 == true ? 2U : 1U;
        REQUIRE(rhi::DrawIndexed(owner.Device, draw) == rhi::RasterStatus::Ready);
        slices[0] = {background, 0, sizeof(back)};
        slices[1] = {centerInstance, 0, sizeof(center)};
        draw.InstanceCount = 1;
        draw.Bindings = darkSet;
        REQUIRE(rhi::DrawIndexed(owner.Device, draw) == rhi::RasterStatus::Ready);
        if (hasOffscreen == true)
        {
            rhi::RasterPassDescription surfacePass;
            REQUIRE(rhi::BeginRasterPass(owner.Device, surfacePass) == rhi::RasterStatus::Ready);
            slices[0] = {vertices, 0, sizeof(points)};
            draw.Pipeline = pipeline;
            draw.Bindings = compositeSet;
            REQUIRE(rhi::DrawIndexed(owner.Device, draw) == rhi::RasterStatus::Ready);
        }
        REQUIRE(rhi::EndFrame(owner.Device) == rhi::DeviceStatus::Ready);
    }
    REQUIRE(rhi::Destroy(owner.Device, pipeline) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, set) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, darkSet) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, vertices) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, background) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, instances) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, centerInstance) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, indices) == rhi::RasterStatus::Ready);
    if (hasOffscreen == true)
    {
        REQUIRE(rhi::Destroy(owner.Device, compositeSet) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(owner.Device, offscreenPipeline) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(owner.Device, attachmentView) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(owner.Device, attachment) == rhi::RasterStatus::Ready);
    }
    StaticArray<uint8, PIXEL_BYTES> pixels{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels.AsSpan()) == true);
    const int32 expectedHigh = isSrgb == true ? 55 : hasBlend == true ? 160 : 255;
    const int32 expectedLow = hasBlend == true ? 32 : 0;
    for (uint32 y = 0; y < HEIGHT; ++y)
    {
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            const bool isVisible = hasRectangles == false || (x >= 20 && x < 48 && y >= 10 && y < 38);
            const bool isRight = hasIndex32 == true ? x % (WIDTH / 2) >= WIDTH / 4 : x >= WIDTH / 2;
            const bool isBottom = y >= (hasRectangles == true ? 22U : HEIGHT / 2);
            const StaticArray<int32, CHANNELS> expected{
                isVisible == true && (isRight == isBottom) ? expectedHigh : expectedLow,
                isVisible == true && isRight == true ? expectedHigh : expectedLow,
                isVisible == true && isBottom == true ? expectedHigh : expectedLow,
                255};
            for (usize channel = 0; channel < CHANNELS; ++channel)
            {
                const int32 difference = pixels[(usize{y} * WIDTH + x) * CHANNELS + channel] - expected[channel];
                if (difference < -1 || difference > 1)
                {
                    CAPTURE(x, y, channel, expected[channel]);
                    FAIL("Portable raster image differs by more than one UNORM byte");
                }
            }
        }
    }
    owner.CheckDebugMessages();
}

TEST_CASE("Portable line fill draws triangle edges and leaves interiors clear", "[rhi][gpu][portable-raster]")
{
    DeviceOwner owner;
    rhi::RasterCapabilities capabilities;
    REQUIRE(rhi::GetRasterCapabilities(owner.Device, capabilities) == rhi::RasterStatus::Ready);
    if (capabilities.HasLineFill == false)
    {
        SKIP("Device did not enable line fill");
    }
    const rhi::RasterShaderDescription vertex = ludus::shaders::raster::Vertex();
    const rhi::RasterShaderDescription fragment = ludus::shaders::raster::Fragment();
    rhi::RasterShaderHandle vertexShader;
    rhi::RasterShaderHandle fragmentShader;
    rhi::BindingLayoutHandle layout;
    REQUIRE(rhi::CreateRasterShader(owner.Device, vertex, vertexShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(owner.Device, fragment, fragmentShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingLayout(owner.Device, fragment.Bindings, layout) == rhi::RasterStatus::Ready);
    const StaticArray<float32, CHANNELS> tint{1, 1, 1, 1};
    rhi::BufferHandle uniform;
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Uniform, sizeof(tint)}, GetBytes(tint), uniform) ==
            rhi::RasterStatus::Ready);
    constexpr usize TEXEL_BYTES = 4;
    const StaticArray<uint8, TEXEL_BYTES> white{255, 255, 255, 255};
    rhi::TextureHandle texture;
    RequireReady(owner.Device,
                 texture,
                 rhi::CreateTexture(owner.Device, {1, 1, rhi::RasterFormat::Rgba8Unorm}, {white.AsSpan(), 4}, texture));
    rhi::TextureViewHandle view;
    rhi::SamplerHandle sampler;
    REQUIRE(rhi::CreateTextureView(owner.Device, texture, view) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateSampler(owner.Device, {}, sampler) == rhi::RasterStatus::Ready);
    StaticArray<rhi::RasterBindingResource, BINDING_COUNT> bindings{};
    bindings[0] = { .Binding = 0, .Buffer = uniform, .Size = sizeof(tint) };
    bindings[1] = { .Binding = 1, .Texture = view };
    bindings[2] = { .Binding = 2, .Sampler = sampler };
    rhi::BindingSetHandle set;
    REQUIRE(rhi::CreateBindingSet(owner.Device, layout, bindings.AsSpan(), set) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::RasterVertexStream, STREAM_COUNT> streams{{{20, false}, {8, true}}};
    const StaticArray<rhi::RasterVertexAttribute, ATTRIBUTE_COUNT> attributes{
        {{0, 0, 0, rhi::RasterVertexFormat::Float3},
         {1, 0, 12, rhi::RasterVertexFormat::Float2},
         {2, 1, 0, rhi::RasterVertexFormat::Float2}}};
    rhi::RasterPipelineDescription description{vertexShader,
                                               fragmentShader,
                                               layout,
                                               streams.AsSpan(),
                                               attributes.AsSpan(),
                                               true};
    description.FillMode = rhi::RasterFillMode::Lines;
    rhi::RasterPipelineHandle pipeline;
    REQUIRE(rhi::CreateRasterPipeline(owner.Device, description, pipeline) == rhi::RasterStatus::Ready);
    // A quad spanning the middle half of the target in each axis, split along its rising diagonal.
    constexpr float32 HALF = .5F;
    constexpr float32 DEPTH = .5F;
    const StaticArray<float32, VERTEX_VALUES> points{-HALF, -HALF, DEPTH, 0, 1, -HALF, HALF,  DEPTH, 0, 0,
                                                     HALF,  HALF,  DEPTH, 1, 0, HALF,  -HALF, DEPTH, 1, 1};
    const StaticArray<float32, FLOAT2_VALUES> center{0, 0};
    const StaticArray<uint16, INDEX_COUNT> indices16{0, 1, 2, 0, 2, 3};
    rhi::BufferHandle vertices;
    rhi::BufferHandle instance;
    rhi::BufferHandle indices;
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, sizeof(points)}, GetBytes(points), vertices) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, sizeof(center)}, GetBytes(center), instance) ==
            rhi::RasterStatus::Ready);
    REQUIRE(
        rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Index16, sizeof(indices16)}, GetBytes(indices16), indices) ==
        rhi::RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(owner.Device, owner.Surface, {WIDTH, HEIGHT, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(owner.Device, owner.Surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.ColorLoad = rhi::RasterLoad::Load;
    REQUIRE(rhi::BeginRasterPass(owner.Device, pass) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::RasterVertexSlice, STREAM_COUNT> slices{
        {{vertices, 0, sizeof(points)}, {instance, 0, sizeof(center)}}};
    const rhi::RasterDraw draw{pipeline, set, slices.AsSpan(), indices, 0, INDEX_COUNT, 4, 1};
    REQUIRE(rhi::DrawIndexed(owner.Device, draw) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::EndFrame(owner.Device) == rhi::DeviceStatus::Ready);
    StaticArray<uint8, PIXEL_BYTES> pixels{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels.AsSpan()) == true);
    const auto isLit = [&pixels](uint32 x, uint32 y) { return pixels[(usize{y} * WIDTH + x) * CHANNELS] > 127; };
    // Rasterization rules differ by a pixel between APIs, so each edge is searched within that tolerance.
    constexpr uint32 TOLERANCE = 1;
    const auto hasLitColumn = [&isLit](uint32 column, uint32 y) {
        return isLit(column - TOLERANCE, y) == true || isLit(column, y) == true || isLit(column + TOLERANCE, y) == true;
    };
    const auto hasLitRow = [&isLit](uint32 x, uint32 row) {
        return isLit(x, row - TOLERANCE) == true || isLit(x, row) == true || isLit(x, row + TOLERANCE) == true;
    };
    const uint32 left = WIDTH / 4;
    const uint32 right = WIDTH - WIDTH / 4;
    const uint32 top = HEIGHT / 4;
    const uint32 bottom = HEIGHT - HEIGHT / 4;
    CHECK(hasLitColumn(left, HEIGHT / 2) == true);
    CHECK(hasLitColumn(right, HEIGHT / 2) == true);
    CHECK(hasLitRow(WIDTH / 2 - WIDTH / 8, top) == true);
    CHECK(hasLitRow(WIDTH / 2 - WIDTH / 8, bottom) == true);
    // The shared diagonal passes through the centre; points well off it and the border stay clear.
    CHECK(hasLitColumn(WIDTH / 2, HEIGHT / 2) == true);
    CHECK(isLit(left + WIDTH / 16, bottom - HEIGHT / 16) == false);
    CHECK(isLit(right - WIDTH / 16, top + HEIGHT / 16) == false);
    usize litCount = 0;
    for (uint32 y = 0; y < HEIGHT; ++y)
    {
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            litCount += isLit(x, y) == true ? 1U : 0U;
        }
    }
    // Five one-pixel edges cover far less than the filled quad's quarter of the target.
    const usize filledCount = usize{WIDTH / 2} * (HEIGHT / 2);
    CHECK(litCount > 0);
    CHECK(litCount * 2 < filledCount);
    REQUIRE(rhi::Destroy(owner.Device, pipeline) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, set) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, vertices) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, instance) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, indices) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, sampler) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, view) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, texture) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, uniform) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, vertexShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, fragmentShader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(owner.Device, layout) == rhi::RasterStatus::Ready);
    owner.CheckDebugMessages();
}
TEST_CASE("Portable attachment passes preserve MRT stencil and selected mip contents", "[rhi][gpu][portable-raster]")
{
    const bool hasStencil = GENERATE(false, true);
    DeviceOwner owner;
    constexpr usize TARGETS = 2;
    constexpr usize MESH_VALUES = 16;
    constexpr usize MESH_ATTRIBUTES = 2;
    const rhi::RasterShaderDescription writer = ludus::shaders::conformance_write::Fragment();
    const rhi::RasterShaderDescription masking = ludus::shaders::conformance_mask::Fragment();
    const rhi::RasterShaderDescription sampling = ludus::shaders::conformance_sample::Fragment();
    rhi::RasterShaderHandle vertex;
    rhi::RasterShaderHandle fragment;
    rhi::RasterShaderHandle maskFragment;
    rhi::RasterShaderHandle sampleFragment;
    rhi::BindingLayoutHandle emptyLayout;
    rhi::BindingLayoutHandle sampleLayout;
    REQUIRE(rhi::CreateRasterShader(owner.Device, ludus::shaders::conformance_write::Vertex(), vertex) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(owner.Device, writer, fragment) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(owner.Device, masking, maskFragment) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(owner.Device, sampling, sampleFragment) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingLayout(owner.Device, {}, emptyLayout) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingLayout(owner.Device, sampling.Bindings, sampleLayout) == rhi::RasterStatus::Ready);
    rhi::BindingSetHandle emptySet;
    REQUIRE(rhi::CreateBindingSet(owner.Device, emptyLayout, {}, emptySet) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::RasterVertexStream, SINGLE_STREAM_COUNT> streams{{{16, false}}};
    const StaticArray<rhi::RasterVertexAttribute, MESH_ATTRIBUTES> attributes{
        {{0, 0, 0, rhi::RasterVertexFormat::Float2}, {1, 0, 8, rhi::RasterVertexFormat::Float2}}};
    const StaticArray<rhi::RasterFormat, TARGETS> formats{rhi::RasterFormat::Rgba8Unorm, rhi::RasterFormat::Rgba8Unorm};
    rhi::RasterPipelineDescription pipelineDescription{vertex,
                                                       fragment,
                                                       emptyLayout,
                                                       streams.AsSpan(),
                                                       attributes.AsSpan()};
    pipelineDescription.ColorFormats = formats.AsSpan();
    pipelineDescription.Stencil = hasStencil;
    pipelineDescription.StencilReference = 7;
    pipelineDescription.StencilFront.Pass = rhi::RasterStencilOperation::Replace;
    pipelineDescription.StencilBack.Pass = rhi::RasterStencilOperation::Zero;
    rhi::RasterPipelineHandle pipeline;
    rhi::RasterPipelineHandle maskPipeline;
    rhi::RasterPipelineHandle samplePipeline;
    REQUIRE(rhi::CreateRasterPipeline(owner.Device, pipelineDescription, pipeline) == rhi::RasterStatus::Ready);
    pipelineDescription.Fragment = maskFragment;
    pipelineDescription.StencilFront.Compare = rhi::RasterStencilCompare::Equal;
    pipelineDescription.StencilBack.Compare = rhi::RasterStencilCompare::Equal;
    pipelineDescription.StencilFront.Pass = rhi::RasterStencilOperation::Keep;
    pipelineDescription.StencilWriteMask = 0;
    REQUIRE(rhi::CreateRasterPipeline(owner.Device, pipelineDescription, maskPipeline) == rhi::RasterStatus::Ready);
    pipelineDescription = {vertex, sampleFragment, sampleLayout, streams.AsSpan(), attributes.AsSpan()};
    REQUIRE(rhi::CreateRasterPipeline(owner.Device, pipelineDescription, samplePipeline) == rhi::RasterStatus::Ready);
    const StaticArray<float32, MESH_VALUES> mesh{-1, -1, 0, 1, -1, 1, 0, 0, 1, 1, 1, 0, 1, -1, 1, 1};
    // Counter-clockwise in canonical clip coordinates; front/back stencil must agree across APIs.
    const StaticArray<uint16, INDEX_COUNT> indexData{0, 2, 1, 0, 3, 2};
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    REQUIRE(rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Vertex, sizeof(mesh)}, GetBytes(mesh), vertices) ==
            rhi::RasterStatus::Ready);
    REQUIRE(
        rhi::CreateBuffer(owner.Device, {rhi::BufferRole::Index16, sizeof(indexData)}, GetBytes(indexData), indices) ==
        rhi::RasterStatus::Ready);
    StaticArray<rhi::TextureHandle, TARGETS> textures{};
    StaticArray<rhi::TextureViewHandle, TARGETS> attachments{};
    StaticArray<rhi::TextureViewHandle, TARGETS> sampled{};
    StaticArray<rhi::BindingSetHandle, TARGETS> sets{};
    StaticArray<rhi::RasterColorAttachment, TARGETS> colors{};
    rhi::SamplerHandle sampler;
    REQUIRE(rhi::CreateSampler(owner.Device, {}, sampler) == rhi::RasterStatus::Ready);
    constexpr uint32 SELECTED_MIP = 1;
    constexpr uint32 ATTACHMENT_EXTENT = 16;
    for (usize target = 0; target < TARGETS; ++target)
    {
        rhi::TextureDescription description{ATTACHMENT_EXTENT, ATTACHMENT_EXTENT, rhi::RasterFormat::Rgba8Unorm, true};
        description.MipLevels = 2;
        description.Stencil = hasStencil == true && target == 0;
        REQUIRE(rhi::CreateTexture(owner.Device, description, {}, textures[target]) == rhi::RasterStatus::Ready);
        const rhi::TextureViewDescription view{ .BaseMip = SELECTED_MIP, .MipCount = 1, .Attachment = true };
        REQUIRE(rhi::CreateTextureView(owner.Device, textures[target], view, attachments[target]) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateTextureView(owner.Device,
                                       textures[target],
                                       { .BaseMip = SELECTED_MIP, .MipCount = 1 },
                                       sampled[target]) == rhi::RasterStatus::Ready);
        const StaticArray<rhi::RasterBindingResource, TARGETS> resources{
            {{ .Binding = 0, .Texture = sampled[target] }, { .Binding = 1, .Sampler = sampler }}};
        REQUIRE(rhi::CreateBindingSet(owner.Device, sampleLayout, resources.AsSpan(), sets[target]) ==
                rhi::RasterStatus::Ready);
        colors[target].View = attachments[target];
    }
    const StaticArray<rhi::RasterVertexSlice, SINGLE_STREAM_COUNT> slices{{{vertices, 0, sizeof(mesh)}}};
    rhi::RasterDraw draw{pipeline, emptySet, slices.AsSpan(), indices, 0, INDEX_COUNT, 4, 1};
    REQUIRE(rhi::BeginFrame(owner.Device, owner.Surface) == rhi::DeviceStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.Colors = colors.GetData();
    pass.ColorCount = TARGETS;
    pass.StencilStore = rhi::RasterStore::Store;
    REQUIRE(rhi::BeginRasterPass(owner.Device, pass) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::DrawIndexed(owner.Device, draw) == rhi::RasterStatus::Ready);
    if (hasStencil == true)
    {
        for (rhi::RasterColorAttachment& color : colors)
        {
            color.Load = rhi::RasterLoad::Load;
        }
        pass.StencilLoad = rhi::RasterLoad::Load;
        pass.StencilReadOnly = true;
        pass.UseScissor = true;
        pass.Scissor = {0, 0, 4, 8};
        draw.Pipeline = maskPipeline;
        REQUIRE(rhi::BeginRasterPass(owner.Device, pass) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::DrawIndexed(owner.Device, draw) == rhi::RasterStatus::Ready);
    }
    for (usize target = 0; target < TARGETS; ++target)
    {
        rhi::RasterPassDescription surface;
        surface.ColorLoad = rhi::RasterLoad::Load;
        surface.UseViewport = true;
        surface.Viewport = {static_cast<uint32>(target) * (WIDTH / 2), 0, WIDTH / 2, HEIGHT};
        surface.UseScissor = true;
        surface.Scissor = surface.Viewport;
        REQUIRE(rhi::BeginRasterPass(owner.Device, surface) == rhi::RasterStatus::Ready);
        draw.Pipeline = samplePipeline;
        draw.Bindings = sets[target];
        REQUIRE(rhi::DrawIndexed(owner.Device, draw) == rhi::RasterStatus::Ready);
    }
    REQUIRE(rhi::EndFrame(owner.Device) == rhi::DeviceStatus::Ready);
    StaticArray<uint8, PIXEL_BYTES> pixels{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels.AsSpan()) == true);
    for (uint32 y = 0; y < HEIGHT; ++y)
    {
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            const bool isSecond = x >= WIDTH / 2;
            const bool isRight = x % (WIDTH / 2) >= WIDTH / 4;
            const bool isBottom = y >= HEIGHT / 2;
            const bool isMasked = hasStencil == true && isRight == false;
            const StaticArray<uint8, CHANNELS> expected{
                static_cast<uint8>(isSecond == false && (isMasked == true || isRight == true) ? 255 : 0),
                static_cast<uint8>(isMasked == true || (isSecond == true && isBottom == true) ? 255 : 0),
                static_cast<uint8>(
                    isMasked == true ? (isSecond == true ? 255 : 0)
                                     : (isSecond == true ? (isRight == true ? 255 : 0) : (isBottom == true ? 255 : 0))),
                255};
            for (usize channel = 0; channel < CHANNELS; ++channel)
            {
                if (pixels[(usize{y} * WIDTH + x) * CHANNELS + channel] != expected[channel])
                {
                    CAPTURE(x, y, channel, hasStencil);
                    FAIL("MRT/stencil image differs from the shared oracle");
                }
            }
        }
    }
    owner.CheckDebugMessages();
}

TEST_CASE("Portable surface graph preparation precedes frame acquisition", "[rhi][gpu][portable-raster]")
{
    DeviceOwner owner;
    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(owner.Device, graph) == rhi::RasterStatus::Ready);
    rhi::GraphPassDescription pass;
    pass.Name = "surface clear before acquisition";
    pass.Attachment.Clear[0] = 1;
    pass.Attachment.Clear[2] = 1;
    REQUIRE(rhi::AddGraphPass(owner.Device, graph, pass) == rhi::RasterStatus::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(owner.Device, graph, report) == rhi::RasterStatus::Ready);
    CHECK(report.LivePasses == 1);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::ExecuteOrderedGraph(owner.Device, owner.Surface, graph, completion) == rhi::RasterStatus::Ready);
    StaticArray<uint8, PIXEL_BYTES> pixels{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels.AsSpan()) == true);
    REQUIRE(rhi::GetStatus(owner.Device, completion) == rhi::RasterStatus::Ready);
    const StaticArray<uint8, CHANNELS> expected{255, 0, 255, 255};
    for (usize index = 0; index < PIXEL_BYTES; ++index)
    {
        if (pixels[index] != expected[index % CHANNELS])
        {
            CAPTURE(index);
            FAIL("Surface graph did not publish its authored clear");
        }
    }
    owner.CheckDebugMessages();
}

TEST_CASE("Portable ordered compute dispatches feed indirect raster and transfer readback",
          "[rhi][gpu][ordered-compute]")
{
    DeviceOwner owner(true);
    const rhi::DeviceHandle device = owner.Device;
#if defined(_WIN32)
    REQUIRE(rhi::SetFrameTarget(device,
                                owner.Surface,
                                { .Width = WIDTH, .Height = HEIGHT, .ShouldPresentToSurface = false }) ==
            rhi::DeviceStatus::Ready);
#endif

    rhi::ComputeCapabilities capabilities;
    REQUIRE(rhi::GetComputeCapabilities(device, capabilities) == rhi::RasterStatus::Ready);
    const usize counterOffset = capabilities.StorageOffsetAlignment;
    Array<uint8> initialCounter;
    REQUIRE(initialCounter.TryResize(counterOffset + sizeof(uint32)) == true);
    rhi::BufferHandle counter;
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::Storage, initialCounter.GetSize()},
                              initialCounter.AsSpan(),
                              counter) == rhi::RasterStatus::Ready);
    constexpr uint32 COUNTER_SEED = 40;
    rhi::TransferTicket update;
    REQUIRE(rhi::RequestBufferUpdate(device,
                                     counter,
                                     counterOffset,
                                     reinterpret_cast<const uint8*>(&COUNTER_SEED),
                                     sizeof(COUNTER_SEED),
                                     update) == rhi::RasterStatus::Pending);
    RequireReady(device, update);
    REQUIRE(rhi::Release(device, update) == rhi::RasterStatus::Ready);

    constexpr uint32 TRIANGLE_INDEX_COUNT = 3;
    constexpr usize TRIANGLE_COORDINATES = TRIANGLE_INDEX_COUNT * FLOAT2_VALUES;
    const StaticArray<float32, TRIANGLE_COORDINATES> initialVertices{};
    const rhi::IndexedIndirectArguments initialArguments{};
    const StaticArray<uint32, TRIANGLE_INDEX_COUNT> initialIndices{0, 1, 2};
    rhi::BufferHandle vertices;
    rhi::BufferHandle arguments;
    rhi::BufferHandle indices;
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::StorageVertex, sizeof(initialVertices)},
                              GetBytes(initialVertices),
                              vertices) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::StorageIndirect, sizeof(initialArguments)},
                              {reinterpret_cast<const uint8*>(&initialArguments), sizeof(initialArguments)},
                              arguments) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::Index32, sizeof(initialIndices)},
                              GetBytes(initialIndices),
                              indices) == rhi::RasterStatus::Ready);

    const auto makeDispatch = [&](const rhi::RasterShaderDescription& description,
                                  std::span<const rhi::RasterBindingResource> resources) {
        rhi::RasterShaderHandle shader;
        rhi::BindingLayoutHandle layout;
        rhi::ComputeDispatch dispatch;
        REQUIRE(rhi::CreateRasterShader(device, description, shader) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateBindingLayout(device, description.Bindings, layout) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateBindingSet(device, layout, resources, dispatch.Bindings) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateComputePipeline(device, {shader, layout}, dispatch.Pipeline) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, shader) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, layout) == rhi::RasterStatus::Ready);
        return dispatch;
    };
    const rhi::RasterShaderDescription advanceShader = ludus::shaders::ordered_advance::Compute();
    const rhi::RasterShaderDescription prepareShader = ludus::shaders::ordered_prepare::Compute();
    const rhi::RasterBindingResource counterBinding{advanceShader.Bindings[0].Binding,
                                                    counter,
                                                    counterOffset,
                                                    sizeof(uint32)};
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> prepareBindings{{
        {prepareShader.Bindings[0].Binding, counter, counterOffset, sizeof(uint32)},
        {prepareShader.Bindings[1].Binding, vertices, 0, sizeof(initialVertices)},
        {prepareShader.Bindings[2].Binding, arguments, 0, sizeof(initialArguments)},
    }};
    rhi::ComputeDispatch advance = makeDispatch(advanceShader, {&counterBinding, 1});
    rhi::ComputeDispatch prepare = makeDispatch(prepareShader, prepareBindings.AsSpan());

    rhi::RasterShaderHandle vertex;
    rhi::RasterShaderHandle fragment;
    rhi::BindingLayoutHandle rasterLayout;
    rhi::BindingSetHandle rasterSet;
    rhi::RasterPipelineHandle rasterPipeline;
    REQUIRE(rhi::CreateRasterShader(device, ludus::shaders::ordered_draw::Vertex(), vertex) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(device, ludus::shaders::ordered_draw::Fragment(), fragment) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingLayout(device, {}, rasterLayout) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingSet(device, rasterLayout, {}, rasterSet) == rhi::RasterStatus::Ready);
    constexpr usize COORDINATE_STRIDE = sizeof(float32) * FLOAT2_VALUES;
    const rhi::RasterVertexStream stream{COORDINATE_STRIDE, false};
    const rhi::RasterVertexAttribute attribute{0, 0, 0, rhi::RasterVertexFormat::Float2};
    REQUIRE(rhi::CreateRasterPipeline(device,
                                      {vertex,
                                       fragment,
                                       rasterLayout,
                                       {&stream, 1},
                                       {&attribute, 1},
                                       false,
                                       false,
                                       rhi::RasterTarget::Rgba8Unorm},
                                      rasterPipeline) == rhi::RasterStatus::Ready);
    rhi::TextureHandle target;
    REQUIRE(rhi::CreateTexture(device, { .Width = WIDTH, .Height = HEIGHT, .Attachment = true }, {}, target) ==
            rhi::RasterStatus::Ready);
    rhi::RasterTextureState targetState;
    REQUIRE(rhi::GetTextureState(device, target, targetState) == rhi::RasterStatus::Ready);

    rhi::OrderedGraph graph;
    rhi::GraphVersion counterInput;
    rhi::GraphVersion vertexInput;
    rhi::GraphVersion indirectInput;
    rhi::GraphVersion indexInput;
    rhi::GraphVersion targetInput;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, counter, counterInput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, vertices, vertexInput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, arguments, indirectInput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, indices, indexInput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphTexture(device,
                                    graph,
                                    target,
                                    targetState,
                                    rhi::RasterTextureUse::ColorAttachment,
                                    targetInput) == rhi::RasterStatus::Ready);
    constexpr usize ADVANCE_PASS_COUNT = 2;
    for (usize pass = 0; pass < ADVANCE_PASS_COUNT; ++pass)
    {
        rhi::GraphVersion next;
        REQUIRE(rhi::NextGraphVersion(device, graph, counterInput, next) == rhi::RasterStatus::Ready);
        const rhi::GraphUse use{next,
                                rhi::GraphAccessMode::StorageReadWrite,
                                counterOffset,
                                sizeof(uint32),
                                rhi::RasterVisibility::Compute};
        REQUIRE(rhi::AddGraphPass(device,
                                  graph,
                                  {
                                      .Name = "advance counter",
                                      .Attachment = {},
                                      .Uses = &use,
                                      .UseCount = 1,
                                      .Dispatch = &advance,
                                  }) == rhi::RasterStatus::Ready);
        counterInput = next;
    }

    rhi::GraphVersion vertexOutput;
    rhi::GraphVersion indirectOutput;
    REQUIRE(rhi::NextGraphVersion(device, graph, vertexInput, vertexOutput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, indirectInput, indirectOutput) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, BINDING_COUNT> prepareUses{{
        {counterInput,
         rhi::GraphAccessMode::StorageRead,
         counterOffset,
         sizeof(uint32),
         rhi::RasterVisibility::Compute},
        {vertexOutput,
         rhi::GraphAccessMode::StorageReadWrite,
         0,
         sizeof(initialVertices),
         rhi::RasterVisibility::Compute},
        {indirectOutput,
         rhi::GraphAccessMode::StorageReadWrite,
         0,
         sizeof(initialArguments),
         rhi::RasterVisibility::Compute},
    }};
    REQUIRE(rhi::AddGraphPass(device,
                              graph,
                              {
                                  .Name = "prepare indirect draw",
                                  .Attachment = {},
                                  .Uses = prepareUses.GetData(),
                                  .UseCount = prepareUses.GetSize(),
                                  .Dispatch = &prepare,
                              }) == rhi::RasterStatus::Ready);

    rhi::GraphVersion targetOutput;
    REQUIRE(rhi::NextGraphVersion(device, graph, targetInput, targetOutput) == rhi::RasterStatus::Ready);
    constexpr usize DRAW_USE_COUNT = 4;
    const StaticArray<rhi::GraphUse, DRAW_USE_COUNT> drawUses{{
        {vertexOutput, rhi::GraphAccessMode::Vertex, 0, sizeof(initialVertices)},
        {indirectOutput, rhi::GraphAccessMode::Indirect, 0, sizeof(initialArguments)},
        {indexInput, rhi::GraphAccessMode::Index, 0, sizeof(initialIndices)},
        {targetOutput, rhi::GraphAccessMode::ColorWrite},
    }};
    const rhi::RasterVertexSlice slice{vertices, 0, sizeof(initialVertices)};
    const rhi::RasterDraw draw
    {
        .Pipeline = rasterPipeline,
        .Bindings = rasterSet,
        .Vertices = {&slice, 1},
        .Indices = indices,
        .IndexCount = TRIANGLE_INDEX_COUNT,
        .VertexCount = TRIANGLE_INDEX_COUNT,
        .InstanceCount = 1,
        .Indirect = arguments,
    };
    REQUIRE(rhi::AddGraphPass(device,
                              graph,
                              {
                                  .Name = "draw generated triangle",
                                  .Attachment = { .Color = target },
                                  .Uses = drawUses.GetData(),
                                  .UseCount = drawUses.GetSize(),
                                  .Draws = &draw,
                                  .DrawCount = 1,
                              }) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::AddGraphRoot(device, graph, targetOutput, rhi::GraphRoot::Readback) == rhi::RasterStatus::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == rhi::RasterStatus::Ready);
    REQUIRE(report.ComputePasses == ADVANCE_PASS_COUNT + 1);
    REQUIRE(report.RasterPasses == 1);

    REQUIRE(rhi::Destroy(device, advance.Pipeline) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, advance.Bindings) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, prepare.Pipeline) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, prepare.Bindings) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
#if defined(_WIN32)
    REQUIRE(rhi::ArmGpuPassTimings(device) == rhi::RasterStatus::Ready);
#endif
    REQUIRE(rhi::ExecuteOrderedGraph(device, owner.Surface, graph, completion) == rhi::RasterStatus::Ready);

    rhi::ReadbackTicket counterReadback;
    REQUIRE(rhi::RequestBufferReadback(device, counter, counterOffset, sizeof(uint32), counterReadback) ==
            rhi::RasterStatus::Pending);
    REQUIRE(rhi::Destroy(device, counter) == rhi::RasterStatus::Ready);
    RequireReady(device, counterReadback);
    uint32 actualCounter = 0;
    REQUIRE(
        rhi::CopyReadback(device, counterReadback, reinterpret_cast<uint8*>(&actualCounter), sizeof(actualCounter)) ==
        rhi::RasterStatus::Ready);
    CHECK(actualCounter == COUNTER_SEED + ADVANCE_PASS_COUNT);
    REQUIRE(rhi::Release(device, counterReadback) == rhi::RasterStatus::Ready);

    constexpr usize ROW_BYTES = WIDTH * CHANNELS;
    rhi::ReadbackTicket imageReadback;
    REQUIRE(rhi::RequestTextureReadback(device, target, {0, 0, 0, WIDTH, HEIGHT}, ROW_BYTES, imageReadback) ==
            rhi::RasterStatus::Pending);
    RequireReady(device, imageReadback);
    StaticArray<uint8, PIXEL_BYTES> pixels{};
    REQUIRE(rhi::CopyReadback(device, imageReadback, pixels.GetData(), pixels.GetSize()) == rhi::RasterStatus::Ready);
    const StaticArray<uint8, CHANNELS> expected{0, 255, 0, 255};
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        if (pixels[index] != expected[index % CHANNELS])
        {
            CAPTURE(index);
            FAIL("Indirect triangle did not cover the offscreen image");
        }
    }
    REQUIRE(rhi::Release(device, imageReadback) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetStatus(device, completion) == rhi::RasterStatus::Ready);
#if defined(_WIN32)
    rhi::GpuPassTimings timings;
    REQUIRE(rhi::GetGpuPassTimings(device, timings) == rhi::RasterStatus::Ready);
    REQUIRE(timings.State == rhi::GpuTimingState::Available);
    REQUIRE(timings.Count == report.ComputePasses + report.RasterPasses);
    CHECK(timings.DroppedPasses == 0);
    for (usize index = 0; index < timings.Count; ++index)
    {
        const rhi::GpuPassTiming& timing = timings.Passes[index];
        CHECK(timing.Timing.State == rhi::GpuTimingState::Available);
        CHECK(timing.Timing.DurationNanoseconds >= 0);
        const bool isCompute = index < report.ComputePasses;
        CHECK(timing.Kind == (isCompute == true ? rhi::GpuPassKind::Compute : rhi::GpuPassKind::Raster));
        const char* expectedName = index < ADVANCE_PASS_COUNT ? "advance counter"
                                   : isCompute == true        ? "prepare indirect draw"
                                                              : "draw generated triangle";
        CHECK(std::string_view(timing.Scope.Name.GetData()) == expectedName);
    }
#endif
    owner.CheckDebugMessages();
}
