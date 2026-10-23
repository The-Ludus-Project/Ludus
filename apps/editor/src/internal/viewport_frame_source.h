#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/viewport_frame.hpp>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/graphics/rhi/raster.h>

#include <QObject>

namespace ludus::editor
{
/// One displayed image as the viewport holds it: a native texture the Qt scene graph samples,
/// plus the source's private record of how to close that read.
///
/// The default ViewportFrameSource members fill Lease and Read (an RHI image lease and a
/// native read of it); a source that presents imported native images instead uses Token and
/// Slot to find its own record. The viewport only reads Image and passes the whole read back.
struct ViewportFrameRead final
{
    /// Native descriptor of the image to wrap; valid until the read is ended.
    graphics::rhi::NativeTextureInfo Image{};
    /// Default implementation: the RHI viewport image lease taken from TakeFrame.
    graphics::renderer::ViewportFrame Lease{};
    /// Default implementation: the native read of that image on the viewport's device.
    graphics::rhi::NativeTextureRead Read{};
    /// Source-private identity for sources that do not use Lease and Read.
    foundation::uint64 Token = 0;
    /// Source-private slot for sources that do not use Lease and Read.
    foundation::uint32 Slot = 0;
};
/// Producer of the display images that a PreviewViewport presents.
///
/// The viewport owns presentation (its Qt Quick window, two frame slots and release after
/// `afterFrameEnd`, or the offscreen readback canvas). A source owns frame production, its
/// leases and its resource generations. `RenderPreview` is the in-process edit source; the
/// Play-in-viewport design adds a source for imported game images (see
/// docs/architecture/editor-gui-systems.md, "The hosted surface and frame ownership").
///
/// Device admission and camera navigation are not part of this contract: they stay with the
/// `RenderPreview` that owns the viewport's device. Images must live on that admitted device,
/// because the viewport imports or reads them through it.
///
/// All members are called on the GUI thread, which also owns the RHI device.
class ViewportFrameSource : public QObject
{
    Q_OBJECT
public:
    /// @param parentOrNull Optional Qt owner of this source.
    explicit ViewportFrameSource(QObject* parentOrNull = nullptr);
    ~ViewportFrameSource() override;
    /// Whether a completed image is ready for TakeFrame.
    [[nodiscard]] virtual bool HasPendingFrame() const noexcept = 0;
    /// Lease the pending image to the caller.
    /// @param outFrame Receives the lease; release it exactly once through ReleaseFrame.
    /// @param outImage Receives the borrowed image, valid until outFrame is released. Its producer
    /// work has completed; the caller completes its own sampling before releasing.
    /// @return False when no image is pending; both outputs are then unchanged.
    [[nodiscard]] virtual bool TakeFrame(graphics::renderer::ViewportFrame& outFrame,
                                         graphics::renderer::ViewportImage& outImage) noexcept = 0;
    /// Return a lease from TakeFrame. A lease of a retired generation still reaches the
    /// generation that issued it; a lease the source already reclaimed is ignored.
    virtual void ReleaseFrame(graphics::renderer::ViewportFrame& inoutFrame) noexcept = 0;
    /// Open a read of the pending image for the Qt scene graph on the viewport's device.
    /// The default leases the image with TakeFrame and opens a queue-ordered native read of it.
    /// @param device Pinned context of the viewport's admitted device, borrowed for the call.
    /// @param consumer Exact native context that will sample the image (Qt's device and queue).
    /// @param outRead Receives the read on success; end it exactly once with EndFrameRead or
    /// DrainFrameRead. Unchanged on failure.
    /// @return Ready on success; NotReady when no image is pending; otherwise the read failure,
    /// after the source has already reclaimed the image.
    [[nodiscard]] virtual graphics::rhi::RasterStatus TakeFrameRead(graphics::rhi::NativeDeviceLease device,
                                                                    const graphics::rhi::NativeDeviceIdentity& consumer,
                                                                    ViewportFrameRead& outRead) noexcept;
    /// End a read after the Qt frame that sampled it has been submitted (`afterFrameEnd`).
    /// @return Ready once the source has its image back and inoutRead is cleared; Pending while
    /// the consumer queue has not yet finished sampling (call again later); otherwise a failure
    /// that preserves the read.
    [[nodiscard]] virtual graphics::rhi::RasterStatus EndFrameRead(ViewportFrameRead& inoutRead) noexcept;
    /// End a read during teardown, waiting for submitted sampling if necessary; never call while
    /// presenting. Clears inoutRead.
    virtual void DrainFrameRead(ViewportFrameRead& inoutRead) noexcept;
    /// Request images of this physical size in pixels for subsequent frames. A source may
    /// produce smaller images (RenderPreview caps edit previews at 1024 pixels) and ignores
    /// non-positive sizes.
    virtual void Resize(foundation::int32 width, foundation::int32 height) = 0;
Q_SIGNALS:
    /// A new image became pending.
    void FrameReady();
    /// The source is about to reclaim every outstanding lease and destroy its images.
    /// Receivers must finish sampling and release their leases before returning.
    void ResettingFrames();
};
} // namespace ludus::editor
