#include "internal/render_graph_upscale.h"

#include "internal/render_graph_program_cache.h"

#include <ludus/foundation/containers/static_array.hpp>
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
constexpr usize BINDING_COUNT = 2;
constexpr usize VERTEX_STREAM_COUNT = 1;
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
} // namespace

RenderGraphUpscaleProgram::~RenderGraphUpscaleProgram() noexcept
{
    Reset();
}

void RenderGraphUpscaleProgram::Reset() noexcept
{
    if (mHasRequest == true)
    {
        // Accepted batches retain the borrowed pipeline beyond this release.
        static_cast<void>(rhi::Release(mDevice, mRequest));
        mHasRequest = false;
    }
    mRequest = {};
    mPipeline = {};
    if (mCacheOrNull != nullptr)
    {
        RenderGraphProgramCacheAccess::Release(*mCacheOrNull, mDependenciesOrNull);
    }
    mCacheOrNull = nullptr;
    mDependenciesOrNull = nullptr;
    mDevice = {};
    mPhase = Phase::Empty;
    mStatus = Status::InvalidState;
    mIsSurface = false;
}

Status RenderGraphUpscaleProgram::Initialize(RenderGraphProgramCache& inoutCache,
                                             const rhi::RasterShaderDescription& vertex,
                                             const rhi::RasterShaderDescription& fragment,
                                             bool isSurface) noexcept
{
    if (mPhase != Phase::Empty)
    {
        return Status::InvalidState;
    }
    if (vertex.Inputs.GetSize() != 1 || vertex.Inputs[0].Location != 0 ||
        vertex.Inputs[0].Format != rhi::RasterVertexFormat::Float4 || vertex.Bindings.IsEmpty() == false ||
        fragment.Bindings.GetSize() != BINDING_COUNT || fragment.Bindings[0].Binding != 0 ||
        fragment.Bindings[0].Kind != rhi::RasterBindingKind::Texture2D || fragment.Bindings[1].Binding != 1 ||
        fragment.Bindings[1].Kind != rhi::RasterBindingKind::Sampler)
    {
        return Status::InvalidDescription;
    }
    const rhi::DeviceHandle device = RenderGraphProgramCacheAccess::GetDevice(inoutCache);
    if (RenderGraphProgramCacheAccess::IsUsable(inoutCache, device) == false)
    {
        return Status::InvalidDescription;
    }
    RenderGraphUpscaleDependencies* dependenciesOrNull = nullptr;
    const Status status =
        RenderGraphProgramCacheAccess::AcquireUpscale(inoutCache, vertex, fragment, dependenciesOrNull);
    if (IsAccepted(status) == false)
    {
        return status;
    }
    mDevice = device;
    mCacheOrNull = &inoutCache;
    mDependenciesOrNull = dependenciesOrNull;
    mIsSurface = isSurface;
    mPhase = Phase::Dependencies;
    mStatus = status;
    return Poll();
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
        status = RenderGraphProgramCacheAccess::Poll(*mDependenciesOrNull);
        if (status == Status::Ready)
        {
            mPhase = Phase::PipelineRequest;
        }
    }
    if (status == Status::Ready && mPhase == Phase::PipelineRequest)
    {
        const StaticArray<rhi::RasterVertexStream, VERTEX_STREAM_COUNT> streams{
            {{VERTEX_COMPONENT_COUNT * sizeof(float32), false}}};
        const StaticArray<rhi::RasterVertexAttribute, VERTEX_STREAM_COUNT> attributes{
            {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
        rhi::RasterPipelineDescription description{mDependenciesOrNull->Vertex,
                                                   mDependenciesOrNull->Fragment,
                                                   mDependenciesOrNull->Layout,
                                                   {streams.GetData(), streams.GetSize()},
                                                   {attributes.GetData(), attributes.GetSize()}};
        description.Target = mIsSurface == true ? rhi::RasterTarget::Surface : rhi::RasterTarget::Rgba8Unorm;
        // Equal shader/layout incarnations and fixed state join one native pipeline in the RHI.
        status = rhi::RequestPipeline(mDevice, description, mRequest);
        if (IsAccepted(status) == true)
        {
            mHasRequest = true;
            mPhase = Phase::Pipeline;
            status = Status::Ready;
        }
    }
    if (status == Status::Ready && mPhase == Phase::Pipeline)
    {
        status = rhi::GetStatus(mDevice, mRequest);
        if (status == Status::Ready)
        {
            status = rhi::GetRequestedPipeline(mDevice, mRequest, mPipeline);
        }
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
    const StaticArray<rhi::RasterVertexSlice, VERTEX_STREAM_COUNT> vertices{
        {{mDependenciesOrNull->Vertices, 0, VERTEX_BYTES}}};
    return rhi::DrawIndexed(mDevice,
                            {mPipeline,
                             bindings,
                             {vertices.GetData(), vertices.GetSize()},
                             mDependenciesOrNull->Indices,
                             0,
                             QUAD_INDEX_COUNT,
                             QUAD_VERTEX_COUNT,
                             1});
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
        const RenderGraphUpscaleDependencies& dependencies = *mProgramOrNull->mDependenciesOrNull;
        const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{
            {{ .Binding = 0, .Texture = mSource }, { .Binding = 1, .Sampler = dependencies.Sampler }}};
        status = rhi::CreateBindingSet(mProgramOrNull->mDevice,
                                       dependencies.Layout,
                                       {resources.GetData(), resources.GetSize()},
                                       mBindings);
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
