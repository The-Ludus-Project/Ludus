#pragma once
#include "render_items.h"
#include <ludus/foundation/base/core.h>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
namespace ludus::world_demo
{
/// Asynchronous Renderer setup, advanced at most one stage per Prepare call without waiting.
enum class FrameRendererStage : uint8
{
    /// Nothing is initialized; the next Prepare starts the flat (L0) programs.
    Idle,
    /// Flat programs are being created.
    Flat,
    /// The reverse-Z views (L1) profile is being created.
    Views,
    /// The lighting output profile (tone mapping and display encoding) is being created.
    Lighting,
    /// The shared unit quad is being uploaded.
    Mesh,
    /// Frames can be updated and presented.
    Ready,
};
/// Presents World-demo frames on one explicit device through the engine Renderer's
/// RasterViewport, the same interactive output the Editor's World viewport uses.
///
/// Each frame is drawn as linear colour, tone-mapped and sRGB-encoded once by the
/// Renderer's lighting output, then composited to the acquired surface. Edit and play
/// therefore show the same pixels. Each Update replaces the viewport's view; the last
/// drawable view keeps presenting until a replacement is Ready, so a Pending upload never
/// blanks the window. One owner thread (the device's), between frames. The device must
/// outlive this object.
class FrameRenderer final
{
public:
    /// Construct an empty renderer; performs no GPU work.
    FrameRenderer() noexcept = default;
    /// Release renderer ownership; call Reset first while the device is still alive.
    ~FrameRenderer() noexcept = default;
    /// The owned Renderer cannot be copied.
    FrameRenderer(const FrameRenderer&) = delete;
    /// The owned Renderer cannot be assigned.
    FrameRenderer& operator=(const FrameRenderer&) = delete;
    /// Advance setup by at most one stage.
    /// @param device Ready device whose renderer this becomes; the same device on every call.
    /// @return Ready once frames can be presented, Pending while setup continues, or the
    /// Renderer's failure, after which only Reset recovers.
    [[nodiscard]] graphics::rhi::RasterStatus Prepare(graphics::rhi::DeviceHandle device) noexcept;
    /// Replace the presented scene with one frame, viewed for a drawable of the given size.
    /// A size change recreates the viewport's targets, so the next frames wait for the new view.
    /// @param frame Extraction to draw; copied into a renderer snapshot before return.
    /// @param width Drawable width in physical pixels; must equal the surface target width.
    /// @param height Drawable height in physical pixels; must equal the surface target height.
    /// @return Ready when the frame's view was accepted (it may still be Pending behind the
    /// last drawable view), NotReady when it was skipped (zero extent, setup or a retained
    /// snapshot slot) and the previous view stays, or a failure for a frame the builders or
    /// Renderer reject. Every outcome except a resize preserves the last drawable view.
    [[nodiscard]] graphics::rhi::RasterStatus Update(const RenderFrame& frame, uint32 width, uint32 height) noexcept;
    /// Draw the latest drawable view and present it to the surface in one submission.
    /// Opens and ends the device frame itself; set the surface's frame target first.
    /// @param surface Surface of the device passed to Prepare, sized like the last Update.
    /// @param outCompletion Null token receiving the accepted submission.
    /// @return Ready when a frame was presented; NotReady when there is no drawable view yet,
    /// the surface skipped acquisition or both display images are leased; otherwise
    /// RasterViewport::Render's failure.
    [[nodiscard]] graphics::rhi::RasterStatus Present(graphics::rhi::SurfaceHandle surface,
                                                      graphics::rhi::SubmissionToken& outCompletion) noexcept;
    /// Lease the latest presented display image (tone-mapped, sRGB-encoded RGBA8) for reading.
    /// @return RasterViewport::Acquire's status; Ready fills both outputs.
    [[nodiscard]] graphics::rhi::RasterStatus Acquire(graphics::renderer::ViewportFrame& outFrame,
                                                      graphics::renderer::ViewportImage& outImage) noexcept;
    /// Return a lease from Acquire after every reader of its image has finished.
    [[nodiscard]] graphics::rhi::RasterStatus Release(graphics::renderer::ViewportFrame& inoutFrame) noexcept;
    /// Release every renderer handle and return to Idle. Requires a closed frame, no leased
    /// image and a live device.
    void Reset() noexcept;

private:
    [[nodiscard]] graphics::rhi::RasterStatus prepareViewport(uint32 width, uint32 height) noexcept;
    graphics::renderer::Renderer mRenderer;
    graphics::renderer::RasterViewport mViewport;
    graphics::renderer::Mesh mQuad;
    graphics::renderer::Snapshot mSnapshot;
    graphics::rhi::DeviceHandle mDevice;
    FrameItems mItems;
    FrameRendererStage mStage = FrameRendererStage::Idle;
    uint32 mViewportWidth = 0;
    uint32 mViewportHeight = 0;
    bool mHasSnapshot = false;
};
} // namespace ludus::world_demo
