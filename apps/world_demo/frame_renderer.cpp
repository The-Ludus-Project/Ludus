#include "internal/frame_renderer.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include <ludus/graphics/rhi/graph.h>
namespace ludus::world_demo
{
namespace
{
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
} // namespace

rhi::RasterStatus FrameRenderer::Prepare(rhi::DeviceHandle device) noexcept
{
    switch (mStage)
    {
        case FrameRendererStage::Idle: {
            const rhi::RasterStatus status =
                mRenderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment());
            if (IsAccepted(status) == false)
            {
                return status;
            }
            mStage = FrameRendererStage::Flat;
            return rhi::RasterStatus::Pending;
        }
        case FrameRendererStage::Flat: {
            rhi::RasterStatus status = mRenderer.Poll();
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            // Reverse-Z views need the L1 profile; Submit's conventional L0 path cannot draw them.
            status = mRenderer.InitializeViews(shaders::renderer_composite::Vertex(),
                                               shaders::renderer_composite::Fragment());
            if (IsAccepted(status) == false)
            {
                return status;
            }
            mStage = FrameRendererStage::Views;
            return rhi::RasterStatus::Pending;
        }
        case FrameRendererStage::Views: {
            rhi::RasterStatus status = mRenderer.PollViews();
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            status = mRenderer.CreateMesh(GetUnitQuadMesh(), mQuad);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            mStage = FrameRendererStage::Mesh;
            return rhi::RasterStatus::Pending;
        }
        case FrameRendererStage::Mesh: {
            const rhi::RasterStatus status = mRenderer.GetStatus(mQuad);
            if (status == rhi::RasterStatus::Ready)
            {
                mStage = FrameRendererStage::Ready;
            }
            return status;
        }
        case FrameRendererStage::Ready:
            return rhi::RasterStatus::Ready;
    }
    return rhi::RasterStatus::InvalidState;
}

rhi::RasterStatus FrameRenderer::promoteCandidate() noexcept
{
    const rhi::RasterStatus status = mRenderer.GetStatus(mCandidate);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    if (mHasView == true)
    {
        // Submitted GPU work keeps its own retention; releasing the CPU view is safe between frames.
        const rhi::RasterStatus released = mRenderer.Release(mView);
        if (released != rhi::RasterStatus::Ready)
        {
            return released;
        }
    }
    mView = mCandidate;
    mCandidate = {};
    mHasView = true;
    mHasCandidate = false;
    return rhi::RasterStatus::Ready;
}

rhi::RasterStatus FrameRenderer::Update(const RenderFrame& frame, uint32 width, uint32 height) noexcept
{
    if (mStage != FrameRendererStage::Ready)
    {
        return rhi::RasterStatus::NotReady;
    }
    if (mHasCandidate == true)
    {
        const rhi::RasterStatus promoted = promoteCandidate();
        if (promoted == rhi::RasterStatus::Pending)
        {
            // Keep drawing the last Ready view; this frame's extraction is superseded by the next.
            return rhi::RasterStatus::NotReady;
        }
        if (promoted != rhi::RasterStatus::Ready)
        {
            return promoted;
        }
    }
    rr::ViewDescription view;
    rhi::RasterStatus status = TryBuildFrameView(frame, width, height, view);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    usize count = 0;
    status = TryBuildFrameItems(frame, mQuad, mItems, count);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    status = mHasSnapshot == true ? mRenderer.ReplaceSnapshot(mItems.GetData(), count, mSnapshot)
                                  : mRenderer.CreateSnapshot(mItems.GetData(), count, mSnapshot);
    if (status == rhi::RasterStatus::CapacityExceeded)
    {
        // Retained views still hold every snapshot slot; the next frame retries.
        return rhi::RasterStatus::NotReady;
    }
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    mHasSnapshot = true;
    rr::ViewReport report;
    status = mRenderer.PrepareView(mSnapshot, view, mCandidate, report);
    if (status == rhi::RasterStatus::CapacityExceeded)
    {
        return rhi::RasterStatus::NotReady;
    }
    if (IsAccepted(status) == false)
    {
        return status;
    }
    mHasCandidate = true;
    // Native immutable uploads are usually Ready at once, so the frame draws without a delay.
    status = promoteCandidate();
    return status == rhi::RasterStatus::Pending ? rhi::RasterStatus::Ready : status;
}

rhi::RasterStatus FrameRenderer::Draw(rhi::TextureHandle target) noexcept
{
    if (mHasView == false)
    {
        return rhi::RasterStatus::NotReady;
    }
    rhi::RasterPassDescription pass;
    pass.Color = target;
    for (usize channel = 0; channel < RGBA_CHANNELS; ++channel)
    {
        pass.Clear[channel] = FRAME_BACKGROUND[channel];
    }
    // Reverse-Z clears depth to zero; the nearest (latest) draw has the greatest depth.
    pass.ClearDepth = 0;
    return mRenderer.DrawView(mView, pass);
}

void FrameRenderer::Reset() noexcept
{
    // Renderer::Reset releases every renderer handle at once.
    mRenderer.Reset();
    mQuad = {};
    mSnapshot = {};
    mView = {};
    mCandidate = {};
    mStage = FrameRendererStage::Idle;
    mHasSnapshot = false;
    mHasView = false;
    mHasCandidate = false;
}
} // namespace ludus::world_demo
