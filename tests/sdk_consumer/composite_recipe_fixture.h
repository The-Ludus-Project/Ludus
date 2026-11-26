#pragma once
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

// The renderer_composite recipe fixture of LUSL L5d wave 2 (issue 1736), shared by the renderer's
// GPU suite (tests/lusl_recipe_parity_tests.cpp) and the installed SDK consumer
// (composite_recipe.cpp). It draws a small asymmetric texture through the composite stages with a
// fixed cropped quad, binds the texture and sampler at the slots the caller names (the generated
// renderer_composite_slots.h constants), and reads the target back through the public RHI.
// The expected image is derived from the fixture inputs: nearest sampling of the texel under each
// covered pixel's centre, and the clear colour elsewhere. Each texel covers a block of whole
// pixels whose centres lie well inside it, so a correct draw is exact on every backend, while a
// linear sampler blends neighbouring texels into the pixels next to each texel edge.
namespace ludus::qa::composite_recipe
{
namespace rhi = graphics::rhi;
using namespace foundation;

/// Square render target edge, in pixels.
inline constexpr uint32 TARGET_EXTENT = 16;
/// Source image extent, in texels.
inline constexpr uint32 IMAGE_WIDTH = 4;
inline constexpr uint32 IMAGE_HEIGHT = 2;
inline constexpr uint32 CHANNELS = 4;
inline constexpr usize TARGET_BYTES = usize{TARGET_EXTENT} * TARGET_EXTENT * CHANNELS;
inline constexpr usize IMAGE_BYTES = usize{IMAGE_WIDTH} * IMAGE_HEIGHT * CHANNELS;
/// The quad covers target pixels [CROP_BEGIN, CROP_END) on both axes.
inline constexpr uint32 CROP_BEGIN = 4;
inline constexpr uint32 CROP_END = 12;
/// It shows texel columns [TEXEL_X_BEGIN, TEXEL_X_END) and rows [TEXEL_Y_BEGIN, TEXEL_Y_END).
inline constexpr uint32 TEXEL_X_BEGIN = 1;
inline constexpr uint32 TEXEL_X_END = 3;
inline constexpr uint32 TEXEL_Y_BEGIN = 0;
inline constexpr uint32 TEXEL_Y_END = 2;
static_assert((CROP_END - CROP_BEGIN) % (TEXEL_X_END - TEXEL_X_BEGIN) == 0);
static_assert((CROP_END - CROP_BEGIN) % (TEXEL_Y_END - TEXEL_Y_BEGIN) == 0);
/// Target pixels per texel on each axis; at least four, so pixel centres sit an eighth of a texel
/// or more inside their texel.
inline constexpr uint32 PIXELS_PER_TEXEL_X = (CROP_END - CROP_BEGIN) / (TEXEL_X_END - TEXEL_X_BEGIN);
inline constexpr uint32 PIXELS_PER_TEXEL_Y = (CROP_END - CROP_BEGIN) / (TEXEL_Y_END - TEXEL_Y_BEGIN);
inline constexpr uint32 MINIMUM_PIXELS_PER_TEXEL = 4;
static_assert(PIXELS_PER_TEXEL_X >= MINIMUM_PIXELS_PER_TEXEL && PIXELS_PER_TEXEL_Y >= MINIMUM_PIXELS_PER_TEXEL);
inline constexpr uint8 OPAQUE = 255;
/// Opaque, pairwise distinct texels with no mirror symmetry, in top-left row order; the shown
/// columns 1 and 2 also differ from their hidden neighbours, so a wrong crop changes pixels.
inline constexpr StaticArray<uint8, IMAGE_BYTES> IMAGE_TEXELS{{
    // clang-format off
    250, 20, 30, OPAQUE,   40, 200, 60, OPAQUE,   70, 80, 240, OPAQUE,   230, 220, 10, OPAQUE,
    15, 160, 170, OPAQUE,  190, 25, 180, OPAQUE,  120, 130, 35, OPAQUE,  5, 90, 100, OPAQUE,
    // clang-format on
}};
/// Clear colour outside the quad, distinct from every texel.
inline constexpr StaticArray<uint8, CHANNELS> CLEAR_BYTES{{0, 0, 0, OPAQUE}};
inline constexpr float32 UNORM8_MAXIMUM = 255;
inline constexpr usize QUAD_VERTEX_COUNT = 4;
inline constexpr usize QUAD_INDEX_COUNT = 6;
inline constexpr usize QUAD_COMPONENTS = 4;
inline constexpr StaticArray<uint32, QUAD_INDEX_COUNT> QUAD_INDICES{{0, 1, 2, 2, 3, 0}};
inline constexpr uint64 WAIT_NANOSECONDS = time::Seconds(10).Nanoseconds;
inline constexpr uint64 FNV_OFFSET_BASIS = 14695981039346656037ULL;
inline constexpr uint64 FNV_PRIME = 1099511628211ULL;

using Pixels = StaticArray<uint8, TARGET_BYTES>;

/// The composite stages and where they read their texture, sampler and vertex data. Slots and the
/// location come from the recipe's generated renderer_composite_slots.h, never from literals.
struct CompositeStages final
{
    rhi::RasterShaderDescription Vertex{};
    rhi::RasterShaderDescription Fragment{};
    uint32 ImageSlot = 0;
    uint32 SamplerSlot = 0;
    uint32 PositionUvLocation = 0;
};

/// Clip-space position (xy) and top-left UV (zw) of the quad, in renderer presentation order:
/// bottom-left, bottom-right, top-right, top-left.
inline StaticArray<float32, QUAD_VERTEX_COUNT * QUAD_COMPONENTS> MakeQuadVertices() noexcept
{
    const auto toClipX = [](uint32 pixel) { return 2 * static_cast<float32>(pixel) / TARGET_EXTENT - 1; };
    const auto toClipY = [](uint32 pixel) { return 1 - 2 * static_cast<float32>(pixel) / TARGET_EXTENT; };
    const float32 left = toClipX(CROP_BEGIN);
    const float32 right = toClipX(CROP_END);
    const float32 top = toClipY(CROP_BEGIN);
    const float32 bottom = toClipY(CROP_END);
    const float32 u0 = static_cast<float32>(TEXEL_X_BEGIN) / IMAGE_WIDTH;
    const float32 u1 = static_cast<float32>(TEXEL_X_END) / IMAGE_WIDTH;
    const float32 v0 = static_cast<float32>(TEXEL_Y_BEGIN) / IMAGE_HEIGHT;
    const float32 v1 = static_cast<float32>(TEXEL_Y_END) / IMAGE_HEIGHT;
    return {{left, bottom, u0, v1, right, bottom, u1, v1, right, top, u1, v0, left, top, u0, v0}};
}

/// Byte offset of target pixel (x, y), top-left origin.
inline usize GetPixelOffset(uint32 x, uint32 y) noexcept
{
    return (usize{y} * TARGET_EXTENT + x) * CHANNELS;
}

/// The image a correct draw produces, derived from the inputs above.
inline void ComputeExpectedPixels(Pixels& outPixels) noexcept
{
    for (uint32 y = 0; y < TARGET_EXTENT; ++y)
    {
        for (uint32 x = 0; x < TARGET_EXTENT; ++x)
        {
            const bool isCovered = x >= CROP_BEGIN && x < CROP_END && y >= CROP_BEGIN && y < CROP_END;
            const usize target = GetPixelOffset(x, y);
            for (uint32 channel = 0; channel < CHANNELS; ++channel)
            {
                uint8 value = CLEAR_BYTES[channel];
                if (isCovered == true)
                {
                    const uint32 texelX = TEXEL_X_BEGIN + (x - CROP_BEGIN) / PIXELS_PER_TEXEL_X;
                    const uint32 texelY = TEXEL_Y_BEGIN + (y - CROP_BEGIN) / PIXELS_PER_TEXEL_Y;
                    value = IMAGE_TEXELS[(usize{texelY} * IMAGE_WIDTH + texelX) * CHANNELS + channel];
                }
                outPixels[target + channel] = value;
            }
        }
    }
}

/// Number of pixels whose bytes differ between the two images.
inline usize CountDifferingPixels(const Pixels& first, const Pixels& second) noexcept
{
    usize count = 0;
    for (usize pixel = 0; pixel < TARGET_BYTES; pixel += CHANNELS)
    {
        bool isDifferent = false;
        for (usize channel = 0; channel < CHANNELS; ++channel)
        {
            isDifferent = isDifferent || first[pixel + channel] != second[pixel + channel];
        }
        count += isDifferent == true ? 1 : 0;
    }
    return count;
}

/// FNV-1a hash of the image bytes, the pixel hash both runs print for the record.
inline uint64 HashPixels(const Pixels& pixels) noexcept
{
    uint64 hash = FNV_OFFSET_BASIS;
    for (usize i = 0; i < pixels.GetSize(); ++i)
    {
        hash = (hash ^ pixels[i]) * FNV_PRIME;
    }
    return hash;
}

/// Owned resources of one draw; Destroy releases whatever was created.
struct CompositeResources final
{
    rhi::TextureHandle Image{};
    rhi::TextureViewHandle ImageView{};
    rhi::TextureHandle Target{};
    rhi::SamplerHandle Sampler{};
    rhi::RasterShaderHandle Vertex{};
    rhi::RasterShaderHandle Fragment{};
    rhi::BindingLayoutHandle Layout{};
    rhi::BindingSetHandle Bindings{};
    rhi::RasterPipelineHandle Pipeline{};
    rhi::BufferHandle Vertices{};
    rhi::BufferHandle Indices{};

    void Destroy(rhi::DeviceHandle device) noexcept
    {
        static_cast<void>(rhi::Destroy(device, Pipeline));
        static_cast<void>(rhi::Destroy(device, Bindings));
        static_cast<void>(rhi::Destroy(device, Layout));
        static_cast<void>(rhi::Destroy(device, Fragment));
        static_cast<void>(rhi::Destroy(device, Vertex));
        static_cast<void>(rhi::Destroy(device, Sampler));
        static_cast<void>(rhi::Destroy(device, ImageView));
        static_cast<void>(rhi::Destroy(device, Image));
        static_cast<void>(rhi::Destroy(device, Target));
        static_cast<void>(rhi::Destroy(device, Vertices));
        static_cast<void>(rhi::Destroy(device, Indices));
    }
};

/// Poll `handle` until it leaves Pending; true when it became Ready.
template <typename Handle>
bool AwaitReady(rhi::DeviceHandle device, Handle handle) noexcept
{
    const uint64 started = time::NowTicks();
    rhi::RasterStatus status = rhi::GetStatus(device, handle);
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - started < WAIT_NANOSECONDS)
    {
        threading::YieldThread();
        status = rhi::GetStatus(device, handle);
    }
    return status == rhi::RasterStatus::Ready;
}

/// Whether a creation call was accepted (Ready or Pending) and its object then became Ready.
template <typename Handle>
bool IsCreated(rhi::DeviceHandle device, rhi::RasterStatus status, Handle handle) noexcept
{
    return (status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending) &&
           AwaitReady(device, handle) == true;
}

/// @brief Draw the fixture once with `stages` and `filter` and read the target back.
///
/// The pass draws into an owned Rgba8Unorm attachment, so the surface's frame size does not matter.
/// @pre `surface` has a frame target (the extent rhi::CreateDevice gave it, or rhi::SetFrameTarget)
/// and no open frame.
/// @param device Ready portable-raster device; `surface` is its surface.
/// @param stages Composite stages and the slots and location they read.
/// @param filter Sampler filtering; Nearest is the fixture's defined sampler.
/// @param outPixels Receives the target in top-left row order; unchanged on failure.
/// @param outFailure Receives the failed step on failure; unchanged on success.
/// @return Whether the draw and readback completed.
inline bool RenderComposite(rhi::DeviceHandle device,
                            rhi::SurfaceHandle surface,
                            const CompositeStages& stages,
                            rhi::RasterFilter filter,
                            Pixels& outPixels,
                            const char*& outFailure) noexcept
{
    CompositeResources resources;
    const auto fail = [&](const char* step) {
        resources.Destroy(device);
        outFailure = step;
        return false;
    };
    const StaticArray<float32, QUAD_VERTEX_COUNT * QUAD_COMPONENTS> vertices = MakeQuadVertices();
    if (IsCreated(device,
                  rhi::CreateTexture(device,
                                     {IMAGE_WIDTH, IMAGE_HEIGHT, rhi::RasterFormat::Rgba8Unorm},
                                     {{IMAGE_TEXELS.GetData(), IMAGE_TEXELS.GetSize()}, usize{IMAGE_WIDTH} * CHANNELS},
                                     resources.Image),
                  resources.Image) == false)
    {
        return fail("create the source image");
    }
    if (IsCreated(device, rhi::CreateTextureView(device, resources.Image, resources.ImageView), resources.ImageView) ==
        false)
    {
        return fail("create the source image view");
    }
    if (IsCreated(device,
                  rhi::CreateTexture(device,
                                     {TARGET_EXTENT, TARGET_EXTENT, rhi::RasterFormat::Rgba8Unorm, true},
                                     {},
                                     resources.Target),
                  resources.Target) == false)
    {
        return fail("create the render target");
    }
    rhi::SamplerDescription sampler;
    sampler.Filter = filter;
    if (IsCreated(device, rhi::CreateSampler(device, sampler, resources.Sampler), resources.Sampler) == false)
    {
        return fail("create the sampler");
    }
    if (IsCreated(device, rhi::CreateRasterShader(device, stages.Vertex, resources.Vertex), resources.Vertex) ==
            false ||
        IsCreated(device, rhi::CreateRasterShader(device, stages.Fragment, resources.Fragment), resources.Fragment) ==
            false)
    {
        return fail("create the composite stages");
    }
    const StaticArray<rhi::RasterBinding, 2> layout{{
        {
            .Binding = stages.ImageSlot,
            .Kind = rhi::RasterBindingKind::Texture2D,
            .Visibility = rhi::RasterVisibility::Fragment,
        },
        {
            .Binding = stages.SamplerSlot,
            .Kind = rhi::RasterBindingKind::Sampler,
            .Visibility = rhi::RasterVisibility::Fragment,
        },
    }};
    if (IsCreated(device,
                  rhi::CreateBindingLayout(device, {layout.GetData(), layout.GetSize()}, resources.Layout),
                  resources.Layout) == false)
    {
        return fail("create the binding layout");
    }
    const StaticArray<rhi::RasterBindingResource, 2> values{{
        { .Binding = stages.ImageSlot, .Texture = resources.ImageView },
        { .Binding = stages.SamplerSlot, .Sampler = resources.Sampler },
    }};
    if (IsCreated(
            device,
            rhi::CreateBindingSet(device, resources.Layout, {values.GetData(), values.GetSize()}, resources.Bindings),
            resources.Bindings) == false)
    {
        return fail("create the binding set");
    }
    if (IsCreated(device,
                  rhi::CreateBuffer(
                      device,
                      {rhi::BufferRole::Vertex, vertices.GetSize() * sizeof(float32)},
                      {reinterpret_cast<const uint8*>(vertices.GetData()), vertices.GetSize() * sizeof(float32)},
                      resources.Vertices),
                  resources.Vertices) == false ||
        IsCreated(device,
                  rhi::CreateBuffer(
                      device,
                      {rhi::BufferRole::Index32, QUAD_INDICES.GetSize() * sizeof(uint32)},
                      {reinterpret_cast<const uint8*>(QUAD_INDICES.GetData()), QUAD_INDICES.GetSize() * sizeof(uint32)},
                      resources.Indices),
                  resources.Indices) == false)
    {
        return fail("create the quad buffers");
    }
    const StaticArray<rhi::RasterVertexStream, 1> streams{{{QUAD_COMPONENTS * sizeof(float32), false}}};
    const StaticArray<rhi::RasterVertexAttribute, 1> attributes{
        {{stages.PositionUvLocation, 0, 0, rhi::RasterVertexFormat::Float4}}};
    rhi::RasterPipelineDescription pipeline;
    pipeline.Vertex = resources.Vertex;
    pipeline.Fragment = resources.Fragment;
    pipeline.Layout = resources.Layout;
    pipeline.Streams = {streams.GetData(), streams.GetSize()};
    pipeline.Attributes = {attributes.GetData(), attributes.GetSize()};
    pipeline.Target = rhi::RasterTarget::Rgba8Unorm;
    if (IsCreated(device, rhi::CreateRasterPipeline(device, pipeline, resources.Pipeline), resources.Pipeline) == false)
    {
        return fail("create the composite pipeline");
    }
    if (rhi::BeginFrame(device, surface) != rhi::DeviceStatus::Ready)
    {
        return fail("begin the frame");
    }
    rhi::RasterPassDescription pass;
    pass.Color = resources.Target;
    for (usize channel = 0; channel < CHANNELS; ++channel)
    {
        pass.Clear[channel] = static_cast<float32>(CLEAR_BYTES[channel]) / UNORM8_MAXIMUM;
    }
    const StaticArray<rhi::RasterVertexSlice, 1> slices{
        {{resources.Vertices, 0, vertices.GetSize() * sizeof(float32)}}};
    rhi::RasterDraw draw;
    draw.Pipeline = resources.Pipeline;
    draw.Bindings = resources.Bindings;
    draw.Vertices = {slices.GetData(), slices.GetSize()};
    draw.Indices = resources.Indices;
    draw.IndexCount = QUAD_INDEX_COUNT;
    draw.VertexCount = QUAD_VERTEX_COUNT;
    rhi::SubmissionToken completion;
    const bool isRecorded = rhi::BeginRasterPass(device, pass) == rhi::RasterStatus::Ready &&
                            rhi::DrawIndexed(device, draw) == rhi::RasterStatus::Ready;
    // A frame that began must end, even after a failed record.
    const bool isSubmitted = rhi::EndFrame(device, completion) == rhi::RasterStatus::Ready;
    if (isRecorded == false || isSubmitted == false)
    {
        return fail("record and submit the composite frame");
    }
    rhi::ReadbackTicket readback;
    const rhi::TextureRegion region{0, 0, 0, TARGET_EXTENT, TARGET_EXTENT};
    const rhi::RasterStatus requested =
        rhi::RequestTextureReadback(device, resources.Target, region, usize{TARGET_EXTENT} * CHANNELS, readback);
    if ((requested != rhi::RasterStatus::Pending && requested != rhi::RasterStatus::Ready) ||
        AwaitReady(device, completion) == false || AwaitReady(device, readback) == false)
    {
        static_cast<void>(rhi::Release(device, readback));
        return fail("read the render target back");
    }
    Pixels pixels{};
    const bool isCopied =
        rhi::CopyReadback(device, readback, pixels.GetData(), pixels.GetSize()) == rhi::RasterStatus::Ready;
    static_cast<void>(rhi::Release(device, readback));
    if (isCopied == false)
    {
        return fail("copy the render target pixels");
    }
    resources.Destroy(device);
    outPixels = pixels;
    return true;
}
} // namespace ludus::qa::composite_recipe
