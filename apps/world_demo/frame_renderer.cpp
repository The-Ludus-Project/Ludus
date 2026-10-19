#include "internal/frame_renderer.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_output.h"
#include "renderer_shadow.h"
namespace ludus::world_demo
{
namespace
{
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
/// Manual exposure of the display transform, the same as the Editor's World viewport.
constexpr float32 DISPLAY_EXPOSURE = 1;
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
            mDevice = device;
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
            // RasterViewport tone-maps and encodes through the lighting output profile.
            status = mRenderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                                  shaders::renderer_output::Fragment(),
                                                  shaders::renderer_shadow::Fragment());
            if (IsAccepted(status) == false)
            {
                return status;
            }
            mStage = FrameRendererStage::Lighting;
            return rhi::RasterStatus::Pending;
        }
        case FrameRendererStage::Lighting: {
            rhi::RasterStatus status = mRenderer.PollLighting();
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

rhi::RasterStatus FrameRenderer::prepareViewport(uint32 width, uint32 height) noexcept
{
    if (mViewportWidth == width && mViewportHeight == height)
    {
        return rhi::RasterStatus::Ready;
    }
    // Accepted GPU work retires through the RHI; the old targets cannot match the new surface.
    mViewport.Reset();
    mViewportWidth = 0;
    mViewportHeight = 0;
    // Flat colours need no HDR target, and Linear8 is what the Editor uses for unlit scenes.
    const rhi::RasterStatus status =
        mViewport.Initialize(mRenderer, mDevice, width, height, DISPLAY_EXPOSURE, rr::ViewportColorMode::Linear8, true);
    if (IsAccepted(status) == false)
    {
        return status;
    }
    mViewportWidth = width;
    mViewportHeight = height;
    return rhi::RasterStatus::Ready;
}

rhi::RasterStatus FrameRenderer::Update(const RenderFrame& frame, uint32 width, uint32 height) noexcept
{
    if (mStage != FrameRendererStage::Ready || width == 0 || height == 0)
    {
        return rhi::RasterStatus::NotReady;
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
    status = prepareViewport(width, height);
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
    status = mViewport.Update(mSnapshot, view, report);
    if (status == rhi::RasterStatus::CapacityExceeded)
    {
        return rhi::RasterStatus::NotReady;
    }
    return IsAccepted(status) == true ? rhi::RasterStatus::Ready : status;
}

rhi::RasterStatus FrameRenderer::Present(rhi::SurfaceHandle surface, rhi::SubmissionToken& outCompletion) noexcept
{
    if (mStage != FrameRendererStage::Ready || mViewportWidth == 0)
    {
        return rhi::RasterStatus::NotReady;
    }
    rhi::SubmissionToken completion;
    const rhi::RasterStatus status = mViewport.Render(surface, completion);
    if (completion.IsNull() == false)
    {
        outCompletion = completion;
    }
    // A viewport without a drawable view yet reports the pending setup as InvalidHandle.
    return status == rhi::RasterStatus::Pending || status == rhi::RasterStatus::InvalidHandle
               ? rhi::RasterStatus::NotReady
               : status;
}

rhi::RasterStatus FrameRenderer::Acquire(rr::ViewportFrame& outFrame, rr::ViewportImage& outImage) noexcept
{
    return mViewport.Acquire(outFrame, outImage);
}

rhi::RasterStatus FrameRenderer::Release(rr::ViewportFrame& inoutFrame) noexcept
{
    return mViewport.Release(inoutFrame);
}

void FrameRenderer::Reset() noexcept
{
    // RasterViewport::Reset must precede Renderer::Reset, which releases every renderer handle.
    mViewport.Reset();
    mRenderer.Reset();
    mQuad = {};
    mSnapshot = {};
    mDevice = {};
    mStage = FrameRendererStage::Idle;
    mViewportWidth = 0;
    mViewportHeight = 0;
    mHasSnapshot = false;
}
} // namespace ludus::world_demo
