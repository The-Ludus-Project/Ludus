#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "r9.h"

#include <cstring>

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#endif
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
constexpr logging::LogCategory LOG_R9{"R9-Fixture"};
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
constexpr usize TEXTURE_COUNT = 12;
constexpr usize VIEW_COUNT = 13;
constexpr uint32 CASE_COUNT = 20;
constexpr uint32 ARRAY_LAYERS = 257;
uint32 gArrayLayers = ARRAY_LAYERS;
uint32 gVolumeDepth = 0;
StaticArray<rhi::TextureHandle, TEXTURE_COUNT> gTextures{};
rhi::TextureHandle gTarget;
StaticArray<rhi::TextureViewHandle, VIEW_COUNT> gViews{};
rhi::BufferHandle gVertices;
rhi::BufferHandle gIndices;
StaticArray<rhi::BufferHandle, CASE_COUNT> gUniforms{};
StaticArray<rhi::BindingSetHandle, CASE_COUNT> gSets{};
rhi::SamplerHandle gSampler;
rhi::BindingLayoutHandle gLayout;
rhi::RasterShaderHandle gVertex;
rhi::RasterShaderHandle gFragment;
rhi::RasterPipelineHandle gPipeline;
rhi::OrderedGraph gGraph;
bool gGraphBuilt = false;
rhi::ReadbackTicket gRead;
rhi::TransferTicket gWrite;
uint32 gStage = 0;
uint32 gFrames = 0;
uint32 gCase = 0;
bool gDone = false;
bool gFailed = false;
bool gBC = false;
bool gHasBc5 = false;
uint32 gLargeWidth = 0;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// Application conformance reporting, outside the engine API.
// clang-format off
EM_JS(int32, Selection, (), { const b = new URL(location.href).searchParams.get('backend'); return b === 'webgpu' ? 1 : b === 'webgl2' ? 2 : 0; });
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(void, Report, (uint32 stage, uint32 frames, int32 failed, int32 done), {
    const n = document.getElementById('status'); n.dataset.state = failed ? 'failed' : done ? 'passed' : frames ? 'rendering' : 'loading';
    n.dataset.stage = stage; n.dataset.frames = frames; n.textContent = n.dataset.state + ' stage=' + stage;
});
// clang-format on
#else
int32 Selection() noexcept
{
    return 0;
}
int32 Paused() noexcept
{
    return 0;
}
void Report(uint32, uint32, int32, int32) noexcept {}
#endif

void Stop(bool failed) noexcept
{
    if (failed)
    {
        LUDUS_LOG_ERROR(LOG_R9, "R9 fixture failed at stage {} case {}", gStage, gCase);
    }
    gFailed = failed;
    gDone = true;
    Report(gStage, gFrames, failed ? 1 : 0, 1);
    rhi::Shutdown();
    (void)logging::LogSystem::Shutdown();
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
}
bool Accepted(rhi::RasterStatus value) noexcept
{
    return value == rhi::RasterStatus::Ready || value == rhi::RasterStatus::Pending;
}
template <typename T>
bool Complete(T handle) noexcept
{
    const rhi::RasterStatus status = rhi::GetStatus(gDevice, handle);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        Stop(true);
    }
    return status == rhi::RasterStatus::Ready;
}
template <typename T, usize N>
ludus::foundation::BorrowedView<const uint8> Bytes(const T (&values)[N]) noexcept
{
    return {reinterpret_cast<const uint8*>(values), sizeof(values)};
}
constexpr uint8 COLORS[6][4]{{255, 0, 0, 255},
                             {0, 255, 0, 255},
                             {0, 0, 255, 255},
                             {255, 255, 0, 255},
                             {255, 0, 255, 255},
                             {0, 255, 255, 255}};
void Fill(uint8* bytes, usize pixels, const uint8* color) noexcept
{
    for (usize i = 0; i < pixels; ++i)
    {
        std::memcpy(bytes + i * 4, color, 4);
    }
}
// Thanks to Microsoft, "Block Compression (Direct3D 10)", BC5:
// https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression
// Independent endpoint palettes and per-texel 3-bit indices exercise both channel modes.
constexpr usize BC5_BLOCK_BYTES = 16;
constexpr usize BC5_CHAIN_BYTES = 80;
struct Bc5PrimaryEndpoints final
{
    uint8 Red = 0;
    uint8 Green = 0;
};
void EncodeBc5Block(uint8* outBytes, Bc5PrimaryEndpoints endpoints, bool hasIndexedPalette) noexcept
{
    outBytes[0] = endpoints.Red;
    outBytes[1] = hasIndexedPalette == true ? static_cast<uint8>(250 - endpoints.Red) : 0;
    outBytes[8] = endpoints.Green;
    outBytes[9] = hasIndexedPalette == true ? static_cast<uint8>(240 - endpoints.Green) : 0;
    for (usize pixel = 0; pixel < 16; ++pixel)
    {
        for (usize channel = 0; channel < 2; ++channel)
        {
            const uint64 index = hasIndexedPalette == true ? (channel == 0 ? pixel % 8 : 7 - pixel % 8) : 0;
            const usize shift = pixel * 3;
            const uint64 encoded = index << (shift % 8);
            const usize offset = channel * 8 + 2 + shift / 8;
            outBytes[offset] |= static_cast<uint8>(encoded & 255);
            if (shift % 8 > 5)
            {
                outBytes[offset + 1] |= static_cast<uint8>(encoded >> 8);
            }
        }
    }
}
bool CreateExtendedTextures() noexcept
{
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(gDevice, info) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    rhi::FormatCapabilities bc5;
    gHasBc5 = rhi::GetFormatCapabilities(gDevice, rhi::RasterFormat::Bc5RgUnorm, bc5) == rhi::RasterStatus::Ready;
    StaticArray<uint8, BC5_CHAIN_BYTES> encoded{};
    EncodeBc5Block(encoded.GetData(), {230, 40}, true);
    EncodeBc5Block(encoded.GetData() + 16, {20, 200}, true);
    for (usize offset = 32; offset < encoded.GetSize(); offset += BC5_BLOCK_BYTES)
    {
        EncodeBc5Block(encoded.GetData() + offset, {128, 64}, false);
    }
    constexpr usize MIP_COUNT = 4;
    const StaticArray<rhi::TextureMipUpload, MIP_COUNT> blockMips{{{0, 32}, {32, 16}, {48, 16}, {64, 16}}};
    const StaticArray<rhi::TextureMipUpload, MIP_COUNT> rgbaMips{{{0, 32}, {128, 16}, {160, 8}, {168, 4}}};
    constexpr usize FALLBACK_BYTES = 172;
    StaticArray<uint8, FALLBACK_BYTES> fallback{};
    for (usize pixel = 0; pixel < fallback.GetSize() / 4; ++pixel)
    {
        fallback[pixel * 4] = 128;
        fallback[pixel * 4 + 1] = 64;
        fallback[pixel * 4 + 3] = 255;
    }
    if (Accepted(rhi::CreateTexture(
            gDevice,
            {
                .Width = 8,
                .Height = 4,
                .Format = gHasBc5 == true ? rhi::RasterFormat::Bc5RgUnorm : rhi::RasterFormat::Rgba8Unorm,
                .MipLevels = 4,
            },
            gHasBc5 == true ? rhi::TextureUpload{{encoded.GetData(), encoded.GetSize()},
                                                 0,
                                                 {blockMips.GetData(), blockMips.GetSize()}}
                            : rhi::TextureUpload{{fallback.GetData(), fallback.GetSize()},
                                                 0,
                                                 {rgbaMips.GetData(), rgbaMips.GetSize()}},
            gTextures[7])) == false)
    {
        return false;
    }
    // Exercise the reference width when enabled. Lower dimension profiles use
    // explicit caller-authored row padding to retain an upload above 64 MiB.
    const uint32 dimension =
        info.Profile.Formats[static_cast<usize>(rhi::RasterFormat::Rgba8Unorm)].MaxSampledDimension2D;
    gLargeWidth = dimension < 8240 ? dimension : 8240;
    const uint32 largeHeight = dimension < 2049 ? dimension : 2049;
    if (gLargeWidth == 0 || largeHeight == 0)
    {
        return false;
    }
    constexpr usize UPLOAD_ALIGNMENT = 4;
    constexpr usize MIN_UPLOAD_BYTES = usize{64} * 1024 * 1024 + UPLOAD_ALIGNMENT;
    const usize minimumRowBytes = MIN_UPLOAD_BYTES / largeHeight + (MIN_UPLOAD_BYTES % largeHeight != 0 ? 1U : 0U);
    const usize paddedRowBytes = (minimumRowBytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
    const usize tightRowBytes = usize{gLargeWidth} * UPLOAD_ALIGNMENT;
    const usize rowPitch = tightRowBytes > paddedRowBytes ? tightRowBytes : paddedRowBytes;
    Array<uint8> large;
    if (large.TryResize(rowPitch * largeHeight) == false)
    {
        return false;
    }
    std::memset(large.GetData(), 0, large.GetSize());
    for (usize row = 0; row < largeHeight; ++row)
    {
        for (usize x = 0; x < gLargeWidth; ++x)
        {
            const usize pixel = row * rowPitch + x * UPLOAD_ALIGNMENT;
            large[pixel] = static_cast<uint8>((x * 8 / gLargeWidth) * 31);
            large[pixel + 1] = 67;
            large[pixel + 2] = 149;
            large[pixel + 3] = 255;
        }
    }
    if (Accepted(rhi::CreateTexture(gDevice,
                                    { .Width = gLargeWidth, .Height = largeHeight },
                                    {{large.GetData(), large.GetSize()}, rowPitch},
                                    gTextures[8])) == false)
    {
        return false;
    }
    LUDUS_LOG_INFO(LOG_R9,
                   "R18c complete upload {} bytes; array layers {}; volume depth {}",
                   large.GetSize(),
                   gArrayLayers,
                   gVolumeDepth);
    constexpr usize FACE_COUNT = 6;
    constexpr usize CUBE_ENCODED_BYTES = FACE_COUNT * BC5_BLOCK_BYTES;
    constexpr usize CUBE_RGBA_BYTES = FACE_COUNT * 4 * 4 * 4;
    StaticArray<uint8, CUBE_ENCODED_BYTES> cube{};
    StaticArray<uint8, CUBE_RGBA_BYTES> rgba{};
    for (usize face = 0; face < FACE_COUNT; ++face)
    {
        EncodeBc5Block(cube.GetData() + face * BC5_BLOCK_BYTES,
                       {static_cast<uint8>(face == 0 ? 40 : 230), static_cast<uint8>(face == 0 ? 200 : 20)},
                       false);
        for (usize pixel = face * 16; pixel < (face + 1) * 16; ++pixel)
        {
            rgba[pixel * 4] = face == 0 ? 40 : 230;
            rgba[pixel * 4 + 1] = face == 0 ? 200 : 20;
            rgba[pixel * 4 + 3] = 255;
        }
    }
    const rhi::RasterFormat format = gHasBc5 == true ? rhi::RasterFormat::Bc5RgUnorm : rhi::RasterFormat::Rgba8Unorm;
    if (Accepted(rhi::CreateTexture(gDevice,
                                    {
                                        .Width = 4,
                                        .Height = 4,
                                        .Format = format,
                                        .Dimension = rhi::TextureDimension::D2Array,
                                        .DepthOrLayers = 2,
                                    },
                                    gHasBc5 == true ? rhi::TextureUpload{{cube.GetData(), 32}, 16}
                                                    : rhi::TextureUpload{{rgba.GetData(), 128}, 16},
                                    gTextures[9])) == false ||
        Accepted(rhi::CreateTexture(gDevice,
                                    {
                                        .Width = 4,
                                        .Height = 4,
                                        .Format = format,
                                        .Dimension = rhi::TextureDimension::Cube,
                                        .DepthOrLayers = 6,
                                    },
                                    gHasBc5 == true ? rhi::TextureUpload{{cube.GetData(), cube.GetSize()}, 16}
                                                    : rhi::TextureUpload{{rgba.GetData(), rgba.GetSize()}, 16},
                                    gTextures[10])) == false)
    {
        return false;
    }
    constexpr usize EDGE_BYTES = 32;
    constexpr usize EDGE_RGBA_BYTES = usize{7} * 3 * 4;
    StaticArray<uint8, EDGE_BYTES> edge{};
    EncodeBc5Block(edge.GetData(), {230, 20}, false);
    EncodeBc5Block(edge.GetData() + BC5_BLOCK_BYTES, {230, 20}, false);
    const bool hasNativeEdges = gHasBc5 == true && bc5.HasUnalignedBlockExtents == true;
    StaticArray<uint8, EDGE_RGBA_BYTES> edgeRgba{};
    constexpr usize CHANNEL_COUNT = 4;
    const StaticArray<uint8, CHANNEL_COUNT> edgeColor{{230, 20, 0, 255}};
    Fill(edgeRgba.GetData(), usize{7} * 3, edgeColor.GetData());
    if (gHasBc5 == true && hasNativeEdges == false)
    {
        rhi::TextureHandle unsupported;
        if (rhi::CreateTexture(gDevice,
                               { .Width = 7, .Height = 3, .Format = rhi::RasterFormat::Bc5RgUnorm },
                               {{edge.GetData(), edge.GetSize()}, 32},
                               unsupported) != rhi::RasterStatus::Unsupported ||
            rhi::IsNull(unsupported) == false)
        {
            return false;
        }
    }
    if (Accepted(rhi::CreateTexture(
            gDevice,
            {
                .Width = 7,
                .Height = 3,
                .Format = hasNativeEdges == true ? rhi::RasterFormat::Bc5RgUnorm : rhi::RasterFormat::Rgba8Unorm,
            },
            hasNativeEdges == true ? rhi::TextureUpload{{edge.GetData(), edge.GetSize()}, 32}
                                   : rhi::TextureUpload{{edgeRgba.GetData(), edgeRgba.GetSize()}, 28},
            gTextures[11])) == false)
    {
        return false;
    }
    LUDUS_LOG_INFO(LOG_R9, "R18 BC5 unaligned edges enabled {}", hasNativeEdges);
    LUDUS_LOG_INFO(LOG_R9,
                   "R18 BC5 enabled {}; sampled width {} device limit {}",
                   gHasBc5,
                   gLargeWidth,
                   info.Profile.Raster.MaxTextureDimension2D);
    return true;
}
bool CreateResources() noexcept
{
    const float32 vertices[16]{-1, 1, 0, 0, -1, -1, 0, 1, 1, 1, 1, 0, 1, -1, 1, 1};
    const uint16 indices[6]{0, 1, 2, 2, 1, 3};
    if (Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Vertex, sizeof(vertices)}, Bytes(vertices), gVertices)) ==
            false ||
        Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Index16, sizeof(indices)}, Bytes(indices), gIndices)) ==
            false ||
        Accepted(rhi::CreateTexture(gDevice,
                                    { .Width = 4, .Height = 4, .MipLevels = 3, .DeferredInitialization = true },
                                    {},
                                    gTextures[0])) == false ||
        Accepted(rhi::CreateTexture(gDevice, { .Width = 8, .Height = 8, .Attachment = true }, {}, gTarget)) == false)
    {
        return false;
    }
    rhi::FormatCapabilities rgba;
    if (rhi::GetFormatCapabilities(gDevice, rhi::RasterFormat::Rgba8Unorm, rgba) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    gArrayLayers = rgba.MaxArrayLayers < ARRAY_LAYERS ? rgba.MaxArrayLayers : ARRAY_LAYERS;
    if (rgba.MaxDimension3D < 4)
    {
        return false;
    }
    gVolumeDepth = 4;
    while (gVolumeDepth < 512 && gVolumeDepth * 2 <= rgba.MaxDimension3D)
    {
        gVolumeDepth *= 2;
    }
    constexpr usize ARRAY_BYTES = usize{ARRAY_LAYERS} * 64;
    StaticArray<uint8, ARRAY_BYTES> arrayBytes{};
    for (usize layer = 0; layer < gArrayLayers; ++layer)
    {
        Fill(arrayBytes.GetData() + layer * 64, 16, COLORS[layer == gArrayLayers - 1 ? 1 : 0]);
    }
    uint8 bytes[384]{};
    for (uint32 slice = 0; slice < 3; ++slice)
    {
        Fill(bytes + usize{slice} * 64, 16, COLORS[slice]);
    }
    if (Accepted(rhi::CreateTexture(
            gDevice,
            { .Width = 4, .Height = 4, .Dimension = rhi::TextureDimension::D2Array, .DepthOrLayers = gArrayLayers },
            {{arrayBytes.GetData(), usize{gArrayLayers} * 64}, 16},
            gTextures[1])) == false)
    {
        return false;
    }
    for (uint32 slice = 0; slice < 6; ++slice)
    {
        for (uint32 y = 0; y < 4; ++y)
        {
            for (uint32 x = 0; x < 4; ++x)
            {
                uint8* pixel = bytes + usize{slice} * 64 + usize{y} * 16 + usize{x} * 4;
                pixel[0] = static_cast<uint8>(31 + slice * 32);
                pixel[1] = x >= 2 ? 255 : 0;
                pixel[2] = y >= 2 ? 255 : 0;
                pixel[3] = 255;
            }
        }
    }
    if (Accepted(rhi::CreateTexture(gDevice,
                                    {
                                        .Width = 4,
                                        .Height = 4,
                                        .Dimension = rhi::TextureDimension::Cube,
                                        .DepthOrLayers = 6,
                                    },
                                    {bytes, 16},
                                    gTextures[2])) == false)
    {
        return false;
    }
    const usize volumeBaseBytes = usize{gVolumeDepth} * 64;
    const usize volumeSecondBytes = usize{gVolumeDepth / 2} * 16;
    const usize volumeBytesCount = volumeBaseBytes + volumeSecondBytes + usize{gVolumeDepth / 4} * 4;
    Array<uint8> volumeBytes;
    if (volumeBytes.TryResize(volumeBytesCount) == false)
    {
        return false;
    }
    for (usize slice = 0; slice < gVolumeDepth; ++slice)
    {
        Fill(volumeBytes.GetData() + slice * 64, 16, COLORS[slice / (gVolumeDepth / 4)]);
    }
    for (usize slice = 0; slice < gVolumeDepth / 2; ++slice)
    {
        Fill(volumeBytes.GetData() + volumeBaseBytes + slice * 16, 4, COLORS[slice < gVolumeDepth / 4 ? 4 : 5]);
    }
    Fill(volumeBytes.GetData() + volumeBaseBytes + volumeSecondBytes, gVolumeDepth / 4, COLORS[0]);
    const rhi::TextureMipUpload mips[3]{{0, 16}, {volumeBaseBytes, 8}, {volumeBaseBytes + volumeSecondBytes, 4}};
    if (Accepted(rhi::CreateTexture(gDevice,
                                    {
                                        .Width = 4,
                                        .Height = 4,
                                        .MipLevels = 3,
                                        .Dimension = rhi::TextureDimension::D3,
                                        .DepthOrLayers = gVolumeDepth,
                                    },
                                    {{volumeBytes.GetData(), volumeBytes.GetSize()}, 0, mips},
                                    gTextures[3])) == false)
    {
        return false;
    }
    uint16 hdr[64]{};
    for (usize i = 0; i < 16; ++i)
    {
        hdr[i * 4] = 0x3800;
        hdr[i * 4 + 1] = 0x3400;
        hdr[i * 4 + 2] = 0x3c00;
        hdr[i * 4 + 3] = 0x3c00;
    }
    if (Accepted(rhi::CreateTexture(gDevice,
                                    { .Width = 4, .Height = 4, .Format = rhi::RasterFormat::Rgba16Float },
                                    rhi::TextureUpload{Bytes(hdr), 32},
                                    gTextures[4])) == false)
    {
        return false;
    }
    rhi::FormatCapabilities bc1;
    rhi::FormatCapabilities bc3;
    gBC = rhi::GetFormatCapabilities(gDevice, rhi::RasterFormat::Bc1RgbaUnorm, bc1) == rhi::RasterStatus::Ready &&
          rhi::GetFormatCapabilities(gDevice, rhi::RasterFormat::Bc3RgbaUnorm, bc3) == rhi::RasterStatus::Ready;
    const uint8 block1[8]{0, 0xf8, 0, 0, 0, 0, 0, 0};
    const uint8 block3[16]{255, 0, 0, 0, 0, 0, 0, 0, 0xe0, 7, 0, 0, 0, 0, 0, 0};
    uint8 chain1[40]{};
    uint8 chain3[80]{};
    for (usize block = 0; block < 5; ++block)
    {
        std::memcpy(chain1 + block * 8, block1, 8);
        std::memcpy(chain3 + block * 16, block3, 16);
    }
    const rhi::TextureMipUpload bc1Mips[4]{{0, 16}, {16, 8}, {24, 8}, {32, 8}};
    const rhi::TextureMipUpload bc3Mips[4]{{0, 32}, {32, 16}, {48, 16}, {64, 16}};
    const rhi::TextureMipUpload rgbaMips[4]{{0, 32}, {128, 16}, {160, 8}, {168, 4}};
    Fill(bytes, 43, COLORS[0]);
    if (Accepted(rhi::CreateTexture(gDevice,
                                    {
                                        .Width = 8,
                                        .Height = 4,
                                        .Format = gBC ? rhi::RasterFormat::Bc1RgbaUnorm : rhi::RasterFormat::Rgba8Unorm,
                                        .MipLevels = 4,
                                    },
                                    gBC ? rhi::TextureUpload{chain1, 0, bc1Mips}
                                        : rhi::TextureUpload{{bytes, 172}, 0, rgbaMips},
                                    gTextures[5])) == false)
    {
        return false;
    }
    Fill(bytes, 43, COLORS[1]);
    LUDUS_LOG_INFO(LOG_R9, "R9 BC profile enabled {}", gBC);
    if (Accepted(rhi::CreateTexture(gDevice,
                                    {
                                        .Width = 8,
                                        .Height = 4,
                                        .Format = gBC ? rhi::RasterFormat::Bc3RgbaUnorm : rhi::RasterFormat::Rgba8Unorm,
                                        .MipLevels = 4,
                                    },
                                    gBC ? rhi::TextureUpload{chain3, 0, bc3Mips}
                                        : rhi::TextureUpload{{bytes, 172}, 0, rgbaMips},
                                    gTextures[6])) == false)
    {
        return false;
    }
    return CreateExtendedTextures();
}
bool CreateDraw() noexcept
{
    const rhi::RasterShaderDescription vertex = ludus::shaders::r9::Vertex();
    const rhi::RasterShaderDescription fragment = ludus::shaders::r9::Fragment();
    if (Accepted(rhi::CreateRasterShader(gDevice, vertex, gVertex)) == false ||
        Accepted(rhi::CreateRasterShader(gDevice, fragment, gFragment)) == false ||
        Accepted(rhi::CreateBindingLayout(gDevice, fragment.Bindings, gLayout)) == false ||
        Accepted(rhi::CreateSampler(gDevice, { .MipFilter = rhi::RasterMipFilter::Nearest }, gSampler)) == false ||
        Accepted(rhi::CreateTextureView(gDevice, gTextures[0], { .BaseMip = 1, .MipCount = 1 }, gViews[0])) == false)
    {
        return false;
    }
    for (usize i = 1; i < 7; ++i)
    {
        if (Accepted(i >= 5 ? rhi::CreateTextureView(gDevice, gTextures[i], { .BaseMip = 2, .MipCount = 1 }, gViews[i])
                            : rhi::CreateTextureView(gDevice, gTextures[i], gViews[i])) == false)
        {
            return false;
        }
    }
    if (Accepted(rhi::CreateTextureView(gDevice, gTextures[7], gViews[7])) == false ||
        Accepted(rhi::CreateTextureView(gDevice, gTextures[7], { .BaseMip = 2, .MipCount = 1 }, gViews[8])) == false ||
        Accepted(rhi::CreateTextureView(gDevice, gTextures[8], gViews[9])) == false ||
        Accepted(rhi::CreateTextureView(gDevice, gTextures[9], gViews[10])) == false ||
        Accepted(rhi::CreateTextureView(gDevice, gTextures[10], gViews[11])) == false ||
        Accepted(rhi::CreateTextureView(gDevice, gTextures[11], gViews[12])) == false)
    {
        return false;
    }
    rhi::TextureViewHandle forbidden;
    return rhi::CreateTextureView(gDevice, gTextures[0], forbidden) == rhi::RasterStatus::InvalidState;
}
bool CreateSets() noexcept
{
    for (uint32 test = 0; test < CASE_COUNT; ++test)
    {
        float32 settings[8]{};
        if (test == 1 || test == 17)
        {
            settings[0] = 1;
            settings[6] = test == 1 ? static_cast<float32>(gArrayLayers - 1) : 1;
        }
        else if (test >= 2 && test < 8)
        {
            settings[0] = 2;
            settings[4 + (test - 2) / 2] = (test % 2 == 0) ? 1.0F : -1.0F;
        }
        else if (test == 8)
        {
            settings[0] = 3;
            settings[6] = 0.375F;
        }
        else if (test == 9)
        {
            settings[0] = 3;
            settings[1] = 1;
            settings[6] = 0.75F;
        }
        else if (test == 10)
        {
            settings[0] = 3;
            settings[1] = 2;
            settings[6] = 0.5F;
        }
        else if (test == 11)
        {
            settings[0] = 4;
        }
        else if (test == 16)
        {
            settings[0] = 0;
        }
        else if (test == 18)
        {
            settings[0] = 2;
            settings[4] = 1;
        }
        else if (test >= 12)
        {
            settings[0] = 5;
        }
        if (Accepted(rhi::CreateBuffer(gDevice,
                                       {rhi::BufferRole::Uniform, sizeof(settings)},
                                       Bytes(settings),
                                       gUniforms[test])) == false)
        {
            return false;
        }
    }
    return true;
}
bool Verify() noexcept
{
    uint8 bytes[256]{};
    if (rhi::CopyReadback(gDevice, gRead, bytes, sizeof(bytes)) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    const uint8 hdr[4]{128, 64, 255, 255};
    constexpr usize CHANNEL_COUNT = 4;
    const StaticArray<uint8, CHANNEL_COUNT> bc5Tail{{128, 64, 0, 255}};
    const StaticArray<uint8, CHANNEL_COUNT> bc5Slice{{230, 20, 0, 255}};
    const StaticArray<uint8, CHANNEL_COUNT> bc5Cube{{40, 200, 0, 255}};
    const uint8* expected = gCase == 18   ? bc5Cube.GetData()
                            : gCase >= 17 ? bc5Slice.GetData()
                            : gCase >= 14 ? bc5Tail.GetData()
                            : gCase == 0  ? COLORS[0]
                            : gCase == 1  ? COLORS[1]
                            : gCase < 8   ? COLORS[gCase - 2]
                            : gCase == 8  ? COLORS[1]
                            : gCase == 9  ? COLORS[5]
                            : gCase == 10 ? COLORS[0]
                            : gCase == 11 ? hdr
                            : gCase == 12 ? COLORS[0]
                                          : COLORS[1];
    for (usize i = 0; i < sizeof(bytes); ++i)
    {
        const usize channel = i % 4;
        const usize x = (i / 4) % 8;
        const usize y = i / 32;
        const uint8 cubeExpected = channel == 0   ? static_cast<uint8>(31 + (gCase - 2) * 32)
                                   : channel == 1 ? (x >= 4 ? 255 : 0)
                                   : channel == 2 ? (y >= 4 ? 255 : 0)
                                                  : 255;
        uint8 value = gCase >= 2 && gCase < 8 ? cubeExpected : expected[channel];
        if (gCase == 16)
        {
            value = channel == 0 ? static_cast<uint8>(x * 31) : channel == 1 ? 67 : channel == 2 ? 149 : 255;
        }
        if (gCase == 14 && gHasBc5 == true && channel < 2)
        {
            const usize pixel = (y / 2) * 4 + x % 4;
            const usize index = channel == 0 ? pixel % 8 : 7 - pixel % 8;
            const uint32 endpoint0 = channel == 0 ? (x < 4 ? 230U : 20U) : (x < 4 ? 40U : 200U);
            const uint32 endpoint1 = channel == 0 ? 250 - endpoint0 : 240 - endpoint0;
            if (index < 2)
            {
                value = static_cast<uint8>(index == 0 ? endpoint0 : endpoint1);
            }
            else if (endpoint0 > endpoint1)
            {
                value = static_cast<uint8>(((8 - index) * endpoint0 + (index - 1) * endpoint1) / 7);
            }
            else
            {
                value = index == 6   ? 0
                        : index == 7 ? 255
                                     : static_cast<uint8>(((6 - index) * endpoint0 + (index - 1) * endpoint1) / 5);
            }
        }
        const int32 delta = static_cast<int32>(bytes[i]) - value;
        if (delta < -1 || delta > 1)
        {
            LUDUS_LOG_ERROR(LOG_R9, "R9 pixel byte {} actual {} expected {}", i, bytes[i], value);
            return false;
        }
    }
    return true;
}
rhi::RasterStatus DrawGraph() noexcept
{
    if (gGraphBuilt == false)
    {
        if (rhi::CreateOrderedGraph(gDevice, gGraph) != rhi::RasterStatus::Ready)
        {
            return rhi::RasterStatus::Failed;
        }
        rhi::GraphVersion versions[11];
        rhi::GraphVersion written;
        const rhi::BufferHandle buffers[3]{gVertices, gIndices, gUniforms[0]};
        for (usize i = 0; i < 3; ++i)
        {
            if (rhi::ImportGraphBuffer(gDevice, gGraph, buffers[i], versions[i]) != rhi::RasterStatus::Ready)
            {
                return rhi::RasterStatus::Failed;
            }
        }
        for (usize i = 0; i < 8; ++i)
        {
            const rhi::TextureHandle texture = i == 7 ? gTarget : gTextures[i];
            rhi::RasterTextureState state;
            if (rhi::GetTextureState(gDevice, texture, state) != rhi::RasterStatus::Ready ||
                rhi::ImportGraphTexture(gDevice,
                                        gGraph,
                                        texture,
                                        state,
                                        rhi::RasterTextureUse::SampledBoth,
                                        versions[i + 3]) != rhi::RasterStatus::Ready)
            {
                return rhi::RasterStatus::Failed;
            }
        }
        if (rhi::NextGraphVersion(gDevice, gGraph, versions[10], written) != rhi::RasterStatus::Ready)
        {
            return rhi::RasterStatus::Failed;
        }
        rhi::GraphUse uses[11]{{versions[0], rhi::GraphAccessMode::Vertex, 0, 64},
                               {versions[1], rhi::GraphAccessMode::Index, 0, 12},
                               {versions[2], rhi::GraphAccessMode::Uniform, 0, 32}};
        for (usize i = 0; i < 7; ++i)
        {
            uses[i + 3] = {versions[i + 3], rhi::GraphAccessMode::Sampled};
        }
        uses[10] = {written, rhi::GraphAccessMode::ColorWrite};
        const rhi::RasterVertexSlice slice{gVertices, 0, 64};
        const rhi::RasterDraw draw{gPipeline, gSets[0], {&slice, 1}, gIndices, 0, 6, 4, 1};
        rhi::GraphPassDescription pass;
        pass.Name = "R9 initialized views";
        pass.Attachment.Color = gTarget;
        pass.Uses = uses;
        pass.UseCount = 11;
        pass.Draws = &draw;
        pass.DrawCount = 1;
        rhi::GraphReport report;
        if (rhi::AddGraphPass(gDevice, gGraph, pass) != rhi::RasterStatus::Ready ||
            rhi::AddGraphRoot(gDevice, gGraph, written, rhi::GraphRoot::Readback) != rhi::RasterStatus::Ready ||
            rhi::CompileOrderedGraph(gDevice, gGraph, report) != rhi::RasterStatus::Ready)
        {
            return rhi::RasterStatus::Failed;
        }
        for (rhi::TextureHandle& texture : gTextures)
        {
            if (rhi::Destroy(gDevice, texture) != rhi::RasterStatus::Ready)
            {
                return rhi::RasterStatus::Failed;
            }
        }
        gGraphBuilt = true;
    }
    rhi::SubmissionToken completion;
    return rhi::ExecuteOrderedGraph(gDevice, gSurface, gGraph, completion);
}
void Tick() noexcept
{
    if (gDone || Paused() != 0)
    {
        return;
    }
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(gDevice, info) != rhi::DeviceStatus::Ready)
    {
        if (info.Startup.State != rhi::StartupState::Pending)
        {
            Stop(true);
        }
        return;
    }
    if (rhi::PollLifetime(gDevice) != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return;
    }
    if (gStage == 0)
    {
        if (CreateResources() == false)
        {
            Stop(true);
            return;
        }
        gStage = 1;
    }
    if (gStage == 1)
    {
        for (const rhi::TextureHandle texture : gTextures)
        {
            if (Complete(texture) == false)
            {
                return;
            }
        }
        if (Complete(gTarget) == false || Complete(gVertices) == false || Complete(gIndices) == false)
        {
            return;
        }
        uint8 red[16]{};
        Fill(red, 4, COLORS[0]);
        if (rhi::RequestTextureUpdate(gDevice, gTextures[0], {1, 0, 0, 2, 2}, red, 16, 8, gWrite) !=
            rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 2;
    }
    if (gStage == 2)
    {
        if (Complete(gWrite) == false)
        {
            return;
        }
        if (rhi::Release(gDevice, gWrite) != rhi::RasterStatus::Ready || CreateDraw() == false || CreateSets() == false)
        {
            Stop(true);
            return;
        }
        gStage = 3;
    }
    if (gStage == 3)
    {
        for (const rhi::TextureViewHandle view : gViews)
        {
            if (Complete(view) == false)
            {
                return;
            }
        }
        for (const rhi::BufferHandle uniform : gUniforms)
        {
            if (Complete(uniform) == false)
            {
                return;
            }
        }
        if (Complete(gLayout) == false || Complete(gVertex) == false || Complete(gFragment) == false ||
            Complete(gSampler) == false)
        {
            return;
        }
        for (uint32 test = 0; test < CASE_COUNT; ++test)
        {
            constexpr usize BINDING_COUNT = 8;
            const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{
                {{ .Buffer = gUniforms[test], .Size = 32 },
                 { .Binding = 1, .Texture = gViews[test == 16 ? 9 : 0] },
                 { .Binding = 2, .Sampler = gSampler },
                 { .Binding = 3, .Texture = gViews[test == 17 ? 10 : 1] },
                 { .Binding = 4, .Texture = gViews[test == 18 ? 11 : 2] },
                 { .Binding = 5, .Texture = gViews[3] },
                 { .Binding = 6, .Texture = gViews[4] },
                 {
                     .Binding = 7,
                     .Texture = gViews[test == 19   ? 12
                                       : test == 14 ? 7
                                       : test == 15 ? 8
                                       : test == 13 ? 6
                                                    : 5],
                 }}};
            if (Accepted(
                    rhi::CreateBindingSet(gDevice, gLayout, {resources.GetData(), resources.GetSize()}, gSets[test])) ==
                false)
            {
                Stop(true);
                return;
            }
        }
        const rhi::RasterVertexStream stream{16, false};
        const rhi::RasterVertexAttribute attributes[2]{{0, 0, 0, rhi::RasterVertexFormat::Float2},
                                                       {1, 0, 8, rhi::RasterVertexFormat::Float2}};
        rhi::RasterPipelineDescription pipeline;
        pipeline.Vertex = gVertex;
        pipeline.Fragment = gFragment;
        pipeline.Layout = gLayout;
        pipeline.Streams = {&stream, 1};
        pipeline.Attributes = attributes;
        pipeline.Depth = false;
        pipeline.Target = rhi::RasterTarget::Rgba8Unorm;
        if (Accepted(rhi::CreateRasterPipeline(gDevice, pipeline, gPipeline)) == false)
        {
            Stop(true);
            return;
        }
        gStage = 4;
    }
    if (gStage == 4)
    {
        if (Complete(gPipeline) == false || Complete(gSets[gCase]) == false)
        {
            return;
        }
        if (rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready)
        {
            Stop(true);
            return;
        }
        if (gCase == 0)
        {
            const rhi::RasterStatus status = DrawGraph();
            if (status == rhi::RasterStatus::NotReady)
            {
                return;
            }
            if (status != rhi::RasterStatus::Ready)
            {
                Stop(true);
                return;
            }
        }
        else
        {
            const rhi::DeviceStatus begin = rhi::BeginFrame(gDevice, gSurface);
            if (begin == rhi::DeviceStatus::Skipped)
            {
                return;
            }
            const rhi::RasterVertexSlice slice{gVertices, 0, 64};
            if (begin != rhi::DeviceStatus::Ready ||
                rhi::BeginRasterPass(gDevice, { .Color = gTarget }) != rhi::RasterStatus::Ready ||
                rhi::DrawIndexed(gDevice, {gPipeline, gSets[gCase], {&slice, 1}, gIndices, 0, 6, 4, 1}) !=
                    rhi::RasterStatus::Ready ||
                rhi::EndFrame(gDevice) != rhi::DeviceStatus::Ready)
            {
                Stop(true);
                return;
            }
        }
        if (rhi::RequestTextureReadback(gDevice, gTarget, {0, 0, 0, 8, 8}, 32, gRead) != rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 5;
    }
    if (gStage == 5)
    {
        if (Complete(gRead) == false)
        {
            return;
        }
        if (Verify() == false || rhi::Release(gDevice, gRead) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        ++gCase;
        ++gFrames;
        Report(gStage, gFrames, 0, 0);
        if (gCase == CASE_COUNT)
        {
            Stop(false);
            return;
        }
        gStage = 4;
    }
}
} // namespace
int main()
{
    (void)logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} });
    rhi::WindowInfo window;
    window.Width = 96;
    window.Height = 64;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo description;
    description.Width = 96;
    description.Height = 64;
    description.CanvasSelector = "#canvas";
    description.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(description, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription device;
    device.Required.PortableRaster = true;
    device.RequiredProfile.Raster.MinTextureUploadBytes = usize{64} * 1024 * 1024 + 4;
    device.RequiredProfile.Raster.MinTextureLayers = 256;
    device.RequiredProfile.FormatCount = 1;
    device.RequiredProfile.Formats[0].Uses.CopySource = true;
    device.RequiredProfile.Formats[0].Uses.CopyDestination = true;
    device.Storage.TransferSlices = 2;
    device.Storage.TransferBytes = 4096;
    device.Selection = Selection() == 1   ? rhi::BackendSelection::WebGPU
                       : Selection() == 2 ? rhi::BackendSelection::WebGL2
                                          : rhi::BackendSelection::Auto;
    const rhi::DeviceStatus created = rhi::CreateDevice({}, window, device, gDevice, gSurface);
    if (created != rhi::DeviceStatus::Ready && created != rhi::DeviceStatus::Pending)
    {
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const uint64 started = time::NowTicks();
    while (gDone == false && time::NowTicks() - started < 30 * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (gDone == false)
    {
        Stop(true);
    }
#endif
    return gFailed ? 1 : 0;
}
