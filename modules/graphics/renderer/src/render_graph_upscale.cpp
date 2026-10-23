#include "internal/render_graph_upscale.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/graph.h>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
using Status = rhi::RasterStatus;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize VERTEX_COMPONENT_COUNT = 4;
constexpr usize VERTEX_BYTES = QUAD_VERTEX_COUNT * VERTEX_COMPONENT_COUNT * sizeof(float32);
constexpr usize INDEX_BYTES = QUAD_INDEX_COUNT * sizeof(uint32);
constexpr usize BINDING_COUNT = 2;
constexpr usize VERTEX_STREAM_COUNT = 1;
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
} // namespace

RenderGraphUpscaleProgram::~RenderGraphUpscaleProgram() noexcept
{
    static_cast<void>(rhi::Destroy(mDevice, mPipeline));
    static_cast<void>(rhi::Destroy(mDevice, mLayout));
    static_cast<void>(rhi::Destroy(mDevice, mSampler));
    static_cast<void>(rhi::Destroy(mDevice, mVertices));
    static_cast<void>(rhi::Destroy(mDevice, mIndices));
    static_cast<void>(rhi::Destroy(mDevice, mVertex));
    static_cast<void>(rhi::Destroy(mDevice, mFragment));
}

Status RenderGraphUpscaleProgram::Initialize(rhi::DeviceHandle device,
                                             const rhi::RasterShaderDescription& vertex,
                                             const rhi::RasterShaderDescription& fragment,
                                             bool isSurface) noexcept
{
    if (mPhase != Phase::Empty || vertex.Inputs.size() != 1 || vertex.Inputs[0].Location != 0 ||
        vertex.Inputs[0].Format != rhi::RasterVertexFormat::Float4 || vertex.Bindings.empty() == false ||
        fragment.Bindings.size() != BINDING_COUNT || fragment.Bindings[0].Binding != 0 ||
        fragment.Bindings[0].Kind != rhi::RasterBindingKind::Texture2D || fragment.Bindings[1].Binding != 1 ||
        fragment.Bindings[1].Kind != rhi::RasterBindingKind::Sampler)
    {
        return Status::InvalidDescription;
    }
    mDevice = device;
    mIsSurface = isSurface;
    Status status = rhi::CreateRasterShader(device, vertex, mVertex);
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, fragment, mFragment);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBindingLayout(device, fragment.Bindings, mLayout);
    }
    rhi::SamplerDescription sampler;
    sampler.Filter = rhi::RasterFilter::Linear;
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateSampler(device, sampler, mSampler);
    }
    // Reuse the installed renderer_composite recipe's canonical top-left UV mapping.
    const StaticArray<math::Vector4, QUAD_VERTEX_COUNT> vertices{
        {{-1, 1, 0, 0}, {1, 1, 1, 0}, {1, -1, 1, 1}, {-1, -1, 0, 1}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> indices{{0, 1, 2, 2, 3, 0}};
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Vertex, VERTEX_BYTES},
                                   {reinterpret_cast<const uint8*>(vertices.GetData()), VERTEX_BYTES},
                                   mVertices);
    }
    if (IsAccepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Index32, INDEX_BYTES},
                                   {reinterpret_cast<const uint8*>(indices.GetData()), INDEX_BYTES},
                                   mIndices);
    }
    mPhase = IsAccepted(status) == true ? Phase::Dependencies : Phase::Failed;
    mStatus = status;
    return IsAccepted(status) == true ? Poll() : status;
}

Status RenderGraphUpscaleProgram::Poll() noexcept
{
    if (mPhase == Phase::Empty || mPhase == Phase::Ready || mPhase == Phase::Failed)
    {
        return mStatus;
    }
    Status status = Status::Ready;
    if (mPhase == Phase::Dependencies)
    {
        status = rhi::GetStatus(mDevice, mVertex);
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(mDevice, mFragment);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(mDevice, mLayout);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(mDevice, mSampler);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(mDevice, mVertices);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(mDevice, mIndices);
        }
        if (status == Status::Ready)
        {
            mPhase = Phase::PipelineCreation;
        }
    }
    if (status == Status::Ready && mPhase == Phase::PipelineCreation)
    {
        const StaticArray<rhi::RasterVertexStream, VERTEX_STREAM_COUNT> streams{
            {{VERTEX_COMPONENT_COUNT * sizeof(float32), false}}};
        const StaticArray<rhi::RasterVertexAttribute, VERTEX_STREAM_COUNT> attributes{
            {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
        rhi::RasterPipelineDescription description{mVertex, mFragment, mLayout, streams.AsSpan(), attributes.AsSpan()};
        description.Target = mIsSurface == true ? rhi::RasterTarget::Surface : rhi::RasterTarget::Rgba8Unorm;
        status = rhi::CreateRasterPipeline(mDevice, description, mPipeline);
        if (IsAccepted(status) == true)
        {
            mPhase = Phase::Pipeline;
        }
    }
    if (status == Status::Ready && mPhase == Phase::Pipeline)
    {
        status = rhi::GetStatus(mDevice, mPipeline);
        if (status == Status::Ready)
        {
            mPhase = Phase::Ready;
        }
    }
    if (IsAccepted(status) == false)
    {
        mPhase = Phase::Failed;
    }
    mStatus = status;
    return status;
}

Status RenderGraphUpscaleProgram::Draw(rhi::BindingSetHandle bindings, rhi::TextureHandle destination) noexcept
{
    if (mPhase != Phase::Ready)
    {
        return mStatus == Status::Pending ? Status::NotReady : mStatus;
    }
    rhi::RasterPassDescription pass;
    pass.Color = destination;
    pass.DepthLoad = rhi::RasterLoad::Discard;
    pass.DepthStore = rhi::RasterStore::Discard;
    const Status status = rhi::BeginRasterPass(mDevice, pass);
    if (status != Status::Ready)
    {
        return status;
    }
    const StaticArray<rhi::RasterVertexSlice, VERTEX_STREAM_COUNT> vertices{{{mVertices, 0, VERTEX_BYTES}}};
    return rhi::DrawIndexed(
        mDevice,
        {mPipeline, bindings, vertices.AsSpan(), mIndices, 0, QUAD_INDEX_COUNT, QUAD_VERTEX_COUNT, 1});
}
RenderGraphUpscalePass::~RenderGraphUpscalePass() noexcept
{
    if (mProgramOrNull != nullptr)
    {
        static_cast<void>(rhi::Destroy(mProgramOrNull->mDevice, mBindings));
        static_cast<void>(rhi::Destroy(mProgramOrNull->mDevice, mSource));
    }
}
Status RenderGraphUpscalePass::Initialize(RenderGraphUpscaleProgram& inoutProgram, rhi::TextureHandle source) noexcept
{
    if (mProgramOrNull != nullptr || inoutProgram.mPhase != RenderGraphUpscaleProgram::Phase::Ready)
    {
        return Status::InvalidState;
    }
    mProgramOrNull = &inoutProgram;
    mStatus = rhi::CreateTextureView(inoutProgram.mDevice, source, mSource);
    return IsAccepted(mStatus) == true ? Poll() : mStatus;
}
Status RenderGraphUpscalePass::Poll() noexcept
{
    if (mProgramOrNull == nullptr || IsAccepted(mStatus) == false)
    {
        return mStatus;
    }
    Status status = rhi::GetStatus(mProgramOrNull->mDevice, mSource);
    if (status == Status::Ready && mHasBindings == false)
    {
        const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{
            {{ .Binding = 0, .Texture = mSource }, { .Binding = 1, .Sampler = mProgramOrNull->mSampler }}};
        status = rhi::CreateBindingSet(mProgramOrNull->mDevice, mProgramOrNull->mLayout, resources.AsSpan(), mBindings);
        mHasBindings = IsAccepted(status);
    }
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(mProgramOrNull->mDevice, mBindings);
    }
    mStatus = status;
    return status;
}
Status RenderGraphUpscalePass::Draw(rhi::TextureHandle destination) noexcept
{
    if (mStatus != Status::Ready || mProgramOrNull == nullptr)
    {
        return mStatus == Status::Pending ? Status::NotReady : mStatus;
    }
    return mProgramOrNull->Draw(mBindings, destination);
}
} // namespace ludus::graphics::renderer::internal
