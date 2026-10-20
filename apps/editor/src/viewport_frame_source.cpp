#include "internal/viewport_frame_source.h"

#include <ludus/foundation/base/core.h>

namespace ludus::editor
{
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
ViewportFrameSource::ViewportFrameSource(QObject* parentOrNull) : QObject(parentOrNull) {}
ViewportFrameSource::~ViewportFrameSource() = default;
rhi::RasterStatus ViewportFrameSource::TakeFrameRead(rhi::NativeDeviceLease device,
                                                     const rhi::NativeDeviceIdentity& consumer,
                                                     ViewportFrameRead& outRead) noexcept
{
    ViewportFrameRead candidate;
    rr::ViewportImage image;
    if (TakeFrame(candidate.Lease, image) == false)
    {
        return rhi::RasterStatus::NotReady;
    }
    const rhi::RasterStatus acquired =
        rhi::AcquireNativeTextureRead(device, consumer, image.Texture, image.Producer, candidate.Read, candidate.Image);
    if (acquired != rhi::RasterStatus::Ready)
    {
        ReleaseFrame(candidate.Lease);
        return acquired;
    }
    outRead = candidate;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus ViewportFrameSource::EndFrameRead(ViewportFrameRead& inoutRead) noexcept
{
    // Queues a completion marker behind Qt's submitted sampling; Ready once it has passed.
    const rhi::RasterStatus released = rhi::ReleaseNativeTextureRead(inoutRead.Read);
    if (released == rhi::RasterStatus::Ready)
    {
        ReleaseFrame(inoutRead.Lease);
        inoutRead = {};
    }
    return released;
}
void ViewportFrameSource::DrainFrameRead(ViewportFrameRead& inoutRead) noexcept
{
    const rhi::RasterStatus released = rhi::DrainNativeTextureRead(inoutRead.Read);
    LUDUS_REQUIRE(released == rhi::RasterStatus::Ready);
    ReleaseFrame(inoutRead.Lease);
    inoutRead = {};
}
} // namespace ludus::editor
