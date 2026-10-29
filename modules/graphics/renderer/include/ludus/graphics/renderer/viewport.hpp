#pragma once

#include <ludus/foundation/base/pointer.hpp>

#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport_frame.hpp>

namespace ludus::graphics::renderer
{
/// Linear scene attachment policy; display images always remain encoded RGBA8.
enum class ViewportColorMode : ludus::foundation::uint8
{
    /// Prefer HDR when enabled, otherwise use linear RGBA8. Lit materials support both.
    PreferHdr,
    /// Use linear RGBA8 for unlit textured material programs, which do not support HDR targets.
    Linear8,
};
/// Interactive raster output for the current extracted World snapshot and camera.
/// Uses ordinary PrepareView/DrawView and authored material lighting, with no
/// compute/ray capability, reference capture, CPU pixel readback or accumulation.
/// Raster lighting does not infer indirect illumination from emissive geometry.
/// All methods run on the RHI owner thread, between frames. The Renderer and
/// device must outlive this object, and Renderer::Reset must follow Reset here.
/// Qt import requires the exact logical device and a separate RHI interop lease;
/// complete foreign sampling and release that lease before Release(frame).
class RasterViewport final
{
public:
    /// Construct an empty viewport; performs no GPU work.
    RasterViewport() noexcept;
    /// Reset owned resources; see Reset's consumer lifetime precondition.
    ~RasterViewport() noexcept;
    /// Prepare a linear scene target, two display targets and their presentation.
    /// Renderer lighting/views must be Ready. Positive dimensions are physical
    /// pixels; exposure follows LightingOutput. Output is tone-mapped/sRGB-encoded RGB;
    /// the consumer preserves the display values without a second encoding. Ready/Pending owns setup.
    /// Color mode chooses the linear scene target; PreferHdr cannot draw unlit textured
    /// material programs into an HDR attachment. Use Linear8 for those snapshots.
    /// @param renderer Borrowed initialized renderer, which must outlive this viewport.
    /// @param device Borrowed device used by renderer, on its owner thread.
    /// @param width Positive output width in physical pixels.
    /// @param height Positive output height in physical pixels.
    /// @param exposure Finite exposure multiplier admitted by LightingOutput.
    /// @param colorMode Linear attachment policy for the selected material programs.
    /// @param shouldPresentToSurface Also composite the encoded display image to the acquired
    /// surface in the same submission, without CPU transfer or a second color encoding.
    /// The surface extent must equal width/height; default false leaves surface presentation to the consumer.
    /// @param toneCurve Display curve of the presentation. The Editor uses PbrNeutral; its debug shading views are
    /// pre-compensated for that curve (MaterialDebugView), so use another curve only without debug views.
    /// @param hasBackdrop Show the editor backdrop gradient behind uncovered scene pixels
    /// (LightingOutput::HasBackdrop); the scene pass then clears alpha to zero. Default false keeps the flat black
    /// background.
    /// @param antialiasing Spatial antialiasing of the display image (LightingOutput::Antialiasing). Default Off.
    /// Failure leaves this empty. Resize by preparing a separate candidate, then retiring this instance
    /// after its producers/consumers finish.
    [[nodiscard]] rhi::RasterStatus Initialize(Renderer& renderer,
                                               rhi::DeviceHandle device,
                                               ludus::foundation::uint32 width,
                                               ludus::foundation::uint32 height,
                                               ludus::foundation::float32 exposure = 1,
                                               ViewportColorMode colorMode = ViewportColorMode::PreferHdr,
                                               bool shouldPresentToSurface = false,
                                               ToneCurve toneCurve = ToneCurve::Reinhard,
                                               bool hasBackdrop = false,
                                               SpatialAntialiasing antialiasing = SpatialAntialiasing::Off) noexcept;
    /// Poll setup/producer completion and publish a pending view only when Ready.
    /// A failed candidate is discarded, preserving the prior drawable view.
    /// Returns that failure once; a later poll may use the preserved view.
    /// No waiting or GPU-memory readback.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Replace the current extracted snapshot/camera atomically. Snapshot contents
    /// are retained by a prepared view; the caller may release the snapshot after
    /// Ready/Pending. Pending retains the prior drawable view until Poll observes
    /// successful preparation; another accepted Update supersedes the pending candidate.
    /// Report describes the accepted candidate (including Pending), not the last
    /// presented image. Rejection preserves the prior view, pending candidate and
    /// report. No PT/BVH setup.
    [[nodiscard]] rhi::RasterStatus Update(Snapshot snapshot, const ViewDescription& view, ViewReport& report) noexcept;
    /// Compose prepared debug geometry into every later Render, before display conversion.
    /// The overlay draws into the linear scene target after the scene and depth-tests each
    /// command (OverlayDepth) against that frame's own scene depth, which the scene pass then
    /// keeps; OverlayDepth::Always draws through geometry. Overlay colors therefore pass through
    /// the same exposure and display encoding as the scene. Between frames only.
    /// @param overlaysOrNull Overlay renderer on this viewport's device, borrowed until replaced,
    /// cleared or Reset. Null clears the overlay, and overlay is then ignored.
    /// @param overlay A version of overlaysOrNull whose GetStatus is Ready. The caller keeps it
    /// unreleased while set; it may Release a replaced version once this call returns, because
    /// accepted frames retain their resources.
    /// @return Ready once set or cleared. InvalidState before Initialize or while this viewport's
    /// frame is open; the overlay's GetStatus failure (such as Pending) when not Ready; Unsupported
    /// when the scene target is not linear RGBA8 (an HDR attachment; initialize with
    /// ViewportColorMode::Linear8). Failure preserves the previous overlay.
    [[nodiscard]] rhi::RasterStatus SetOverlay(OverlayRenderer* overlaysOrNull, PreparedOverlay overlay) noexcept;
    /// Render the latest Ready view into an unleased display image. Closed frame
    /// required; completion starts null. NotReady applies producer/consumer
    /// backpressure. A failure may still publish a token for accepted partial work.
    /// If EndFrame reservation fails with a frame still open, this object retains
    /// recording ownership; retry Render before any other frame/setup/reset call.
    [[nodiscard]] rhi::RasterStatus Render(rhi::SurfaceHandle surface, rhi::SubmissionToken& completion) noexcept;
    /// Acquire the latest completed image. Null frame required. Ready publishes
    /// both outputs; rejection preserves them. Acquired images are never overwritten.
    [[nodiscard]] rhi::RasterStatus Acquire(ViewportFrame& frame, ViewportImage& image) noexcept;
    /// Return a lease after all foreign GPU consumers and RHI interop leases finish.
    /// Ready clears identity; stale/foreign rejection preserves it.
    [[nodiscard]] rhi::RasterStatus Release(ViewportFrame& frame) noexcept;
    /// Invalidate all identities and detach resources; accepted RHI work retires
    /// independently. External consumers must hold RHI interop backing leases.
    /// No open frame; Renderer/device must still be alive. No waiting/readback.
    void Reset() noexcept;

private:
    struct State;
    ludus::foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
