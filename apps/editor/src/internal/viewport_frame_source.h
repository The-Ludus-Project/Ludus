#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/viewport.hpp>

#include <QObject>

namespace ludus::editor
{
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
