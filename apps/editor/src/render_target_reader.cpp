#include "internal/render_target_reader.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/graphics/rhi/graph.h>

#include "editor_texture_depth_pack.h"

#include <cstring>

namespace ludus::editor
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace shaders = ludus::shaders;
namespace
{
/// One fullscreen triangle: clip positions (-1,-1), (3,-1) and (-1,3) cover the attachment.
constexpr usize VERTEX_COUNT = 3;
constexpr usize STREAM_COUNT = 1;
constexpr usize ATTRIBUTE_COUNT = 1;
constexpr float32 FULLSCREEN_EXTENT = 3.0F;
constexpr usize PACKED_TEXEL_BYTES = 4;

/// Destroying a never-created (default) handle is a rejected no-op, so release needs no tracking.
template <typename Handle>
void DestroyHandle(rhi::DeviceHandle device, Handle& inoutHandle) noexcept
{
    static_cast<void>(rhi::Destroy(device, inoutHandle));
    inoutHandle = {};
}
} // namespace

RenderTargetReader::~RenderTargetReader()
{
    Release();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — width and height of one subresource.
RenderTargetReadStatus RenderTargetReader::Begin(rhi::DeviceHandle device,
                                                 rhi::SurfaceHandle surface,
                                                 rhi::TextureHandle texture,
                                                 rhi::TextureAspect aspect,
                                                 uint32 width,
                                                 uint32 height)
{
    Cancel();
    mDevice = device;
    mHasDevice = true;
    mPendingPolls = 0;
    rhi::RasterTextureState state;
    if (width == 0 || height == 0 || rhi::GetTextureState(device, texture, state) != rhi::RasterStatus::Ready)
    {
        return RenderTargetReadStatus::Failed;
    }
    const bool isDepth = aspect == rhi::TextureAspect::Depth;
    if ((isDepth == true ? state.DepthDefined : state.ColorDefined) == false)
    {
        return RenderTargetReadStatus::Undefined;
    }
    mEncoding = isDepth == true ? TexelEncoding::Depth32Float : TexelEncoding::Rgba8Unorm;
    mWidth = width;
    mHeight = height;
    rhi::TextureHandle source = texture;
    if (isDepth == true)
    {
        if (preparePacking(width, height) == false || encodePacking(surface, texture) == false)
        {
            return RenderTargetReadStatus::Failed;
        }
        source = mPacked;
    }
    if (mReader.Begin(device, source, width, height) == false)
    {
        return RenderTargetReadStatus::Failed;
    }
    mIsBusy = true;
    mIsComplete = false;
    return RenderTargetReadStatus::Pending;
}

RenderTargetReadStatus RenderTargetReader::Poll() noexcept
{
    if (mIsBusy == false)
    {
        return RenderTargetReadStatus::Idle;
    }
    if (mIsComplete == true)
    {
        return RenderTargetReadStatus::Complete;
    }
    const OffscreenReadStatus status = mReader.Poll();
    if (status == OffscreenReadStatus::Pending)
    {
        ++mPendingPolls;
        return RenderTargetReadStatus::Pending;
    }
    if (status != OffscreenReadStatus::Complete)
    {
        Cancel();
        return RenderTargetReadStatus::Failed;
    }
    LUDUS_PROFILE_SCOPE(EditorReadbackCopyTextureBytes);
    const QImage image = mReader.TakeImage();
    InspectedTexture texture;
    texture.Encoding = mEncoding;
    texture.Width = mWidth;
    texture.Height = mHeight;
    // Packed depth bytes are already the little-endian binary32 words InspectedTexture stores.
    const usize rowBytes = static_cast<usize>(mWidth) * PACKED_TEXEL_BYTES;
    texture.Bytes.resize(static_cast<qsizetype>(rowBytes * mHeight));
    for (uint32 y = 0; y < mHeight; ++y)
    {
        std::memcpy(texture.Bytes.data() + static_cast<qsizetype>(rowBytes * y),
                    image.constScanLine(static_cast<int32>(y)),
                    rowBytes);
    }
    mTexture = texture;
    mIsComplete = true;
    return RenderTargetReadStatus::Complete;
}

InspectedTexture RenderTargetReader::TakeTexture() noexcept
{
    InspectedTexture texture;
    if (mIsComplete == true)
    {
        texture = mTexture;
        mTexture = {};
    }
    mIsBusy = false;
    mIsComplete = false;
    return texture;
}

void RenderTargetReader::Cancel() noexcept
{
    mReader.Cancel();
    mTexture = {};
    mIsBusy = false;
    mIsComplete = false;
}

void RenderTargetReader::Release() noexcept
{
    Cancel();
    if (mHasDevice == false)
    {
        return;
    }
    DestroyHandle(mDevice, mPacked);
    DestroyHandle(mDevice, mPipeline);
    DestroyHandle(mDevice, mLayout);
    DestroyHandle(mDevice, mFragment);
    DestroyHandle(mDevice, mVertex);
    DestroyHandle(mDevice, mVertices);
    DestroyHandle(mDevice, mIndices);
    mPackedWidth = 0;
    mPackedHeight = 0;
    mIsPackingReady = false;
    mHasDevice = false;
    mDevice = {};
}

bool RenderTargetReader::preparePacking(uint32 width, uint32 height) noexcept
{
    if (mIsPackingReady == false)
    {
        const rhi::RasterShaderDescription vertex = shaders::editor_texture_depth_pack::Vertex();
        const rhi::RasterShaderDescription fragment = shaders::editor_texture_depth_pack::Fragment();
        if (rhi::CreateRasterShader(mDevice, vertex, mVertex) != rhi::RasterStatus::Ready ||
            rhi::CreateRasterShader(mDevice, fragment, mFragment) != rhi::RasterStatus::Ready ||
            rhi::CreateBindingLayout(mDevice, fragment.Bindings, mLayout) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        const StaticArray<rhi::RasterVertexStream, STREAM_COUNT> streams{{{sizeof(math::Vector4), false}}};
        const StaticArray<rhi::RasterVertexAttribute, ATTRIBUTE_COUNT> attributes{
            {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
        rhi::RasterPipelineDescription description;
        description.Vertex = mVertex;
        description.Fragment = mFragment;
        description.Layout = mLayout;
        description.Streams = streams.AsSpan();
        description.Attributes = attributes.AsSpan();
        // Linear RGBA8 offscreen attachment, no depth test: every texel is written once.
        description.Target = rhi::RasterTarget::Rgba8Unorm;
        const StaticArray<math::Vector4, VERTEX_COUNT> positions{
            {{-1, -1, 0, 1}, {FULLSCREEN_EXTENT, -1, 0, 1}, {-1, FULLSCREEN_EXTENT, 0, 1}}};
        const StaticArray<uint32, VERTEX_COUNT> indices{{0, 1, 2}};
        if (rhi::CreateRasterPipeline(mDevice, description, mPipeline) != rhi::RasterStatus::Ready ||
            rhi::CreateBuffer(mDevice,
                              {rhi::BufferRole::Vertex, sizeof(positions)},
                              {reinterpret_cast<const uint8*>(positions.GetData()), sizeof(positions)},
                              mVertices) != rhi::RasterStatus::Ready ||
            rhi::CreateBuffer(mDevice,
                              {rhi::BufferRole::Index32, sizeof(indices)},
                              {reinterpret_cast<const uint8*>(indices.GetData()), sizeof(indices)},
                              mIndices) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        mIsPackingReady = true;
    }
    if (mPackedWidth != width || mPackedHeight != height)
    {
        DestroyHandle(mDevice, mPacked);
        mPackedWidth = 0;
        mPackedHeight = 0;
        rhi::TextureDescription description;
        description.Width = width;
        description.Height = height;
        description.Attachment = true;
        if (rhi::CreateTexture(mDevice, description, {}, mPacked) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        mPackedWidth = width;
        mPackedHeight = height;
    }
    return true;
}

bool RenderTargetReader::encodePacking(rhi::SurfaceHandle surface, rhi::TextureHandle texture) noexcept
{
    rhi::TextureViewHandle depth;
    if (rhi::CreateTextureView(mDevice, texture, { .Aspect = rhi::TextureAspect::Depth }, depth) !=
        rhi::RasterStatus::Ready)
    {
        return false;
    }
    const rhi::RasterBindingResource resource{ .Texture = depth };
    rhi::BindingSetHandle set;
    if (rhi::CreateBindingSet(mDevice, mLayout, {&resource, 1}, set) != rhi::RasterStatus::Ready)
    {
        DestroyHandle(mDevice, depth);
        return false;
    }
    const rhi::RasterVertexSlice slice{mVertices, 0, sizeof(math::Vector4) * VERTEX_COUNT};
    rhi::RasterDraw drawing;
    drawing.Pipeline = mPipeline;
    drawing.Bindings = set;
    drawing.Vertices = {&slice, 1};
    drawing.Indices = mIndices;
    drawing.IndexCount = VERTEX_COUNT;
    drawing.VertexCount = VERTEX_COUNT;
    rhi::RasterPassDescription pass;
    pass.Color = mPacked;
    bool isEncoded = rhi::BeginFrame(mDevice, surface) == rhi::DeviceStatus::Ready;
    if (isEncoded == true)
    {
        isEncoded = rhi::BeginRasterPass(mDevice, pass) == rhi::RasterStatus::Ready &&
                    rhi::DrawIndexed(mDevice, drawing) == rhi::RasterStatus::Ready;
        // The frame always closes, even after a rejected draw; nothing waits for its completion.
        rhi::SubmissionToken completion;
        isEncoded = rhi::EndFrame(mDevice, completion) == rhi::RasterStatus::Ready && isEncoded == true;
    }
    // Submitted frames retain their bindings until GPU completion.
    DestroyHandle(mDevice, set);
    DestroyHandle(mDevice, depth);
    return isEncoded;
}
} // namespace ludus::editor
