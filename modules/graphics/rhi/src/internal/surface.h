#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/rhi.h>

// R14b multiple surfaces per device. The facade owns surface identities and slots;
// slot 0 is the primary surface CreateDevice publishes, and slots
// 1..AdditionalSurfaceLimit() are surfaces created afterwards. A backend keeps one
// presentation record per slot and routes SetTarget/Begin/End to the slot most recently
// passed to SelectSurface. See docs/development/fullscreen-rendering.md, "Multiple
// surfaces per device (R14b)".
namespace ludus::graphics::rhi::backend
{
/// Per-surface completion counters a backend observes without waiting.
struct SurfaceCompletion final
{
    /// Frames of this surface whose GPU work completed.
    ludus::foundation::uint64 RenderedFrames = 0;
    /// Frames of this surface handed to the platform presentation queue or hosted consumer.
    ludus::foundation::uint64 PresentedFrames = 0;
};
/// Surfaces the ready session can host besides its primary surface; zero when the backend
/// keeps a single surface. Never above MAX_DEVICE_SURFACES - 1.
[[nodiscard]] ludus::foundation::usize AdditionalSurfaceLimit() noexcept;
/// Connect a borrowed target as surface `slot` (1..AdditionalSurfaceLimit()) between frames.
/// @return None on success; InvalidWindow for an invalid target; BackendUnavailable when the
/// target cannot share the primary color format; SurfaceUnavailable when connection failed.
/// Failure leaves no presentation state in the slot.
[[nodiscard]] StartupError CreateSurface(ludus::foundation::usize slot, const WindowInfo& window) noexcept;
/// Wait for this slot's submitted frames, then release only its presentation resources.
/// Valid for any created slot, after its loss and after session failure. Never the primary.
void DestroySurface(ludus::foundation::usize slot) noexcept;
/// Route the following SetTarget, Begin, End and test readback to `slot`. Between frames only.
void SelectSurface(ludus::foundation::usize slot) noexcept;
/// Counters of one created or primary slot, refreshed without waiting.
[[nodiscard]] SurfaceCompletion GetSurfaceCompletion(ludus::foundation::usize slot) noexcept;
} // namespace ludus::graphics::rhi::backend

namespace ludus::graphics::rhi::internal
{
/// Called by a backend on the owner thread when a created surface (slot > 0) stops
/// accepting frames. Marks only that surface Lost; ignored for the primary or a free slot.
void LoseSurface(ludus::foundation::usize slot) noexcept;
/// True while a created surface is marked Lost.
[[nodiscard]] bool IsSurfaceLost(ludus::foundation::usize slot) noexcept;
/// SetFrameTarget for one surface slot; the slot-0 form is the public facade call.
[[nodiscard]] FrameStatus SetSurfaceFrameTarget(ludus::foundation::usize slot, const FrameTarget& target) noexcept;
/// BeginFrameStatus for one surface slot; the slot-0 form is the public facade call.
[[nodiscard]] FrameStatus BeginSurfaceFrame(ludus::foundation::usize slot) noexcept;
/// Slot whose frame is open; meaningful only while FrameOpen().
[[nodiscard]] ludus::foundation::usize GetFrameSurface() noexcept;
/// Clear a slot's frame counters and accepted target when a surface takes the slot.
void ResetSurfaceFrames(ludus::foundation::usize slot) noexcept;
/// Frames submitted on a slot since its surface was created.
[[nodiscard]] ludus::foundation::uint64 GetSurfaceSubmittedFrames(ludus::foundation::usize slot) noexcept;
} // namespace ludus::graphics::rhi::internal
