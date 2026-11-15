#pragma once

#include <ludus/foundation/base/types.h>

// Private context for a backend synchronization call made while an ordered graph lowers its
// uses. The encoder sets it immediately before ComputeBufferBarrier and RasterTextureBarrier;
// backends that record no observations or range-aware barriers ignore it. Owner thread only.
// See docs/architecture/rhi-gdi.md#synchronization-evidence.
namespace ludus::graphics::rhi::internal
{
/// Encoder boundary that requested a native synchronization operation.
enum class GraphLoweringBoundary : foundation::uint8
{
    /// The call did not come from ordered-graph lowering (direct draws, copies, presentation).
    None,
    /// Declared use before a live pass, or before the leading pass of a merged raster group.
    Pass,
    /// Restoration of a resource's final or role state after the last live pass.
    Final,
};
/// Graph context of the backend synchronization call being recorded.
struct GraphLoweringSite final
{
    /// Encoder boundary; None outside ordered-graph lowering.
    GraphLoweringBoundary Boundary = GraphLoweringBoundary::None;
    /// GPU scope identity of the pass (graph pass index plus one); zero outside a pass.
    foundation::uint64 Scope = 0;
    /// Generic frame submission being recorded; zero when unavailable.
    foundation::uint64 Submission = 0;
    /// Nonzero identity of the current graph execution; resets backend range snapshots.
    foundation::uint64 Execution = 0;
    /// Graph resource index plus one; zero outside a declared use.
    foundation::uint32 Resource = 0;
    /// Declared buffer use start in bytes; zero for textures and whole-resource restoration.
    foundation::usize Offset = 0;
    /// Declared buffer use length in bytes; zero means the whole resource.
    foundation::usize Size = 0;
};
/// Current lowering context. The encoder clears it when lowering ends or fails.
inline GraphLoweringSite gGraphLoweringSite;
/// Last assigned graph execution identity.
inline foundation::uint64 gGraphLoweringExecutions = 0;
/// Clears the lowering context on every exit from an ordered-graph lowering scope.
struct GraphLoweringSiteReset final
{
    GraphLoweringSiteReset() noexcept = default;
    GraphLoweringSiteReset(const GraphLoweringSiteReset&) = delete;
    GraphLoweringSiteReset& operator=(const GraphLoweringSiteReset&) = delete;
    ~GraphLoweringSiteReset() noexcept
    {
        gGraphLoweringSite = {};
    }
};
} // namespace ludus::graphics::rhi::internal
