#include "conformance_mask.h"
#include "conformance_sample.h"
#include "conformance_write.h"
#include "internal/readback.h"
#include "raster.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#if defined(_WIN32)
#    include "internal/d3d12_device.h"
#    include <d3d12sdklayers.h>
#endif

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// The same shader, packet authoring and complete-image oracle run on D3D12,
// Vulkan and Metal. No backend-specific expected image or rendering substitute.
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
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
    DeviceOwner()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
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
void WaitForTexture(rhi::DeviceHandle device, rhi::TextureHandle texture, rhi::RasterStatus status)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (status == rhi::RasterStatus::Pending && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        status = rhi::GetStatus(device, texture);
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
    WaitForTexture(owner.Device, texture, textureStatus);
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
    pass.ClearDepth = hasReverseDepth == true ? 0 : 1;
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
