#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/graphics/renderer/static_lighting.hpp>

#include "internal/render_scene.h"

namespace ludus::editor
{
// Editor CPU budget for the static-lighting bake, excluding the UI thread, which
// never helps. It leaves cores for Qt, RHI submission and the render load thread.
inline constexpr foundation::uint32 STATIC_LIGHTING_WORKER_COUNT = 3;
// Ranges per bake: several per worker so uneven triangle cost balances.
inline constexpr foundation::uint32 STATIC_LIGHTING_RANGE_COUNT = 64;

// CPU preparation and transport on a caller-owned JobSystem; no GPU or QWidget
// access. Immutable decoded storage stays alive through the retained RenderScene.
// Every call, including destruction, belongs to the thread that owns the system.
class StaticLightingJob final
{
public:
    StaticLightingJob(const RenderScene& scene, const graphics::renderer::StaticLightingBakeDescription& description);
    // Cancels and drains an in-flight bake; blocks until running ranges observe it.
    ~StaticLightingJob() noexcept;
    StaticLightingJob(const StaticLightingJob&) = delete;
    StaticLightingJob& operator=(const StaticLightingJob&) = delete;
    // Pending once submitted; the system must outlive this job.
    [[nodiscard]] graphics::rhi::RasterStatus Start(foundation::threading::JobSystem& inoutSystem) noexcept;
    // Nonblocking; Pending until every range resolves, then the merged final status.
    [[nodiscard]] graphics::rhi::RasterStatus Poll() noexcept;
    // Blocking form of Poll.
    [[nodiscard]] graphics::rhi::RasterStatus Wait() noexcept;
    void RequestInterruption() noexcept;
    [[nodiscard]] foundation::uint32 GetProgressPercent() const noexcept;
    // Tolerated input problems, Ready once preparation succeeded (see StaticLightingBake::GetDiagnostics).
    [[nodiscard]] graphics::rhi::RasterStatus
    GetDiagnostics(graphics::renderer::StaticLightingBakeDiagnostics& outDiagnostics) const noexcept;
    // Zero-area triangles skipped in one placed object, under the same conditions.
    [[nodiscard]] graphics::rhi::RasterStatus GetSkippedTriangleCount(foundation::usize objectIndex,
                                                                      foundation::uint32& outCount) const noexcept;
    // Call only after Poll or Wait returned Ready.
    [[nodiscard]] foundation::UniquePtr<graphics::renderer::StaticLightingBake> TakeBake() noexcept;

private:
    RenderScene mScene;
    graphics::renderer::StaticLightingBakeDescription mDescription;
    foundation::UniquePtr<graphics::renderer::StaticLightingBake> mBake;
    foundation::Array<graphics::renderer::StaticLightingSurfaceDescription> mSurfaces;
    foundation::Array<foundation::uint64> mAnalyticLightIds;
    graphics::rhi::RasterStatus mStatus = graphics::rhi::RasterStatus::Pending;
    bool mHasStarted = false;
};
} // namespace ludus::editor
