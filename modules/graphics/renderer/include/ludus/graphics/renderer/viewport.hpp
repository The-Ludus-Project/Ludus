#pragma once

#include <ludus/foundation/base/pointer.hpp>

#include <ludus/graphics/renderer/renderer.hpp>

namespace ludus::graphics::renderer
{
/// One immutable display image lease. Copies borrow; release the original once.
struct ViewportFrame final
{
private:
    ludus::foundation::uint64 Owner = 0, Generation = 0;
    ludus::foundation::uint32 Slot = 0;
    friend class RasterViewport; ///< Validates the private lease identity.
};
/// Borrowed GPU display image, valid until its ViewportFrame is released.
struct ViewportImage final
{
    rhi::TextureHandle Texture{};         ///< Read-only sRGB-encoded, tone-mapped RGBA8Unorm image; never destroy it.
    rhi::SubmissionToken Producer{};      ///< Completed producer work; consumer completion is separate.
    ludus::foundation::uint32 Width = 0;  ///< Physical width in pixels, top-left origin.
    ludus::foundation::uint32 Height = 0; ///< Physical height in pixels.
};
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
    /// Failure leaves this empty. Resize by preparing a separate candidate, then
    /// retiring this instance after its producers/consumers finish.
    [[nodiscard]] rhi::RasterStatus Initialize(Renderer& renderer,
                                               rhi::DeviceHandle device,
                                               ludus::foundation::uint32 width,
                                               ludus::foundation::uint32 height,
                                               ludus::foundation::float32 exposure = 1,
                                               ViewportColorMode colorMode = ViewportColorMode::PreferHdr,
                                               bool shouldPresentToSurface = false) noexcept;
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
