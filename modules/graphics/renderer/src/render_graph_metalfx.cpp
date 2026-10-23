#include "internal/render_graph_metalfx.h"

namespace ludus::graphics::renderer::internal
{
RenderGraphMetalFxPass::~RenderGraphMetalFxPass() noexcept
{
    static_cast<void>(rhi::Destroy(mDevice, mUpscaler));
    // The copy's view retains the intermediate until its own destruction.
    static_cast<void>(rhi::Destroy(mDevice, mIntermediate));
}
rhi::RasterStatus RenderGraphMetalFxPass::Initialize(rhi::DeviceHandle device,
                                                     const rhi::SpatialUpscalerDescription& description,
                                                     rhi::TextureHandle source,
                                                     rhi::TextureHandle destination,
                                                     RenderGraphUpscaleProgram* surfaceProgramOrNull) noexcept
{
    mDevice = device;
    mSource = source;
    mDestination = destination;
    mSurfaceProgramOrNull = surfaceProgramOrNull;
    const rhi::RasterStatus created = rhi::CreateSpatialUpscaler(device, description, mUpscaler);
    if (created != rhi::RasterStatus::Ready)
    {
        return created;
    }
    if (surfaceProgramOrNull != nullptr)
    {
        rhi::TextureDescription texture;
        texture.Width = description.OutputWidth;
        texture.Height = description.OutputHeight;
        texture.Attachment = true;
        return rhi::CreateTexture(device, texture, {}, mIntermediate);
    }
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus RenderGraphMetalFxPass::Poll() noexcept
{
    const rhi::RasterStatus status = rhi::GetStatus(mDevice, mUpscaler);
    if (status != rhi::RasterStatus::Ready || mSurfaceProgramOrNull == nullptr)
    {
        return status;
    }
    const rhi::RasterStatus textureStatus = rhi::GetStatus(mDevice, mIntermediate);
    if (textureStatus != rhi::RasterStatus::Ready)
    {
        return textureStatus;
    }
    if (mHasCopy == false)
    {
        const rhi::RasterStatus created = mCopy.Initialize(*mSurfaceProgramOrNull, mIntermediate);
        if (created != rhi::RasterStatus::Ready && created != rhi::RasterStatus::Pending)
        {
            return created;
        }
        mHasCopy = true;
    }
    return mCopy.Poll();
}
rhi::RasterStatus RenderGraphMetalFxPass::Draw() noexcept
{
    const rhi::RasterStatus status =
        rhi::EncodeSpatialUpscale(mDevice,
                                  mUpscaler,
                                  mSource,
                                  mSurfaceProgramOrNull != nullptr ? mIntermediate : mDestination);
    return status == rhi::RasterStatus::Ready && mSurfaceProgramOrNull != nullptr ? mCopy.Draw({}) : status;
}
} // namespace ludus::graphics::renderer::internal
