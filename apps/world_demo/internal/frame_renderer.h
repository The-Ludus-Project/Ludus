#pragma once
#include "render_items.h"
#include <ludus/foundation/base/core.h>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
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
    /// The shared unit quad is being uploaded.
    Mesh,
    /// Frames can be updated and drawn.
    Ready,
};
/// Draws World-demo frames on one explicit device through the engine Renderer.
///
/// Each Update turns the latest extraction into a replacement snapshot and a prepared
/// candidate view. The last Ready view keeps drawing until that candidate is Ready, so a
/// Pending upload never blanks the window. One owner thread (the device's), between frames,
/// except Draw, which needs the caller's acquired frame. The device must outlive this object.
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
    /// @return Ready once frames can be drawn, Pending while setup continues, or the
    /// Renderer's failure, after which only Reset recovers.
    [[nodiscard]] graphics::rhi::RasterStatus Prepare(graphics::rhi::DeviceHandle device) noexcept;
    /// Replace the drawn scene with one frame, viewed for a drawable of the given size.
    /// @param frame Extraction to draw; copied into a renderer snapshot before return.
    /// @param width Drawable width in physical pixels.
    /// @param height Drawable height in physical pixels.
    /// @return Ready when the frame's view was accepted (it may still be Pending behind the
    /// last Ready view), NotReady when it was skipped (zero extent, or every snapshot or view
    /// slot is still retained) and the previous view stays, or a failure for a frame the
    /// builders or Renderer reject. Every outcome preserves the last Ready view.
    [[nodiscard]] graphics::rhi::RasterStatus Update(const RenderFrame& frame, uint32 width, uint32 height) noexcept;
    /// Whether a Ready view exists for Draw.
    [[nodiscard]] bool HasView() const noexcept
    {
        return mHasView;
    }
    /// Draw the latest Ready view inside the caller's acquired frame.
    /// Clears colour to FRAME_BACKGROUND and reverse-Z depth to zero over the whole target.
    /// @param target Linear RGBA8 attachment, or a null handle for the acquired surface.
    /// @return NotReady without a Ready view; otherwise Renderer::DrawView's status.
    [[nodiscard]] graphics::rhi::RasterStatus Draw(graphics::rhi::TextureHandle target) noexcept;
    /// Release every renderer handle and return to Idle. Requires a closed frame and a live device.
    void Reset() noexcept;

private:
    [[nodiscard]] graphics::rhi::RasterStatus promoteCandidate() noexcept;
    graphics::renderer::Renderer mRenderer;
    graphics::renderer::Mesh mQuad;
    graphics::renderer::Snapshot mSnapshot;
    /// Latest Ready view, drawn every frame until a newer candidate becomes Ready.
    graphics::renderer::PreparedView mView;
    /// View of a newer frame whose instance upload is still Pending.
    graphics::renderer::PreparedView mCandidate;
    FrameItems mItems;
    FrameRendererStage mStage = FrameRendererStage::Idle;
    bool mHasSnapshot = false;
    bool mHasView = false;
    bool mHasCandidate = false;
};
} // namespace ludus::world_demo
