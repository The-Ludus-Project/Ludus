#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/raster.h>

#include <span>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
/// Bound flat program uniform layout: read-only uniform buffer bindings merged from
/// the vertex and fragment stages, each placed at an aligned offset of one buffer.
struct FlatUniformLayout final
{
    /// Ascending binding numbers; MinSize is the stage range rounded up to UNIFORM_RANGE_GRANULE.
    // Every element uses RasterBinding's valid Visibility::Both default member initializer.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    StaticArray<rhi::RasterBinding, rhi::RASTER_BINDING_COUNT> Bindings{};
    StaticArray<usize, rhi::RASTER_BINDING_COUNT> Offsets{}; ///< Aligned buffer offset of each binding.
    StaticArray<usize, rhi::RASTER_BINDING_COUNT> Sizes{};   ///< Caller block bytes: the declared stage MinSize.
    usize Count = 0;                                         ///< Number of bindings; zero for an unbound flat program.
    usize CallerBytes = 0;                                   ///< Sum of Sizes, the caller's concatenated block bytes.
    usize BufferBytes = 0;                                   ///< Aligned buffer size holding every block.
    rhi::BindingLayoutHandle Handle{};                       ///< Renderer-owned layout shared by every flat pipeline.
};
/// RHI uniform binding ranges are multiples of 16 bytes (the std140/WGSL uniform struct granule).
inline constexpr usize UNIFORM_RANGE_GRANULE = 16;
/// Merge stage bindings into a flat layout. Accepts only constant, scalar, non-dynamic
/// uniform buffers; a binding number declared by both stages must have an equal MinSize.
/// @param alignment Device uniform offset alignment, positive power of two.
/// @param maxRange Device maximum bound uniform range.
/// @return False for any other binding, an empty or oversized range or a conflict; output preserved.
[[nodiscard]] bool MergeFlatUniformLayout(std::span<const rhi::RasterBinding> vertex,
                                          std::span<const rhi::RasterBinding> fragment,
                                          usize alignment,
                                          usize maxRange,
                                          FlatUniformLayout& outLayout) noexcept;
/// Create a binding snapshot on layout.Handle that reads every block of buffer.
[[nodiscard]] rhi::RasterStatus CreateFlatUniformBindings(rhi::DeviceHandle device,
                                                          const FlatUniformLayout& layout,
                                                          rhi::BufferHandle buffer,
                                                          rhi::BindingSetHandle& outBindings) noexcept;
/// Slot and non-wrapping generation naming one live uniform set.
struct UniformSetIdentity final
{
    usize Slot = 0;        ///< Record index.
    uint64 Generation = 0; ///< Incarnation of the record at Slot.
};
/// Private bounded registry of immutable per-draw uniform sets for one Renderer.
/// Each set owns one uniform buffer and binding snapshot. Owned sets are detached by
/// the Renderer; snapshot references keep a detached set alive, and RHI retains
/// submitted GPU uses through completion. Owner-thread only; no allocation after Reserve.
class UniformSetTable final
{
public:
    /// Reserve record and staging storage once; failure preserves an empty table.
    [[nodiscard]] bool Reserve(usize capacity, usize stagingBytes) noexcept;
    /// Host bytes requested by Reserve for the given capacity and staging size.
    [[nodiscard]] static usize GetHostBytes(usize capacity, usize stagingBytes) noexcept;
    /// Copy caller blocks into a new buffer. bytes holds layout.CallerBytes, each binding's Sizes
    /// bytes in ascending binding order. Ready/Pending publishes outIdentity.
    [[nodiscard]] rhi::RasterStatus Create(rhi::DeviceHandle device,
                                           const FlatUniformLayout& layout,
                                           BorrowedView<const uint8> bytes,
                                           UniformSetIdentity& outIdentity) noexcept;
    /// Create the zero-filled default buffer of a bound program; the caller owns the buffer.
    [[nodiscard]] rhi::RasterStatus
    CreateDefault(rhi::DeviceHandle device, const FlatUniformLayout& layout, rhi::BufferHandle& outBuffer) noexcept;
    /// Advance buffer and binding creation without blocking.
    [[nodiscard]] rhi::RasterStatus
    GetStatus(rhi::DeviceHandle device, const FlatUniformLayout& layout, usize slot, uint64 generation) noexcept;
    /// Whether slot/generation names a live owned set.
    [[nodiscard]] bool IsOwned(usize slot, uint64 generation) const noexcept;
    /// Add one snapshot reference to a live set.
    void Retain(usize slot) noexcept;
    /// Remove one snapshot reference; destroys a detached unreferenced set.
    void Unretain(rhi::DeviceHandle device, usize slot) noexcept;
    /// Detach public ownership; earlier snapshot references keep the set alive.
    void Detach(rhi::DeviceHandle device, usize slot) noexcept;
    /// Binding snapshot of a Ready set.
    [[nodiscard]] rhi::BindingSetHandle GetBindings(usize slot) const noexcept;
    /// Destroy every set regardless of references; used when the Renderer resets.
    void Clear(rhi::DeviceHandle device) noexcept;

private:
    struct Record final
    {
        uint64 Generation = 1;
        uint32 References = 0;
        bool IsUsed = false;
        bool IsOwned = false;
        bool HasBindings = false;
        rhi::RasterStatus Setup = rhi::RasterStatus::Pending;
        rhi::BufferHandle Buffer{};
        rhi::BindingSetHandle Bindings{};
    };
    void dropIfUnused(rhi::DeviceHandle device, usize slot) noexcept;
    Array<Record> mRecords;
    Array<uint8> mStaging;
};
} // namespace ludus::graphics::renderer::internal
